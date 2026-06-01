#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <time.h>

#define BUFFER_SIZE 4096

// Global değişkenler
int server_sock = -1;
int is_running = 0;
int current_port = 0;
char current_ip[INET_ADDRSTRLEN] = "Bilinmiyor";
pthread_t server_thread_id;

// İstemciye gönderilecek ağaç formatındaki (LIST yanıtı) yapısal önbellek
char *cached_list_response = NULL;
size_t cached_size = 0;

// Loglama için Mutex
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// cached_list_response paylaşımlı kaynak: birçok client okur, GUI thread'i nadir yazar.
pthread_rwlock_t cache_rwlock = PTHREAD_RWLOCK_INITIALIZER;

#define MAX_TREE_DEPTH 16

GtkWidget *main_window;
GtkWidget *status_label;
GtkWidget *ip_port_label;
GtkWidget *toggle_button;
GtkWidget *tree_view;
GtkWidget *scrolled_window;
GtkTreeStore *tree_store;
GtkWidget *log_view;        // Canlı log paneli (GtkTextView)
GtkWidget *clients_label;   // "Bağlı istemci: N" etiketi

// Eşzamanlı bağlı istemci sayısı. Birçok client thread'i + GUI thread'i erişir,
// bu yüzden atomik olarak güncellenir.
volatile int connected_clients = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Ana thread'te çalışır (g_idle_add): tek bir log satırını GUI paneline ekler.
// data: g_strdup ile ayrılmış, "[zaman] mesaj" biçimli satır; burada serbest bırakılır.
static gboolean append_log_to_view(gpointer data) {
    char *line = (char *)data;
    if (log_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buf, &end);
        gtk_text_buffer_insert(buf, &end, line, -1);
        // En alta otomatik kaydır.
        gtk_text_buffer_get_end_iter(buf, &end);
        GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view), mark);
        gtk_text_buffer_delete_mark(buf, mark);
    }
    g_free(line);
    return G_SOURCE_REMOVE;
}

// Zaman damgalı log yazma fonksiyonu (Thread-safe).
// Hem server.log dosyasına hem de GUI'deki canlı log paneline yazar.
void log_event(const char *msg) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_buf[26];
    strftime(time_buf, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen("server.log", "a");
    if (f) {
        fprintf(f, "[%s] %s\n", time_buf, msg);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mutex);

    // GUI paneline iletmek için satırı hazırla; GTK'ya yalnızca ana thread'ten
    // (g_idle_add) dokunulur çünkü log_event farklı client thread'lerinden çağrılır.
    char *line = g_strdup_printf("[%s] %s\n", time_buf, msg);
    g_idle_add(append_log_to_view, line);
}

// Ana thread'te çalışır (g_idle_add): bağlı istemci sayacını GUI'de günceller.
static gboolean refresh_clients_label(gpointer data) {
    (void)data;
    if (clients_label) {
        pthread_mutex_lock(&clients_mutex);
        int n = connected_clients;
        pthread_mutex_unlock(&clients_mutex);
        char txt[64];
        snprintf(txt, sizeof(txt), "Bağlı istemci: <b>%d</b>", n);
        gtk_label_set_markup(GTK_LABEL(clients_label), txt);
    }
    return G_SOURCE_REMOVE;
}

// Sayacı değiştirir (delta: +1 bağlanınca, -1 kopunca) ve GUI'yi tetikler.
static void update_client_count(int delta) {
    pthread_mutex_lock(&clients_mutex);
    connected_clients += delta;
    if (connected_clients < 0) connected_clients = 0;
    pthread_mutex_unlock(&clients_mutex);
    g_idle_add(refresh_clients_label, NULL);
}

// Satır okuma yardımcı fonksiyonu (Komutları güvenle almak için)
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

// IP Adresini bulma fonksiyonu
void get_local_ip(char *buffer, size_t buflen) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        strncpy(buffer, "Bulunamadi", buflen);
        return;
    }
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    
    if (connect(sock, (const struct sockaddr*) &serv, sizeof(serv)) < 0) {
        strncpy(buffer, "Bulunamadi", buflen);
        close(sock);
        return;
    }
    
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, (struct sockaddr*) &name, &namelen) < 0) {
        strncpy(buffer, "Bulunamadi", buflen);
    } else {
        inet_ntop(AF_INET, &name.sin_addr, buffer, buflen);
    }
    close(sock);
}

// Ağaçtaki onay kutusuna (tik) tıklandığında çalışır
void on_cell_toggled(GtkCellRendererToggle *cell, gchar *path_string, gpointer user_data) {
    if (is_running) return; 
    
    GtkTreeModel *model = GTK_TREE_MODEL(user_data);
    GtkTreeIter iter;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    
    gboolean is_ticked;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_model_get(model, &iter, 1, &is_ticked, -1);
    
    is_ticked = !is_ticked;
    gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 1, is_ticked, -1);
    
    gtk_tree_path_free(path);
}

// Dizin içeriğini ağaca ekleyen özyinelemeli fonksiyon
void populate_tree(GtkTreeStore *store, GtkTreeIter *parent, const char *path, int depth) {
    if (depth >= MAX_TREE_DEPTH) return;
    DIR *d = opendir(path);
    if (!d) return;
    
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        
        GtkTreeIter iter;
        gtk_tree_store_append(store, &iter, parent);
        
        char full_path[2048];
        if (strcmp(path, ".") == 0) {
            snprintf(full_path, sizeof(full_path), "%s", dir->d_name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, dir->d_name);
        }
        
        struct stat st;
        int is_dir = 0;
        if (stat(full_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        } else if (dir->d_type == DT_DIR) {
            is_dir = 1;
        }
        
        char display_name[512];
        if (is_dir) {
            snprintf(display_name, sizeof(display_name), "📁 %s", dir->d_name);
        } else {
            snprintf(display_name, sizeof(display_name), "📄 %s", dir->d_name);
        }
        
        gtk_tree_store_set(store, &iter, 0, display_name, 1, TRUE, 2, full_path, -1);
        
        if (is_dir) {
            populate_tree(store, &iter, full_path, depth + 1);
        }
    }
    closedir(d);
}

// İstemciye yollanacak veriyi ekler
void append_to_cache(int depth, gboolean is_ticked, int is_dir, const char *name, const char *path) {
    char line[2048];
    snprintf(line, sizeof(line), "%d|%d|%d|%s|%s\n", depth, is_ticked, is_dir, name, path);
    size_t len = strlen(line);
    
    cached_list_response = realloc(cached_list_response, cached_size + len + 1);
    strcpy(cached_list_response + cached_size, line);
    cached_size += len;
}

// Ağacı okuyarak istemcinin anlayacağı tek bir metne dönüştürür
void build_cached_list_recursive(GtkTreeModel *model, GtkTreeIter *iter, int depth) {
    do {
        gboolean is_ticked;
        gchar *full_path;
        gchar *display_name;
        
        gtk_tree_model_get(model, iter, 0, &display_name, 1, &is_ticked, 2, &full_path, -1);
        
        int is_dir = (strstr(display_name, "📁") != NULL) ? 1 : 0;
        append_to_cache(depth, is_ticked, is_dir, display_name, full_path);
        
        if (is_dir && is_ticked) {
            GtkTreeIter child_iter;
            if (gtk_tree_model_iter_children(model, &child_iter, iter)) {
                build_cached_list_recursive(model, &child_iter, depth + 1);
            }
        }
        
        g_free(full_path);
        g_free(display_name);
    } while (gtk_tree_model_iter_next(model, iter));
}

// İstemciyle ilgilenecek thread fonksiyonu
void *client_handler(void *socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc);

    update_client_count(+1);

    char buffer[BUFFER_SIZE];

    while(recv_line(client_sock, buffer, BUFFER_SIZE) > 0) {
        printf("Gelen Komut: %s", buffer);

        if(strstr(buffer, "../") != NULL || strstr(buffer, "..\\") != NULL) {
            char *error_msg = "ERROR Yetkisiz erisim\n";
            send(client_sock, error_msg, strlen(error_msg), 0);
            log_event("Güvenlik İhlali: Yetkisiz dizin erisimi denemesi.");
            continue;
        }

        if(strncmp(buffer, "LIST", 4) == 0) {
            pthread_rwlock_rdlock(&cache_rwlock);
            if (cached_list_response && strlen(cached_list_response) > 0) {
                send(client_sock, cached_list_response, strlen(cached_list_response), 0);
            }
            pthread_rwlock_unlock(&cache_rwlock);
            char *end_marker = "__END_OF_LIST__\n";
            send(client_sock, end_marker, strlen(end_marker), 0);
            log_event("Komut islendi: LIST");
        }
        else if(strncmp(buffer, "GET ", 4) == 0) {
            char filepath[1024];
            if (sscanf(buffer + 4, "%1023s", filepath) == 1) {
                // Sadece izin verilen listede ve "ticked=1" olarak işaretlenmişse indirilebilir.
                pthread_rwlock_rdlock(&cache_rwlock);
                int allowed = 0;
                if (cached_list_response) {
                    // "|1|...|<path>\n" formatında satır arıyoruz: ticked=1 olmalı.
                    char needle[1100];
                    snprintf(needle, sizeof(needle), "|%s\n", filepath);
                    char *hit = strstr(cached_list_response, needle);
                    if (hit) {
                        // Aynı satırın başına gidip ticked alanını kontrol et.
                        char *line_start = hit;
                        while (line_start > cached_list_response && *(line_start - 1) != '\n') line_start--;
                        // satır formatı: depth|ticked|is_dir|name|path
                        char *p1 = strchr(line_start, '|');
                        if (p1 && *(p1 + 1) == '1') allowed = 1;
                    }
                }
                pthread_rwlock_unlock(&cache_rwlock);
                if (allowed) {
                    FILE *f = fopen(filepath, "rb");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        long size = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        char header[128];
                        snprintf(header, sizeof(header), "OK %ld\n", size);
                        send(client_sock, header, strlen(header), 0);

                        struct timespec t0, t1;
                        clock_gettime(CLOCK_MONOTONIC, &t0);

                        char file_buf[4096];
                        size_t bytes_read;
                        long sent_total = 0;
                        while ((bytes_read = fread(file_buf, 1, sizeof(file_buf), f)) > 0) {
                            ssize_t s = send(client_sock, file_buf, bytes_read, 0);
                            if (s <= 0) break;
                            sent_total += s;
                        }
                        fclose(f);

                        clock_gettime(CLOCK_MONOTONIC, &t1);
                        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
                        double mbps = (elapsed > 0) ? (sent_total / (1024.0 * 1024.0)) / elapsed : 0.0;

                        char log_msg[2048];
                        snprintf(log_msg, sizeof(log_msg),
                                 "GET %s (boyut: %ld byte, sure: %.3fs, hiz: %.2f MB/s)",
                                 filepath, sent_total, elapsed, mbps);
                        log_event(log_msg);
                    } else {
                        send(client_sock, "ERROR Dosya bulunamadi\n", 23, 0);
                        log_event("Hata: Istenilen dosya bulunamadi.");
                    }
                } else {
                    send(client_sock, "ERROR Erisim Engellendi\n", 24, 0);
                    char log_msg[2048];
                    snprintf(log_msg, sizeof(log_msg), "Izinsiz GET denemesi: %s", filepath);
                    log_event(log_msg);
                }
            }
        }
        else if(strncmp(buffer, "PUT ", 4) == 0) {
            char filename[1024];
            long filesize = 0;
            if (sscanf(buffer + 4, "%1023s %ld", filename, &filesize) == 2) {
                // Sadece mevcut dizine (root) yazmaya izin ver
                if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
                    send(client_sock, "ERROR Gecersiz dosya adi (../ kullanilamaz)\n", 45, 0);
                    log_event("Hata: PUT komutunda gecersiz dosya adi kullanildi.");
                    continue;
                }
                
                FILE *f = fopen(filename, "wb");
                if (f) {
                    send(client_sock, "OK\n", 3, 0);
                    struct timespec t0, t1;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    long total = 0;
                    char file_buf[4096];
                    while (total < filesize) {
                        int to_receive = sizeof(file_buf);
                        if (filesize - total < to_receive) to_receive = filesize - total;
                        int r = recv(client_sock, file_buf, to_receive, 0);
                        if (r <= 0) break;
                        fwrite(file_buf, 1, r, f);
                        total += r;
                    }
                    fclose(f);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
                    double mbps = (elapsed > 0) ? (total / (1024.0 * 1024.0)) / elapsed : 0.0;
                    char log_msg[2048];
                    snprintf(log_msg, sizeof(log_msg),
                             "PUT %s (boyut: %ld byte, sure: %.3fs, hiz: %.2f MB/s)",
                             filename, total, elapsed, mbps);
                    log_event(log_msg);
                } else {
                    send(client_sock, "ERROR Sunucu dosyayi olusturamadi\n", 34, 0);
                    log_event("Hata: PUT icin dosya olusturulamadi.");
                }
            } else {
                send(client_sock, "ERROR HATA\n", 11, 0);
            }
        }
        else {
            char *msg = "HATA: Gecersiz komut.\n";
            send(client_sock, msg, strlen(msg), 0);
            log_event("Bilinmeyen komut alindi.");
        }
    }

    log_event("Istemci baglantiyi kesti.");
    close(client_sock);
    update_client_count(-1);
    pthread_exit(NULL);
}

void *server_thread(void *arg) {
    int client_sock, c;
    struct sockaddr_in client;

    listen(server_sock, 5);
    c = sizeof(struct sockaddr_in);

    while(is_running) {
        client_sock = accept(server_sock, (struct sockaddr *)&client, (socklen_t*)&c);
        if (client_sock < 0) {
            break;
        }
        
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Yeni istemci baglandi. (IP: %s)", inet_ntoa(client.sin_addr));
        log_event(log_msg);
        
        int *new_sock = malloc(sizeof(int));
        *new_sock = client_sock;

        pthread_t sn_thread;
        if(pthread_create(&sn_thread, NULL, client_handler, (void*) new_sock) < 0) {
            perror("Thread olusturulamadi");
            free(new_sock);
            continue;
        }
        pthread_detach(sn_thread);
    }
    
    if (server_sock != -1) {
        close(server_sock);
        server_sock = -1;
    }
    return NULL;
}

void on_toggle_clicked(GtkWidget *widget, gpointer data) {
    if (!is_running) {
        pthread_rwlock_wrlock(&cache_rwlock);
        if (cached_list_response) {
            free(cached_list_response);
        }
        cached_list_response = calloc(1, 1);
        cached_size = 0;

        GtkTreeIter root_iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tree_store), &root_iter)) {
            build_cached_list_recursive(GTK_TREE_MODEL(tree_store), &root_iter, 0);
        }
        pthread_rwlock_unlock(&cache_rwlock);

        struct sockaddr_in server;
        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        
        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = INADDR_ANY;
        server.sin_port = htons(0); 

        if(bind(server_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
            perror("Bind basarisiz");
            gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>Hata: Port alınamadı!</span>");
            close(server_sock);
            server_sock = -1;
            return;
        }
        
        socklen_t len = sizeof(server);
        if (getsockname(server_sock, (struct sockaddr *)&server, &len) != -1) {
            current_port = ntohs(server.sin_port);
        } else {
            current_port = 0;
        }
        
        get_local_ip(current_ip, sizeof(current_ip));
        
        is_running = 1;
        gtk_button_set_label(GTK_BUTTON(toggle_button), "Sunucuyu Durdur");
        
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='green' weight='bold'>Durum: Çalışıyor</span>");
        
        char ip_port_markup[256];
        snprintf(ip_port_markup, sizeof(ip_port_markup), "IP: <span foreground='blue'>%s</span> | Port: <span foreground='blue'>%d</span>", current_ip, current_port);
        gtk_label_set_markup(GTK_LABEL(ip_port_label), ip_port_markup);
        
        log_event("Sunucu baslatildi.");
        
        pthread_create(&server_thread_id, NULL, server_thread, NULL);
        pthread_detach(server_thread_id);
    } else {
        is_running = 0;
        if (server_sock != -1) {
            shutdown(server_sock, SHUT_RDWR);
            close(server_sock);
            server_sock = -1;
        }
        gtk_button_set_label(GTK_BUTTON(toggle_button), "Sunucuyu Başlat");
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>Durum: Durduruldu</span>");
        gtk_label_set_text(GTK_LABEL(ip_port_label), "");
        
        log_event("Sunucu durduruldu.");

        pthread_rwlock_wrlock(&cache_rwlock);
        if (cached_list_response) {
            free(cached_list_response);
            cached_list_response = NULL;
            cached_size = 0;
        }
        pthread_rwlock_unlock(&cache_rwlock);
    }
}

// Uygulamaya modern bir görünüm kazandıran CSS temasını yükler.
static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "window { background-color: #f4f6f8; }"
        "button {"
        "  padding: 8px 14px;"
        "  border-radius: 6px;"
        "  font-weight: bold;"
        "}"
        "button:hover { background-image: none; background-color: #d9e6f2; }"
        "textview { font-size: 10pt; background-color: #1e1e1e; color: #d4d4d4; }"
        "textview text { background-color: #1e1e1e; color: #d4d4d4; }"
        "treeview { font-size: 11pt; }";

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
    gtk_window_set_title(GTK_WINDOW(main_window), "TCP Dosya Sunucusu");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 560, 640);
    gtk_container_set_border_width(GTK_CONTAINER(main_window), 20);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    GtkWidget *top_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), top_hbox, FALSE, FALSE, 0);

    status_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>Durum: Durduruldu</span>");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top_hbox), status_label, TRUE, TRUE, 0);

    ip_port_label = gtk_label_new("");
    gtk_widget_set_halign(ip_port_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(top_hbox), ip_port_label, TRUE, TRUE, 0);

    // Bağlı istemci sayacı (canlı güncellenir).
    clients_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(clients_label), "Bağlı istemci: <b>0</b>");
    gtk_widget_set_halign(clients_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), clients_label, FALSE, FALSE, 0);

    toggle_button = gtk_button_new_with_label("Sunucuyu Başlat");
    gtk_widget_set_size_request(toggle_button, -1, 40);
    g_signal_connect(toggle_button, "clicked", G_CALLBACK(on_toggle_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), toggle_button, FALSE, FALSE, 0);

    scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    tree_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tree_store));
    g_object_unref(tree_store);

    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *text_column = gtk_tree_view_column_new_with_attributes("Sunucu Dosyaları", text_renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), text_column);

    GtkCellRenderer *toggle_renderer = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle_renderer, "toggled", G_CALLBACK(on_cell_toggled), tree_store);
    GtkTreeViewColumn *toggle_column = gtk_tree_view_column_new_with_attributes("Paylaş", toggle_renderer, "active", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), toggle_column);

    gtk_container_add(GTK_CONTAINER(scrolled_window), tree_view);

    populate_tree(tree_store, NULL, ".", 0);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_view));

    // ---- Canlı log paneli ----
    GtkWidget *log_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(log_label), "<b>Olay Günlüğü</b>");
    gtk_widget_set_halign(log_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), log_label, FALSE, FALSE, 0);

    GtkWidget *log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(log_scroll, -1, 150);
    gtk_box_pack_start(GTK_BOX(vbox), log_scroll, FALSE, TRUE, 0);

    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);

    apply_css(); // Modern görünüm

    gtk_widget_show_all(main_window);
    
    log_event("Sunucu uygulamasi baslatildi.");
    gtk_main();

    if (is_running) {
        is_running = 0;
        if (server_sock != -1) {
            shutdown(server_sock, SHUT_RDWR);
            close(server_sock);
        }
        pthread_rwlock_wrlock(&cache_rwlock);
        if (cached_list_response) {
            free(cached_list_response);
            cached_list_response = NULL;
        }
        pthread_rwlock_unlock(&cache_rwlock);
    }
    log_event("Sunucu uygulamasi kapatildi.\n---");

    return 0;
}
