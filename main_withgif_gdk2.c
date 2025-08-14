#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

// ===================== Widgets =====================
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label, *ticker_label2;
GtkWidget *gif_area = NULL;

// ===================== GIF Player =====================
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GtkWidget *drawing_area;
    GTimer *timer;
} GifPlayer;

static GifPlayer *gif_player = NULL;

// ===================== Ticker =====================
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

// ===================== Tokens =====================
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

// ===================== Helpers =====================
void refocus_main_window(GtkWidget *win) {
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_present(GTK_WINDOW(win));
        if (gtk_widget_get_window(win)) {
            gdk_window_raise(gtk_widget_get_window(win));
        }
    }
}

// --- GIF timing advance ---
static gboolean gif_player_advance(gpointer data) {
    if (!gif_player || !gif_player->iter) return G_SOURCE_REMOVE;

    gdouble elapsed_ms = g_timer_elapsed(gif_player->timer, NULL) * 1000.0;
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    if (delay < 0) delay = 100;

    if (elapsed_ms >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_player->drawing_area);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

// --- GIF draw ---
static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_player || !gif_player->iter) return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;

    int da_w = gtk_widget_get_allocated_width(widget);
    int da_h = gtk_widget_get_allocated_height(widget);
    if (da_w < 1 || da_h < 1) return FALSE;

    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);
    if (fw <= 0 || fh <= 0) return FALSE;

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    double scale = (double)da_h / (double)fh;
    int scaled_w = (int)(fw * scale);
    int x_offset = (da_w - scaled_w) / 2;

    cairo_save(cr);
    cairo_translate(cr, x_offset, 0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, frame, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    return FALSE;
}

static void gif_player_cleanup(void) {
    if (!gif_player) return;

    if (gif_player->timeout_id) {
        g_source_remove(gif_player->timeout_id);
        gif_player->timeout_id = 0;
    }
    if (gif_player->animation) g_object_unref(gif_player->animation);
    if (gif_player->iter) g_object_unref(gif_player->iter);
    if (gif_player->timer) g_timer_destroy(gif_player->timer);
    g_free(gif_player);
    gif_player = NULL;
}

gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;
    if (!gif_area) return FALSE;

    if (gif_player) gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);
    gif_player->drawing_area = gif_area;
    g_signal_connect(gif_area, "draw", G_CALLBACK(gif_player_draw), NULL);

    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (error || !gif_player->animation) {
        g_printerr("GIF Error loading %s: %s\n", filename, error ? error->message : "Invalid animation");
        if (error) g_error_free(error);
        gif_player_cleanup();
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();
    g_timer_start(gif_player->timer);

    gtk_widget_set_visible(gif_area, TRUE);
    gtk_widget_queue_draw(gif_area);
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);

    refocus_main_window(window);
    return FALSE;
}

gboolean hide_overlay_gif(gpointer user_data) {
    if (gif_area) gtk_widget_set_visible(gif_area, FALSE);
    gif_player_cleanup();
    refocus_main_window(window);
    return FALSE;
}

// ===================== Token Images =====================
void generate_token_image(GtkWidget *widget, const char *number, const char *label,
                          const char *filename, const char *bg, const char *fg,
                          float number_size_percent, float label_size_percent,
                          float number_x_offset_percent, float number_y_offset_percent,
                          float label_x_offset_percent, float label_y_offset_percent,
                          const char *number_font, const char *label_font) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    if (width < 100 || height < 100) return;

    int number_pointsize = (int)(height * number_size_percent);
    int label_pointsize  = (int)(height * label_size_percent);
    int number_x_offset  = (int)(width * number_x_offset_percent);
    int number_y_offset  = (int)(height * number_y_offset_percent);
    int label_x_offset   = (int)(width * label_x_offset_percent);
    int label_y_offset   = (int)(height * label_y_offset_percent);

    char command[1024];
    snprintf(command, sizeof(command),
        "convert -size %dx%d xc:%s "
        "-gravity center -fill %s -font %s "
        "-pointsize %d -annotate +%d+%d \"%s\" "
        "-font %s -pointsize %d -annotate +%d+%d \"%s\" "
        "%s",
        width, height, bg,
        fg, number_font,
        number_pointsize, number_x_offset, number_y_offset, number,
        label_font, label_pointsize, label_x_offset, label_y_offset, label,
        filename);

    system(command);
}

gboolean refresh_images_on_ui(gpointer user_data) {
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");
    return FALSE;
}

void *image_generator_thread(void *arg) {
    generate_token_image(current_image, current_token, "Current Draw", "current.png", "peachpuff", "red",
                         0.78, 0.18, 0.1, -0.07, 0.05, 0.41,
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf");

    generate_token_image(previous_image, previous_token, "Previous Draw", "previous.png", "peachpuff", "blue",
                         0.70, 0.10, -0.04, -0.03, -0.06, 0.30,
                         "Arial-Bold", "Arial");

    generate_token_image(preceding_image, preceding_token, "Preceding Draw", "preceding.png", "peachpuff", "brown",
                         0.92, 0.17, -0.08, -0.1, -0.05, 0.35,
                         "Arial-Bold", "Arial");

    g_idle_add(refresh_images_on_ui, NULL);
    return NULL;
}

// ===================== Seamless Ticker =====================
gboolean animate_ticker(gpointer data) {
    ticker_x -= 2;
    if (ticker_x <= -ticker_width) {
        ticker_x += ticker_width;
    }
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label2, ticker_x + ticker_width, 0);
    return G_SOURCE_CONTINUE;
}

gboolean finalize_ticker_setup(gpointer data) {
    ticker_width = gtk_widget_get_allocated_width(ticker_label);
    ticker_area_width = gtk_widget_get_allocated_width(ticker_fixed);

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    ticker_x = 0;
    ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== Layout sizing =====================
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

    int top_font_size = (int)(outermost_alloc.height * 0.08 * 0.8 * PANGO_SCALE);
    int ticker_font_size = (int)(outermost_alloc.height * 0.045 * 0.9 * PANGO_SCALE);

    gtk_widget_set_margin_top(top_pane, (int)(outermost_alloc.height * 0.02));

    char markup_top[256];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Fira Sans' weight='bold' size='%d' foreground='#8B0000'>Aurum Mega Event</span>",
        top_font_size);
    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);

    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        ticker_font_size);
    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);
    gtk_label_set_markup(GTK_LABEL(ticker_label2), markup_ticker);

    pthread_t init_image_thread;
    pthread_create(&init_image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(init_image_thread);

    g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== Tokens =====================
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

// ===================== Serial Thread =====================
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

            if (strcmp(token, "C1") == 0) {
                g_idle_add(show_fullscreen_gif, "congratulations1.gif");
            } else if (strcmp(token, "G1") == 0) {
                g_idle_add(show_fullscreen_gif, "gameover1.gif");
                strcpy(current_token, "--");
                strcpy(previous_token, "--");
                strcpy(preceding_token, "--");
                g_idle_add(update_ui_from_serial, NULL);
            } else {
                g_idle_add(hide_overlay_gif, NULL);
                shift_tokens(token);
                g_idle_add(update_ui_from_serial, NULL);
            }
        }
        usleep(100000);
    }

    close(fd);
    return NULL;
}

// ===================== Cleanup =====================
void cleanup_images(void) {
    remove("current.png");
    remove("previous.png");
    remove("preceding.png");
    gif_player_cleanup();
}

// ===================== main =====================
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_overlay.glade");
    window         = GTK_WIDGET(gtk_builder_get_object(builder, "main"));

    top_label      = GTK_WIDGET(gtk_builder_get_object(builder, "top_label"));
    current_image  = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image= GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    top_pane       = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));
    outermost      = GTK_WIDGET(gtk_builder_get_object(builder, "outermost"));
    outer          = GTK_WIDGET(gtk_builder_get_object(builder, "outer"));
    inner          = GTK_WIDGET(gtk_builder_get_object(builder, "inner"));
    ticker_fixed   = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label   = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));
    gif_area       = GTK_WIDGET(gtk_builder_get_object(builder, "gif_area"));

    if (!window || !top_label || !current_image || !previous_image || !preceding_image ||
        !top_pane || !outermost || !outer || !inner || !ticker_fixed || !ticker_label || !gif_area) {
        g_printerr("Error: Missing widgets from Glade.\n");
        return 1;
    }

    // Create ticker_label2 for seamless scrolling
    ticker_label2 = gtk_label_new(NULL);
    gtk_fixed_put(GTK_FIXED(ticker_fixed), ticker_label2, 0, 0);

    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_idle_add(set_paned_ratios, NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_images), NULL);

    gtk_widget_show_all(window);
    gtk_widget_set_visible(gif_area, FALSE);
    refocus_main_window(window);

    strcpy(current_token, "--");
    strcpy(previous_token, "--");
    strcpy(preceding_token, "--");

    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    gtk_main();
    return 0;
}
