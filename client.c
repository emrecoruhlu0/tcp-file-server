#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <sys/stat.h>

int client_sock = -1;

GtkWidget *main_window;
GtkWidget *ip_entry;
GtkWidget *port_spin;
GtkWidget *connect_btn;
GtkWidget *disconnect_btn;
GtkWidget *status_label;
GtkWidget *tree_view;
GtkTreeStore *tree_store;

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

void on_connect_clicked(GtkWidget *w, gpointer data) {
    const char *ip = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_spin));
    
    struct sockaddr_in sa;
    if (inet_pton(AF_INET, ip, &(sa.sin_addr)) != 1) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>HATA: Geçersiz IPv4 Adresi!</span>");
        return;
    }
    
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    
    gtk_label_set_markup(GTK_LABEL(status_label), "Bağlanılıyor...");
    
    if (connect(client_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>Bağlantı hatası! Sunucu açık mı?</span>");
        close(client_sock);
        client_sock = -1;
        return;
    }
    
    gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='green' weight='bold'>Bağlanıldı! Dosyalar getiriliyor...</span>");
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
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='red' weight='bold'>Bellek hatasi</span>");
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
    GtkTreeIter iters[100];
    
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
            
            GtkTreeIter *parent = (depth == 0) ? NULL : &iters[depth - 1];
            gtk_tree_store_append(tree_store, &iters[depth], parent);
            
            const char *color = is_ticked ? "black" : "red";
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
    if (client_sock != -1) {
        close(client_sock);
        client_sock = -1;
    }
    gtk_widget_set_sensitive(connect_btn, TRUE);
    gtk_widget_set_sensitive(ip_entry, TRUE);
    gtk_widget_set_sensitive(port_spin, TRUE);
    gtk_widget_set_sensitive(disconnect_btn, FALSE);
    gtk_label_set_markup(GTK_LABEL(status_label), "<span weight='bold'>Bağlantı kesildi.</span>");
    gtk_tree_store_clear(tree_store);
}

void on_get_clicked(GtkWidget *w, gpointer data) {
    if (client_sock == -1) return;
    
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
            
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "GET %s\n", full_path);
            send(client_sock, cmd, strlen(cmd), 0);
            
            char resp[128];
            if (recv_line(client_sock, resp, sizeof(resp)) > 0) {
                if (strncmp(resp, "OK ", 3) == 0) {
                    long size = atol(resp + 3);
                    FILE *f = fopen(save_filename, "wb");
                    if (f) {
                        long total = 0;
                        char buf[4096];
                        while (total < size) {
                            int to_recv = (size - total < sizeof(buf)) ? (size - total) : sizeof(buf);
                            int rx = recv(client_sock, buf, to_recv, 0);
                            if (rx <= 0) break;
                            fwrite(buf, 1, rx, f);
                            total += rx;
                        }
                        fclose(f);
                        GtkWidget *md = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Dosya başarıyla indirildi.");
                        gtk_dialog_run(GTK_DIALOG(md));
                        gtk_widget_destroy(md);
                    }
                } else {
                    GtkWidget *md = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "İndirme başarısız: %s", resp);
                    gtk_dialog_run(GTK_DIALOG(md));
                    gtk_widget_destroy(md);
                }
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
    if (client_sock == -1) return;
    
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Yüklenecek Dosyayı Seç", GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_OPEN, "İptal", GTK_RESPONSE_CANCEL, "Aç", GTK_RESPONSE_ACCEPT, NULL);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *filename = strrchr(filepath, '/');
        if (filename) filename++; else filename = filepath;
        
        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
            long size = st.st_size;
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "PUT %s %ld\n", filename, size);
            send(client_sock, cmd, strlen(cmd), 0);
            
            char resp[128];
            if (recv_line(client_sock, resp, sizeof(resp)) > 0) {
                if (strncmp(resp, "OK", 2) == 0) {
                    FILE *f = fopen(filepath, "rb");
                    if (f) {
                        char buf[4096];
                        int r;
                        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
                            send(client_sock, buf, r, 0);
                        }
                        fclose(f);
                        GtkWidget *md = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Dosya başarıyla yüklendi.");
                        gtk_dialog_run(GTK_DIALOG(md));
                        gtk_widget_destroy(md);
                    }
                } else {
                    GtkWidget *md = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Sunucu yüklemeyi reddetti: %s", resp);
                    gtk_dialog_run(GTK_DIALOG(md));
                    gtk_widget_destroy(md);
                }
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
    gtk_box_pack_start(GTK_BOX(top_hbox), port_spin, FALSE, FALSE, 0);
    
    connect_btn = gtk_button_new_with_label("Bağlan");
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_hbox), connect_btn, FALSE, FALSE, 0);
    
    disconnect_btn = gtk_button_new_with_label("Kopar");
    gtk_widget_set_sensitive(disconnect_btn, FALSE); 
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_hbox), disconnect_btn, FALSE, FALSE, 0);
    
    status_label = gtk_label_new("Bağlantı bekleniyor...");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);
    
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    
    tree_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tree_store));
    g_object_unref(tree_store);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Sunucu Klasörleri", renderer, "text", 0, "foreground", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    gtk_container_add(GTK_CONTAINER(scroll), tree_view);
    
    // Alt Butonlar
    GtkWidget *bottom_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), bottom_hbox, FALSE, FALSE, 0);
    
    GtkWidget *get_btn = gtk_button_new_with_label("Seçili Dosyayı İndir (GET)");
    gtk_box_pack_start(GTK_BOX(bottom_hbox), get_btn, TRUE, TRUE, 0);
    g_signal_connect(get_btn, "clicked", G_CALLBACK(on_get_clicked), NULL);
    
    GtkWidget *put_btn = gtk_button_new_with_label("Sunucuya Dosya Yükle (PUT)");
    gtk_box_pack_start(GTK_BOX(bottom_hbox), put_btn, TRUE, TRUE, 0);
    g_signal_connect(put_btn, "clicked", G_CALLBACK(on_put_clicked), NULL);
    
    gtk_widget_show_all(main_window);
    gtk_main();
    
    if (client_sock != -1) close(client_sock);
    return 0;
}
