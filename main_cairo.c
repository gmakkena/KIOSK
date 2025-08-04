// GTK Token Display using Cairo + Pango instead of ImageMagick
#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cairo.h>
#include <pango/pangocairo.h>

// Widgets
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

// Ticker
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

// Tokens
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

void draw_token_image(const char *filename, const char *number, const char *label,
                      int width, int height,
                      const char *number_color, const char *label_color,
                      int number_font_size, int label_font_size) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1.0, 0.85, 0.72); // peachpuff
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    char markup_number[256];
    snprintf(markup_number, sizeof(markup_number),
             "<span font_desc='Arial Bold' size='%d000' foreground='%s'>%s</span>",
             number_font_size, number_color, number);
    pango_layout_set_markup(layout, markup_number, -1);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    cairo_move_to(cr, 0, height * 0.2);
    pango_cairo_show_layout(cr, layout);

    char markup_label[256];
    snprintf(markup_label, sizeof(markup_label),
             "<span font_desc='Arial' size='%d000' foreground='%s'>%s</span>",
             label_font_size, label_color, label);
    pango_layout_set_markup(layout, markup_label, -1);
    cairo_move_to(cr, 0, height * 0.75);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_write_to_png(surface, filename);
    cairo_surface_destroy(surface);
}

void generate_token_image(GtkWidget *widget, const char *number, const char *label,
                          const char *filename, const char *fg,
                          float number_size_percent, float label_size_percent) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    if (width < 100 || height < 100) return;

    int number_font_size = (int)(height * number_size_percent);
    int label_font_size  = (int)(height * label_size_percent);

    draw_token_image(filename, number, label, width, height,
                     fg, fg, number_font_size, label_font_size);
}

gboolean refresh_images_on_ui(gpointer user_data) {
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");
    return FALSE;
}

void *image_generator_thread(void *arg) {
    generate_token_image(current_image, current_token, "Current Draw", "current.png", "red", 0.15, 0.05);
    generate_token_image(previous_image, previous_token, "Previous Draw", "previous.png", "blue", 0.13, 0.04);
    generate_token_image(preceding_image, preceding_token, "Preceding Draw", "preceding.png", "brown", 0.16, 0.045);
    g_idle_add(refresh_images_on_ui, NULL);
    return NULL;
}

gboolean animate_ticker(gpointer data) {
    ticker_x -= 2;
    if (ticker_x + ticker_width < 0)
        ticker_x = ticker_area_width;
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    return G_SOURCE_CONTINUE;
}

gboolean finalize_ticker_setup(gpointer data) {
    ticker_width = gtk_widget_get_allocated_width(ticker_label);
    ticker_area_width = gtk_widget_get_allocated_width(ticker_fixed);
    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;
    ticker_x = ticker_area_width;
    ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);
    return G_SOURCE_REMOVE;
}

gboolean set_paned_ratios(gpointer user_data) {
    gtk_paned_set_wide_handle(GTK_PANED(top_pane), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outermost), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outer), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(inner), FALSE);

    GtkAllocation top_alloc, outermost_alloc, outer_alloc, inner_alloc;
    gtk_widget_get_allocation(top_pane, &top_alloc);
    gtk_widget_get_allocation(outermost, &outermost_alloc);
    gtk_widget_get_allocation(outer, &outer_alloc);
    gtk_widget_get_allocation(inner, &inner_alloc);

    gtk_paned_set_position(GTK_PANED(top_pane), top_alloc.height * 0.08);
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.92);
    gtk_paned_set_position(GTK_PANED(outer), outer_alloc.width * 0.72);
    gtk_paned_set_position(GTK_PANED(inner), inner_alloc.height * 0.70);

    GdkRectangle monitor_geometry;
    GdkMonitor *monitor = gdk_display_get_primary_monitor(gdk_display_get_default());
    gdk_monitor_get_geometry(monitor, &monitor_geometry);
    int h = monitor_geometry.height;

    char markup_top[256];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#8B0000'>Aurum Mega Event</span>",
        (int)(h * 0.059 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);

    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        (int)(h * 0.045 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);

    pthread_t init_image_thread;
    pthread_create(&init_image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(init_image_thread);

    g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

gboolean update_ui_from_serial(gpointer user_data) {
    pthread_t image_thread;
    pthread_create(&image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(image_thread);
    return FALSE;
}

void *serial_reader_thread(void *arg) {
    const char *serial_port = "/dev/serial0";
    int fd = open(serial_port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("Unable to open serial port");
        return NULL;
    }

    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, B9600);
    cfsetospeed(&options, B9600);
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CRTSCTS;
    options.c_iflag = IGNPAR;
    options.c_oflag = 0;
    options.c_lflag = 0;
    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &options);

    char buf[64];
    while (1) {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char token[32];
            sscanf(buf, "%31s", token);
            shift_tokens(token);
            g_idle_add(update_ui_from_serial, NULL);
        }
        usleep(100000);
    }

    close(fd);
    return NULL;
}

void cleanup_images(void) {
    remove("current.png");
    remove("previous.png");
    remove("preceding.png");
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned.glade");
    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));

    top_label       = GTK_WIDGET(gtk_builder_get_object(builder, "top_label"));
    current_image   = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image  = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    top_pane        = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));
    outermost       = GTK_WIDGET(gtk_builder_get_object(builder, "outermost"));
    outer           = GTK_WIDGET(gtk_builder_get_object(builder, "outer"));
    inner           = GTK_WIDGET(gtk_builder_get_object(builder, "inner"));
    ticker_fixed    = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label    = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));

    if (!window || !top_label || !current_image || !previous_image || !preceding_image ||
        !top_pane || !outermost || !outer || !inner || !ticker_fixed || !ticker_label) {
        g_printerr("Error: Missing widgets from Glade.\n");
        return 1;
    }

    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    g_idle_add(set_paned_ratios, NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_images), NULL);
    gtk_widget_show_all(window);

    g_timeout_add(300, (GSourceFunc)update_ui_from_serial, NULL);

    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    gtk_main();
    return 0;
}
