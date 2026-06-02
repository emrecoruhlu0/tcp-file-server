#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <pthread.h>

// Sunucudaki ağaç derinliği sınırıyla uyumlu olmalı (server.c).
#define MAX_TREE_DEPTH 16

// Bağlantı zaman aşımı (saniye). GUI'nin uzun süre donmasını engeller.
#define CONNECT_TIMEOUT_SEC 5

// Non-blocking connect + select ile zaman aşımlı bağlanma.
// Başarıda 0, hata/zaman aşımında -1 döner. Soket başarıda blocking moda geri alınır.
static int connect_with_timeout(int sock, struct sockaddr_in *sa, int timeout_sec) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(sock, (struct sockaddr *)sa, sizeof(*sa));
    if (rc == 0) {
        // Anında bağlandı (genelde localhost).
        fcntl(sock, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) return -1;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };

    rc = select(sock + 1, NULL, &wset, NULL, &tv);
    if (rc <= 0) return -1; // 0 = zaman aşımı, <0 = hata

    // connect sonucunu SO_ERROR ile kontrol et.
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) return -1;
    if (so_error != 0) return -1;

    fcntl(sock, F_SETFL, flags); // blocking moda geri dön
    return 0;
}

int client_sock = -1;

GtkWidget *main_window;
GtkWidget *ip_entry;
GtkWidget *port_spin;
GtkWidget *connect_btn;
GtkWidget *disconnect_btn;
GtkWidget *status_label;
GtkWidget *tree_view;
GtkTreeStore *tree_store;
GtkWidget *progress_bar;   // Transfer ilerleme çubuğu
GtkWidget *get_btn;        // GET butonu (transfer sırasında pasifleştirilir)
GtkWidget *put_btn;        // PUT butonu

// Aynı anda yalnızca bir transfer çalışsın diye basit bayrak (GUI thread'i okur/yazar).
int transfer_active = 0;

// ---- Worker thread <-> GUI thread iletişimi ----
// GTK thread-safe değildir; worker thread GUI'ye yalnızca g_idle_add ile dokunur.

// Progress güncellemesi için ana thread'e geçirilen veri.
typedef struct {
    double fraction;   // 0.0 - 1.0
    char   text[128];  // çubuk üstündeki metin
} ProgressUpdate;

// Transfer bittiğinde ana thread'e geçirilen sonuç.
typedef struct {
    int  success;          // 1 = başarılı
    char message[320];     // dialogda gösterilecek mesaj (çağıranların m[300]'ünü taşırmadan alır)
} TransferResult;

// Worker thread'e geçirilen iş tanımı (GET veya PUT).
typedef struct {
    int   is_put;              // 1 = PUT, 0 = GET
    char  remote_path[1024];   // sunucudaki yol (GET) ya da yüklenecek dosya adı (PUT)
    char  local_path[1024];    // yerel kayıt yolu (GET) ya da kaynak dosya (PUT)
    long  filesize;            // PUT için boyut
} TransferJob;

// Ana thread'te çalışır (g_idle_add ile). Progress çubuğunu günceller.
static gboolean apply_progress(gpointer data) {
    ProgressUpdate *u = (ProgressUpdate *)data;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), u->fraction);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), u->text);
    g_free(u);
    return G_SOURCE_REMOVE;
}

// Ana thread'te çalışır. Transfer sonucunu gösterir, butonları geri açar.
static gboolean apply_result(gpointer data) {
    TransferResult *r = (TransferResult *)data;
    transfer_active = 0;
    gtk_widget_set_sensitive(get_btn, TRUE);
    gtk_widget_set_sensitive(put_btn, TRUE);
    gtk_widget_set_sensitive(disconnect_btn, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), r->success ? 1.0 : 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), r->success ? "Tamamlandı" : "Başarısız");

    GtkWidget *md = gtk_message_dialog_new(
        GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
        r->success ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK, "%s", r->message);
    gtk_dialog_run(GTK_DIALOG(md));
    gtk_widget_destroy(md);
    g_free(r);
    return G_SOURCE_REMOVE;
}

// Worker'dan ana thread'e ilerleme bildirimi gönderir.
static void post_progress(double fraction, const char *text) {
    ProgressUpdate *u = g_new0(ProgressUpdate, 1);
    u->fraction = fraction;
    snprintf(u->text, sizeof(u->text), "%s", text);
    g_idle_add(apply_progress, u);
}

// Worker'dan ana thread'e sonuç gönderir.
static void post_result(int success, const char *msg) {
    TransferResult *r = g_new0(TransferResult, 1);
    r->success = success;
    snprintf(r->message, sizeof(r->message), "%s", msg);
    g_idle_add(apply_result, r);
}

// Satır okuma yardımcı fonksiyonu (Komut yanıtlarını güvenle almak için)
int recv_line(int sock, char *buf, int maxlen) {
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        int r = recv(sock, &c, 1, 0);
        if (r <= 0) return (i == 0 ? r : i);
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i;
}

// Byte sayısını insan-okunur birime çevirir (B / KB / MB / GB).
static void human_size(double bytes, char *out, size_t outlen) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; i++; }
    snprintf(out, outlen, "%.1f %s", bytes, units[i]);
}

// İlerleme metnini "İndiriliyor: %45 — 3.2 MB/s — ~12 sn" formatında hazırlar.
// elapsed_sec: transfer başından beri geçen süre; speed = done/elapsed.
static void format_progress(char *out, size_t outlen, const char *verb,
                            long done, long total, double elapsed_sec) {
    double frac = (total > 0) ? (double)done / (double)total : 1.0;
    double speed = (elapsed_sec > 0.0) ? (double)done / elapsed_sec : 0.0; // byte/s
    char spd[32];
    human_size(speed, spd, sizeof(spd));

    if (speed > 0.0 && total > done) {
        double eta = (double)(total - done) / speed; // saniye
        if (eta < 1.0) eta = 1.0;
        snprintf(out, outlen, "%s %.0f%% — %s/s — ~%.0f sn", verb, frac * 100.0, spd, eta);
    } else {
        snprintf(out, outlen, "%s %.0f%% — %s/s", verb, frac * 100.0, spd);
    }
}

// Transferi gerçekleştiren worker thread. GUI'ye yalnızca post_progress/post_result
// üzerinden (g_idle_add) dokunur; doğrudan GTK çağrısı yapmaz.
static void *transfer_worker(void *arg) {
    TransferJob *job = (TransferJob *)arg;
    char cmd[2200];
    char buf[4096];

    if (!job->is_put) {
        // ---- GET ----
        snprintf(cmd, sizeof(cmd), "GET %s\n", job->remote_path);
        send(client_sock, cmd, strlen(cmd), 0);

        char resp[128];
        if (recv_line(client_sock, resp, sizeof(resp)) <= 0) {
            post_result(0, "Sunucudan yanıt alınamadı.");
            free(job);
            return NULL;
        }
        if (strncmp(resp, "OK ", 3) != 0) {
            char m[300];
            snprintf(m, sizeof(m), "İndirme başarısız: %s", resp);
            post_result(0, m);
            free(job);
            return NULL;
        }

        long size = atol(resp + 3);
        FILE *f = fopen(job->local_path, "wb");
        if (!f) {
            post_result(0, "Yerel dosya oluşturulamadı.");
            free(job);
            return NULL;
        }

        long total = 0;
        gint64 start_us = g_get_monotonic_time();
        while (total < size) {
            long remaining = size - total;
            size_t to_recv = (remaining < (long)sizeof(buf)) ? (size_t)remaining : sizeof(buf);
            int rx = recv(client_sock, buf, to_recv, 0);
            if (rx <= 0) break;
            fwrite(buf, 1, rx, f);
            total += rx;

            double frac = (size > 0) ? (double)total / (double)size : 1.0;
            double elapsed = (g_get_monotonic_time() - start_us) / 1e6;
            char ptext[128];
            format_progress(ptext, sizeof(ptext), "İndiriliyor", total, size, elapsed);
            post_progress(frac, ptext);
        }
        fclose(f);

        if (total == size) {
            post_result(1, "Dosya başarıyla indirildi.");
        } else {
            post_result(0, "İndirme yarıda kesildi (bağlantı koptu).");
        }
    } else {
        // ---- PUT ----
        snprintf(cmd, sizeof(cmd), "PUT %s %ld\n", job->remote_path, job->filesize);
        send(client_sock, cmd, strlen(cmd), 0);

        char resp[128];
        if (recv_line(client_sock, resp, sizeof(resp)) <= 0 || strncmp(resp, "OK", 2) != 0) {
            char m[300];
            snprintf(m, sizeof(m), "Sunucu yüklemeyi reddetti: %s",
                     resp[0] ? resp : "(yanıt yok)");
            post_result(0, m);
            free(job);
            return NULL;
        }

        FILE *f = fopen(job->local_path, "rb");
        if (!f) {
            post_result(0, "Kaynak dosya açılamadı.");
            free(job);
            return NULL;
        }

        long total = 0;
        size_t r;
        gint64 start_us = g_get_monotonic_time();
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            ssize_t s = send(client_sock, buf, r, 0);
            if (s <= 0) break;
            total += s;

            double frac = (job->filesize > 0) ? (double)total / (double)job->filesize : 1.0;
            double elapsed = (g_get_monotonic_time() - start_us) / 1e6;
            char ptext[128];
            format_progress(ptext, sizeof(ptext), "Yükleniyor", total, job->filesize, elapsed);
            post_progress(frac, ptext);
        }
        fclose(f);

        if (total == job->filesize) {
            post_result(1, "Dosya başarıyla yüklendi.");
        } else {
            post_result(0, "Yükleme yarıda kesildi (bağlantı koptu).");
        }
    }

    free(job);
    return NULL;
}

// Bir transfer işini başlatır: butonları kilitler, worker thread'i ayırır.
static void start_transfer(TransferJob *job) {
    transfer_active = 1;
    gtk_widget_set_sensitive(get_btn, FALSE);
    gtk_widget_set_sensitive(put_btn, FALSE);
    gtk_widget_set_sensitive(disconnect_btn, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Başlatılıyor...");

    pthread_t tid;
    if (pthread_create(&tid, NULL, transfer_worker, job) != 0) {
        transfer_active = 0;
        gtk_widget_set_sensitive(get_btn, TRUE);
        gtk_widget_set_sensitive(put_btn, TRUE);
        gtk_widget_set_sensitive(disconnect_btn, TRUE);
        free(job);
        post_result(0, "Transfer thread'i oluşturulamadı.");
        return;
    }
    pthread_detach(tid);
}

void on_connect_clicked(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    const char *ip = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_spin));
    
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    if (inet_pton(AF_INET, ip, &(sa.sin_addr)) != 1) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#c81e1e' weight='bold'>HATA: Geçersiz IPv4 Adresi!</span>");
        return;
    }
    
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#b45309' weight='bold'>⏳ Bağlanılıyor…</span>");
    // GUI'nin yanıt vermez görünmemesi için etiketi hemen çiz.
    while (gtk_events_pending()) gtk_main_iteration();

    if (connect_with_timeout(client_sock, &sa, CONNECT_TIMEOUT_SEC) < 0) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#c81e1e' weight='bold'>Bağlantı hatası! Sunucu açık mı? (zaman aşımı)</span>");
        close(client_sock);
        client_sock = -1;
        return;
    }
    
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#15803d' weight='bold'>✓ Bağlanıldı — dosyalar getiriliyor…</span>");
    gtk_widget_set_sensitive(connect_btn, FALSE);
    gtk_widget_set_sensitive(ip_entry, FALSE);
    gtk_widget_set_sensitive(port_spin, FALSE);
    gtk_widget_set_sensitive(disconnect_btn, TRUE);
    
    send(client_sock, "LIST\n", 5, 0);

    gtk_tree_store_clear(tree_store);

    // Liste boyutunu önceden bilmiyoruz; gerektiğinde büyüyen dinamik buffer kullan.
    size_t cap = 8192;
    size_t used = 0;
    char *list_data = malloc(cap);
    if (!list_data) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#c81e1e' weight='bold'>Bellek hatasi</span>");
        return;
    }
    list_data[0] = '\0';
    char recv_buf[4096];

    while(1) {
        int r = recv(client_sock, recv_buf, sizeof(recv_buf), 0);
        if (r <= 0) break;
        if (used + r + 1 > cap) {
            while (used + r + 1 > cap) cap *= 2;
            char *tmp = realloc(list_data, cap);
            if (!tmp) { free(list_data); return; }
            list_data = tmp;
        }
        memcpy(list_data + used, recv_buf, r);
        used += r;
        list_data[used] = '\0';
        if (strstr(list_data, "__END_OF_LIST__\n")) {
            break;
        }
    }

    char *saveptr1;
    char *line = strtok_r(list_data, "\n", &saveptr1);
    // Sunucudaki MAX_TREE_DEPTH=16 ile uyumlu; +1 emniyet payı.
    GtkTreeIter iters[MAX_TREE_DEPTH + 1];

    while (line) {
        if (strcmp(line, "__END_OF_LIST__") == 0) break;

        char *saveptr2;
        char *p_depth = strtok_r(line, "|", &saveptr2);
        char *p_ticked = strtok_r(NULL, "|", &saveptr2);
        char *p_dir = strtok_r(NULL, "|", &saveptr2);
        char *p_name = strtok_r(NULL, "|", &saveptr2);
        char *p_path = strtok_r(NULL, "", &saveptr2);

        if (p_path && p_depth && p_ticked && p_name) {
            int depth = atoi(p_depth);
            int is_ticked = atoi(p_ticked);

            // Bozuk/aşırı derin satırlara karşı sınır kontrolü (taşma önleme).
            if (depth < 0 || depth > MAX_TREE_DEPTH) {
                line = strtok_r(NULL, "\n", &saveptr1);
                continue;
            }

            GtkTreeIter *parent = (depth == 0) ? NULL : &iters[depth - 1];
            gtk_tree_store_append(tree_store, &iters[depth], parent);
            
            const char *color = is_ticked ? "black" : "red";
            // p_name zaten sunucu tarafında 📁/📄 ikonuyla geliyor.
            (void)p_dir;
            char display_name[512];
            if (is_ticked) {
                snprintf(display_name, sizeof(display_name), "%s", p_name);
            } else {
                snprintf(display_name, sizeof(display_name), "%s (Erişim Engellendi)", p_name);
            }
            
            gtk_tree_store_set(tree_store, &iters[depth], 0, display_name, 1, color, 2, p_path, -1);
        }
        
        line = strtok_r(NULL, "\n", &saveptr1);
    }
    free(list_data);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_view));
}

void on_disconnect_clicked(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    if (client_sock != -1) {
        close(client_sock);
        client_sock = -1;
    }
    gtk_widget_set_sensitive(connect_btn, TRUE);
    gtk_widget_set_sensitive(ip_entry, TRUE);
    gtk_widget_set_sensitive(port_spin, TRUE);
    gtk_widget_set_sensitive(disconnect_btn, FALSE);
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#475569' weight='bold'>● Bağlantı kesildi.</span>");
    gtk_tree_store_clear(tree_store);
}

void on_get_clicked(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    if (client_sock == -1) return;
    if (transfer_active) return; // Aynı anda tek transfer

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *display_name;
        gchar *color;
        gchar *full_path;
        gtk_tree_model_get(model, &iter, 0, &display_name, 1, &color, 2, &full_path, -1);
        
        if (strcmp(color, "red") == 0) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Bu dosyayı indirme yetkiniz yok.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            goto cleanup;
        }
        
        if (strstr(display_name, "📁") != NULL) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Klasörler indirilemez, sadece dosyaları seçin.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            goto cleanup;
        }
        
        GtkWidget *dialog = gtk_file_chooser_dialog_new("Dosyayı Kaydet", GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_SAVE, "İptal", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
        char *base = strrchr(full_path, '/');
        if (base) gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), base + 1);
        else gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), full_path);
        
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char *save_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

            // Asıl transferi worker thread'e devret; GUI donmaz, progress çubuğu güncellenir.
            TransferJob *job = calloc(1, sizeof(TransferJob));
            if (job) {
                job->is_put = 0;
                snprintf(job->remote_path, sizeof(job->remote_path), "%s", full_path);
                snprintf(job->local_path, sizeof(job->local_path), "%s", save_filename);
                start_transfer(job);
            }
            g_free(save_filename);
        }
        gtk_widget_destroy(dialog);
        
cleanup:
        g_free(display_name);
        g_free(color);
        g_free(full_path);
    }
}

void on_put_clicked(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    if (client_sock == -1) return;
    if (transfer_active) return; // Aynı anda tek transfer

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Yüklenecek Dosyayı Seç", GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_OPEN, "İptal", GTK_RESPONSE_CANCEL, "Aç", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *filename = strrchr(filepath, '/');
        if (filename) filename++; else filename = filepath;

        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
            // Transferi worker thread'e devret (progress + donma yok).
            TransferJob *job = calloc(1, sizeof(TransferJob));
            if (job) {
                job->is_put   = 1;
                job->filesize = st.st_size;
                snprintf(job->remote_path, sizeof(job->remote_path), "%s", filename);
                snprintf(job->local_path, sizeof(job->local_path), "%s", filepath);
                start_transfer(job);
            }
        } else {
            GtkWidget *md = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Geçersiz dosya seçimi.");
            gtk_dialog_run(GTK_DIALOG(md));
            gtk_widget_destroy(md);
        }
        g_free(filepath);
    }
    gtk_widget_destroy(dialog);
}

// Uygulamaya modern bir görünüm kazandıran CSS temasını yükler.
static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        /* Genel: açık arka plan + koyu metin (sistem teması koyu olsa bile sabit) */
        "window { background-color: #f4f6f8; color: #1f2933; }"
        "label { color: #1f2933; }"
        "entry, spinbutton, spinbutton entry {"
        "  color: #1f2933; background-color: #ffffff;"
        "}"
        "entry placeholder, entry:disabled { color: #6b7280; }"
        "button {"
        "  padding: 8px 14px;"
        "  border-radius: 6px;"
        "  font-weight: bold;"
        "  color: #1f2933;"
        "  border: 1px solid #94a3b8;"
        "  background-image: none;"
        "  background-color: #ffffff;"
        "  transition: background-color 120ms ease, box-shadow 120ms ease;"
        "}"
        "button:hover { background-image: none; background-color: #e8eef6; }"
        "button:active { background-image: none; background-color: #d4e0ef; }"
        /* Devre dışı: metin/zemin arası kontrast WCAG AA üstü kalsın */
        "button:disabled { color: #64748b; background-color: #e5e8ec; border-color: #cbd5e1; }"
        /* Birincil aksiyon (Bağlan / İndir) */
        "button.primary {"
        "  background-image: none; background-color: #1d4ed8;"
        "  color: #ffffff; border: 1px solid #1e40af;"
        "}"
        "button.primary:hover { background-color: #1e40af; }"
        "button.primary:active { background-color: #1e3a8a; }"
        "button.primary:disabled { background-color: #93acdf; color: #f8fafc; border-color: #93acdf; }"
        /* İkincil/yükle aksiyonu */
        "button.accent {"
        "  background-image: none; background-color: #0f766e;"
        "  color: #ffffff; border: 1px solid #115e59;"
        "}"
        "button.accent:hover { background-color: #115e59; }"
        "button.accent:disabled { background-color: #8fc4bf; color: #f8fafc; border-color: #8fc4bf; }"
        /* Tehlikeli aksiyon (Kopar) */
        "button.danger {"
        "  background-image: none; background-color: #dc2626;"
        "  color: #ffffff; border: 1px solid #b91c1c;"
        "}"
        "button.danger:hover { background-color: #b91c1c; }"
        "button.danger:disabled { background-color: #e89a9a; color: #f8fafc; border-color: #e89a9a; }"
        "entry { border-radius: 6px; padding: 4px 8px; border: 1px solid #94a3b8; }"
        "entry:focus { border-color: #1d4ed8; box-shadow: 0 0 0 2px #93acdf; }"
        "spinbutton:focus { border-color: #1d4ed8; }"
        "progressbar > trough { min-height: 18px; border-radius: 6px; background-color: #d4e0ef; }"
        "progressbar > trough > progress {"
        "  background-image: none;"
        "  background-color: #15803d;"
        "  border-radius: 6px;"
        "}"
        "progressbar text { font-weight: bold; color: #1f2933; }"
        "treeview { font-size: 11pt; background-color: #ffffff; color: #1f2933; }"
        "treeview:selected { background-color: #1d4ed8; color: #ffffff; }"
        "treeview header button { font-weight: bold; color: #1f2933; background-color: #e2e8f0; }"
        "label.section { font-weight: bold; font-size: 11pt; color: #1f2933; }";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "TCP İstemci Arayüzü");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 500, 500);
    gtk_container_set_border_width(GTK_CONTAINER(main_window), 15);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);
    
    GtkWidget *top_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), top_hbox, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(top_hbox), gtk_label_new("Sunucu IP:"), FALSE, FALSE, 0);
    ip_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ip_entry), "Örn: 192.168.1.5");
    gtk_box_pack_start(GTK_BOX(top_hbox), ip_entry, TRUE, TRUE, 0);
    
    gtk_box_pack_start(GTK_BOX(top_hbox), gtk_label_new("Port:"), FALSE, FALSE, 0);
    port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_spin), 8080); // makul varsayılan
    gtk_box_pack_start(GTK_BOX(top_hbox), port_spin, FALSE, FALSE, 0);
    
    connect_btn = gtk_button_new_with_label("Bağlan");
    gtk_style_context_add_class(gtk_widget_get_style_context(connect_btn), "primary");
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_hbox), connect_btn, FALSE, FALSE, 0);

    disconnect_btn = gtk_button_new_with_label("Kopar");
    gtk_style_context_add_class(gtk_widget_get_style_context(disconnect_btn), "danger");
    gtk_widget_set_sensitive(disconnect_btn, FALSE);
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_hbox), disconnect_btn, FALSE, FALSE, 0);
    
    status_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#475569'>Bağlantı bekleniyor…</span>");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);
    
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    
    tree_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tree_store));
    g_object_unref(tree_store);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Sunucu Dosya ve Klasörleri", renderer, "text", 0, "foreground", 1, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    gtk_container_add(GTK_CONTAINER(scroll), tree_view);
    
    // Alt Butonlar
    GtkWidget *bottom_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), bottom_hbox, FALSE, FALSE, 0);
    
    get_btn = gtk_button_new_with_label("⬇  Seçili Dosyayı İndir");
    gtk_style_context_add_class(gtk_widget_get_style_context(get_btn), "primary");
    gtk_box_pack_start(GTK_BOX(bottom_hbox), get_btn, TRUE, TRUE, 0);
    g_signal_connect(get_btn, "clicked", G_CALLBACK(on_get_clicked), NULL);

    put_btn = gtk_button_new_with_label("⬆  Sunucuya Dosya Yükle");
    gtk_style_context_add_class(gtk_widget_get_style_context(put_btn), "accent");
    gtk_box_pack_start(GTK_BOX(bottom_hbox), put_btn, TRUE, TRUE, 0);
    g_signal_connect(put_btn, "clicked", G_CALLBACK(on_put_clicked), NULL);

    // Transfer ilerleme çubuğu (worker thread tarafından g_idle_add ile güncellenir).
    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Hazır");
    gtk_box_pack_start(GTK_BOX(vbox), progress_bar, FALSE, FALSE, 0);

    apply_css(); // Modern görünüm için CSS temayı yükle

    gtk_widget_show_all(main_window);
    gtk_main();
    
    if (client_sock != -1) close(client_sock);
    return 0;
}
