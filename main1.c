#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

static GtkCssProvider *provider_current = NULL;
static GtkCssProvider *provider_previous = NULL;
static GtkCssProvider *provider_preprevious = NULL;

GtkLabel *label_current;
GtkLabel *label_previous;
GtkLabel *label_preprevious;

char latest_token[32] = {0};
char previous_token[32] = {0};
char preprevious_token[32] = {0};

// ---------- Token Update ----------
gboolean update_labels_ui(gpointer user_data) {
    gtk_label_set_text(label_current, latest_token);
    gtk_label_set_text(label_previous, previous_token);
    gtk_label_set_text(label_preprevious, preprevious_token);
    return FALSE;
}

// ---------- Socket Thread ----------
void *socket_thread_func(void *arg) {
    int server_fd = *(int *)arg;
    char buffer[1024];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (1) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        int bytes = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            g_print("Received: %s\n", buffer);

            // Shift tokens
            strncpy(preprevious_token, previous_token, sizeof(preprevious_token) - 1);
            strncpy(previous_token, latest_token, sizeof(previous_token) - 1);
            strncpy(latest_token, buffer, sizeof(latest_token) - 1);

            g_idle_add(update_labels_ui, NULL);
        }

        close(client_fd);
    }
    return NULL;
}

// ---------- Server Setup ----------
int setup_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return -1;
    }

    listen(server_fd, 5);
    return server_fd;
}

// ---------- CSS Loading ----------
void load_css(const char *css_path) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, css_path, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );
    g_object_unref(provider);
}

// Helper to set font size and color using dynamic CSS per label
static void set_label_font_size(GtkWidget *widget, int percent, const char *css_class, const char *color, GtkCssProvider **provider_ptr) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);

   int font_size = (allocation.height ) * percent / 100;
    if (font_size < 10) font_size = 10;
    if (font_size > 170) font_size = 170; // Prevents oversized fonts

    char css[256];
    snprintf(css, sizeof(css),
        "label.%s { color: %s; font-size: %dpx; font-weight: bold; }",
        css_class, color, font_size);

    printf("%s: allocation=%dx%d, percent=%d, font_size=%d\n", css_class,  allocation.width, allocation.height, percent, font_size);

    if (*provider_ptr == NULL) {
        *provider_ptr = gtk_css_provider_new();
        GtkStyleContext *context = gtk_widget_get_style_context(widget);
        gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(*provider_ptr), GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_data(*provider_ptr, css, -1, NULL);
}

struct LabelStyleParams {
    int percent;
    const char *css_class;
    const char *color;
    GtkCssProvider **provider_ptr;
};

static void on_number_label_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    struct LabelStyleParams *params = user_data;
    set_label_font_size(widget, params->percent, params->css_class, params->color, params->provider_ptr);
}

// ---------- Main ----------
int main(int argc, char *argv[]) {
    GtkBuilder *builder;
    GtkWidget *window;
    GError *error = NULL;

    gtk_init(&argc, &argv);

    builder = gtk_builder_new();
    gtk_builder_add_from_file(builder, "interface_copilot.glade", &error);
    if (error) {
        g_error("Glade load error: %s", error->message);
        g_clear_error(&error);
        return 1;
    }

    window = GTK_WIDGET(gtk_builder_get_object(builder, "window1"));
    label_current = GTK_LABEL(gtk_builder_get_object(builder, "current_draw_label"));
    label_previous = GTK_LABEL(gtk_builder_get_object(builder, "previous_draw_label"));
    label_preprevious = GTK_LABEL(gtk_builder_get_object(builder, "preceding_draw_label"));

    if (!label_current || !label_previous || !label_preprevious) {
        g_printerr("Some label objects not found. Check Glade IDs.\n");
        return 1;
    }

    load_css("style.css");

    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(label_current)), "current-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(label_previous)), "previous-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(label_preprevious)), "preprevious-label");

    static struct LabelStyleParams current_params = {50, "current-label", "red", &provider_current};
    static struct LabelStyleParams previous_params = {25, "previous-label", "#00008b", &provider_previous};
    static struct LabelStyleParams preprevious_params = {20, "preprevious-label", "black", &provider_preprevious};

    g_signal_connect(label_current, "size-allocate", G_CALLBACK(on_number_label_size_allocate), &current_params);
    g_signal_connect(label_previous, "size-allocate", G_CALLBACK(on_number_label_size_allocate), &previous_params);
    g_signal_connect(label_preprevious, "size-allocate", G_CALLBACK(on_number_label_size_allocate), &preprevious_params);

    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    gtk_widget_show_all(window);

    gtk_label_set_use_markup(label_previous, FALSE);
    gtk_label_set_use_markup(label_preprevious, FALSE);
    gtk_label_set_use_markup(label_current, FALSE);

    int server_fd = setup_server_socket(9000);
    if (server_fd == -1) return 1;

    pthread_t socket_thread;
    pthread_create(&socket_thread, NULL, socket_thread_func, &server_fd);

    gtk_main();
    close(server_fd);
    return 0;
}

