#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/********************************************************************
 * GLOBALS
 ********************************************************************/
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

GtkWidget *gif_window = NULL;     // NEW: fullscreen window for GIF
GtkWidget *gif_area   = NULL;     // drawing area inside gif_window

typedef struct {
    GdkPixbufAnimation *anim;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GTimer *timer;
} GifPlayer;

GifPlayer *gif_player = NULL;

char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

/********************************************************************
 * HELPER — SHIFT TOKENS
 ********************************************************************/
static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

/********************************************************************
 * GIF DRAW
 ********************************************************************/
static gboolean gif_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    if (!gif_player || !gif_player->iter) return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;

    int w  = gtk_widget_get_allocated_width(widget);
    int h  = gtk_widget_get_allocated_height(widget);
    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    double scale = (double)h / fh;
    int scaled_w = fw * scale;
    int xoff = (w - scaled_w) / 2;

    cairo_save(cr);
    cairo_translate(cr, xoff, 0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, frame, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    return FALSE;
}

static gboolean gif_advance(gpointer data) {
    if (!gif_player || !gif_player->iter) return G_SOURCE_REMOVE;

    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    if (delay < 0) delay = 100;

    if (g_timer_elapsed(gif_player->timer, NULL) * 1000 >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_area);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

/********************************************************************
 * GIF CLEANUP
 ********************************************************************/
static void close_gif_window(void) {
    if (!gif_window) return;

    if (gif_player) {
        if (gif_player->timeout_id) g_source_remove(gif_player->timeout_id);
        if (gif_player->anim) g_object_unref(gif_player->anim);
        if (gif_player->iter) g_object_unref(gif_player->iter);
        if (gif_player->timer) g_timer_destroy(gif_player->timer);
        free(gif_player);
        gif_player = NULL;
    }

    gtk_widget_destroy(gif_window);
    gif_window = NULL;
    gif_area   = NULL;

    gtk_widget_queue_draw(window);
}

/********************************************************************
 * SHOW FULLSCREEN GIF (MODE A)
 ********************************************************************/
static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;

    close_gif_window(); // in case previous GIF still exists

    gif_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_fullscreen(GTK_WINDOW(gif_window));
    gtk_window_set_decorated(GTK_WINDOW(gif_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(gif_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(gif_window), TRUE);

    gif_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(gif_area, TRUE);
    gtk_widget_set_vexpand(gif_area, TRUE);

    gtk_container_add(GTK_CONTAINER(gif_window), gif_area);
    gtk_widget_show_all(gif_window);

    gif_player = calloc(1, sizeof(GifPlayer));
    gif_player->timer = g_timer_new();

    GError *err = NULL;
    gif_player->anim = gdk_pixbuf_animation_new_from_file(filename, &err);
    if (!gif_player->anim) {
        g_printerr("GIF load error: %s\n", err->message);
        close_gif_window();
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->anim, NULL);

    g_signal_connect(gif_area, "draw", G_CALLBACK(gif_draw), NULL);
    gif_player->timeout_id = g_timeout_add(10, gif_advance, NULL);

    // Clicking or pressing any key closes gif
    g_signal_connect(gif_window, "button-press-event", G_CALLBACK(close_gif_window), NULL);
    g_signal_connect(gif_window, "key-press-event",    G_CALLBACK(close_gif_window), NULL);

    return FALSE;
}

/********************************************************************
 * TOKEN RENDERER (Cairo + Pango)
 ********************************************************************/
static GdkPixbuf *render_token_pixbuf(GtkWidget *widget,
                                      const char *number, const char *label,
                                      const char *bg_hex, const char *fg_hex,
                                      double number_frac, double label_frac,
                                      const char *num_font, const char *lab_font)
{
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w < 50) w = 600;
    if (h < 50) h = 300;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);

    unsigned int r, g, b;
    double br = 1, bg = 1, bb = 1;
    sscanf(bg_hex, "#%02x%02x%02x", &r, &g, &b);
    br = r/255.0; bg = g/255.0; bb = b/255.0;

    cairo_set_source_rgb(cr, br, bg, bb);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    char fontdesc[128];

    /* NUMBER */
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", num_font, (int)(h * number_frac));
    PangoFontDescription *fdn = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout, fdn);

    pango_layout_set_text(layout, number, -1);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_move_to(cr, (w - tw)/2, h * 0.28);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fdn);

    /* LABEL */
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", lab_font, (int)(h * label_frac));
    PangoFontDescription *fdl = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout, fdl);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, (w - tw)/2, h * 0.80);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fdl);
    g_object_unref(layout);

    cairo_surface_flush(surf);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    return pix;
}

static gboolean refresh_images_on_ui(gpointer data) {
    gtk_image_set_from_pixbuf(GTK_IMAGE(current_image),
        render_token_pixbuf(current_image, current_token, "Current Draw",
                            "#FFDAB9", "#FF0000", 0.7, 0.12,
                            "Sans Bold", "Sans"));

    gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image),
        render_token_pixbuf(previous_image, previous_token, "Previous Draw",
                            "#FFDAB9", "#0000FF", 0.45, 0.1,
                            "Sans Bold", "Sans"));

    gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image),
        render_token_pixbuf(preceding_image, preceding_token, "Preceding Draw",
                            "#FFDAB9", "#8B4513", 0.6, 0.12,
                            "Sans Bold", "Sans"));

    return FALSE;
}

/********************************************************************
 * SERIAL THREAD
 ********************************************************************/
static void *serial_reader_thread(void *arg) {
    int fd = open("/dev/serial0", O_RDWR | O_NOCTTY);
    if (fd < 0) return NULL;

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B9600);
    tcsetattr(fd, TCSANOW, &t);

    char buf[64];
    int pos = 0;

    while (1) {
        char c;
        if (read(fd, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                buf[pos] = 0;
                pos = 0;

                if (strcmp(buf, "G1") == 0) {
                    g_idle_add(show_fullscreen_gif, "gameover1.gif");
                    strcpy(current_token, "--");
                    strcpy(previous_token, "--");
                    strcpy(preceding_token, "--");
                    g_idle_add(refresh_images_on_ui, NULL);
                }
                else if (strcmp(buf, "C1") == 0) {
                    g_idle_add(show_fullscreen_gif, "congratulations1.gif");
                }
                else {
                    shift_tokens(buf);
                    g_idle_add(refresh_images_on_ui, NULL);
                }
            }
            else if (pos < sizeof(buf)-1) buf[pos++] = c;
        }
        usleep(20000);
    }
}

/********************************************************************
 * MAIN
 ********************************************************************/
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkBuilder *b = gtk_builder_new_from_file("interface_paned_overlay.glade");

    window = GTK_WIDGET(gtk_builder_get_object(b, "main"));
    top_label = GTK_WIDGET(gtk_builder_get_object(b, "top_label"));
    current_image = GTK_WIDGET(gtk_builder_get_object(b, "current_image"));
    previous_image = GTK_WIDGET(gtk_builder_get_object(b, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(b, "preceding_image"));
    top_pane = GTK_WIDGET(gtk_builder_get_object(b, "top_pane"));
    outermost = GTK_WIDGET(gtk_builder_get_object(b, "outermost"));
    outer = GTK_WIDGET(gtk_builder_get_object(b, "outer"));
    inner = GTK_WIDGET(gtk_builder_get_object(b, "inner"));
    ticker_fixed = GTK_WIDGET(gtk_builder_get_object(b, "ticker_fixed"));
    ticker_label = GTK_WIDGET(gtk_builder_get_object(b, "ticker_label"));

    gtk_widget_show_all(window);
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    pthread_t tid;
    pthread_create(&tid, NULL, serial_reader_thread, NULL);
    pthread_detach(tid);

    gtk_main();
    return 0;
}
