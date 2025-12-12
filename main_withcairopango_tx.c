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

/***********************************************************
 * GLOBALS
 ***********************************************************/
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

/* NEW — created in code */
GtkWidget *root_overlay = NULL;
GtkWidget *gif_overlay  = NULL;

/***********************************************************
 * GIF PLAYER STRUCT
 ***********************************************************/
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GTimer *timer;
} GifPlayer;

static GifPlayer *gif_player = NULL;

/***********************************************************
 * TOKENS
 ***********************************************************/
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

/***********************************************************
 * UTILS
 ***********************************************************/
static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

/***********************************************************
 * GIF PLAYER DRAW
 ***********************************************************/
static gboolean gif_player_advance(gpointer data) {
    if (!gif_player || !gif_player->iter) return G_SOURCE_REMOVE;

    gdouble elapsed_ms = g_timer_elapsed(gif_player->timer, NULL) * 1000.0;
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    if (delay < 0) delay = 100;

    if (elapsed_ms >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_overlay);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_player || !gif_player->iter) return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;

    int da_w = gtk_widget_get_allocated_width(widget);
    int da_h = gtk_widget_get_allocated_height(widget);

    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

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

/***********************************************************
 * GIF CLEANUP
 ***********************************************************/
static void gif_player_cleanup(void) {
    if (!gif_player) return;

    if (gif_player->timeout_id) g_source_remove(gif_player->timeout_id);
    if (gif_player->animation) g_object_unref(gif_player->animation);
    if (gif_player->iter) g_object_unref(gif_player->iter);
    if (gif_player->timer) g_timer_destroy(gif_player->timer);

    g_free(gif_player);
    gif_player = NULL;

    gtk_widget_hide(gif_overlay);
}

/***********************************************************
 * SHOW GIF (OVERLAY)
 ***********************************************************/
static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = filename_ptr;

    if (gif_player) gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);
    gif_player->timer = g_timer_new();

    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (!gif_player->animation) {
        if (error) g_printerr("GIF Error: %s\n", error->message);
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);

    gtk_widget_show(gif_overlay);
    gtk_widget_grab_focus(gif_overlay);

    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);
    return FALSE;
}

/***********************************************************
 * HIDE GIF
 ***********************************************************/
static gboolean hide_overlay_gif(gpointer data) {
    gif_player_cleanup();
    gtk_widget_queue_draw(window);
    return FALSE;
}

/***********************************************************
 * TOKEN RENDERER (Cairo + Pango)
 ***********************************************************/
static GdkPixbuf *render_token_pixbuf(
    GtkWidget *widget, const char *number, const char *label,
    const char *bg_hex, const char *fg_hex,
    double number_frac, double label_frac,
    const char *num_font, const char *lab_font)
{
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w < 100 || h < 100) { w = 600; h = 300; }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);

    unsigned int r, g, b;
    double br = 1, bg = 1, bb = 1;

    if (sscanf(bg_hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        br = r/255.0, bg = g/255.0, bb = b/255.0;

    cairo_set_source_rgb(cr, br, bg, bb);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    /* NUMBER */
    char fontdesc[128];
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", num_font, (int)(h * number_frac));
    PangoFontDescription *fdn = pango_font_description_from_string(fontdesc);

    pango_layout_set_font_description(layout, fdn);
    pango_layout_set_text(layout, number ? number : "--", -1);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_move_to(cr, (w - tw)/2, (h - th)/2);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fdn);

    /* LABEL */
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", lab_font, (int)(h * label_frac));
    PangoFontDescription *fdl = pango_font_description_from_string(fontdesc);

    pango_layout_set_font_description(layout, fdl);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, (w - tw)/2, h * 0.8);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fdl);

    g_object_unref(layout);

    cairo_surface_flush(surf);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    return pix;
}

/***********************************************************
 * APPLY TOKEN IMAGES
 ***********************************************************/
static gboolean refresh_images_on_ui(gpointer user_data) {
    GdkPixbuf *pb1 = render_token_pixbuf(current_image, current_token,
                                         "Current Draw", "#FFDAB9", "#FF0000",
                                         0.7, 0.12, "Sans Bold", "Sans");

    GdkPixbuf *pb2 = render_token_pixbuf(previous_image, previous_token,
                                         "Previous Draw", "#FFDAB9", "#0000FF",
                                         0.45, 0.10, "Sans Bold", "Sans");

    GdkPixbuf *pb3 = render_token_pixbuf(preceding_image, preceding_token,
                                         "Preceding Draw", "#FFDAB9", "#8B4513",
                                         0.6, 0.12, "Sans Bold", "Sans");

    gtk_image_set_from_pixbuf(GTK_IMAGE(current_image), pb1);
    gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image), pb2);
    gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image), pb3);

    g_object_unref(pb1);
    g_object_unref(pb2);
    g_object_unref(pb3);

    return FALSE;
}

/***********************************************************
 * SERIAL THREAD
 ***********************************************************/
static void *serial_reader_thread(void *arg) {
    const char *port = "/dev/serial0";
    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) return NULL;

    struct termios options;
    tcgetattr(fd, &options);
    cfmakeraw(&options);
    cfsetispeed(&options, B9600);
    cfsetospeed(&options, B9600);
    tcsetattr(fd, TCSANOW, &options);

    char buf[128];
    int pos = 0;

    while (1) {
        char c;
        if (read(fd, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                buf[pos] = 0;
                pos = 0;

                if (strcmp(buf, "G1") == 0) {
                    gtk_widget_hide(gif_overlay);
                    g_idle_add(show_fullscreen_gif, "gameover1.gif");
                    strcpy(current_token, "--");
                    strcpy(previous_token, "--");
                    strcpy(preceding_token, "--");
                    g_idle_add(refresh_images_on_ui, NULL);
                }
                else if (strcmp(buf, "C1") == 0) {
                    gtk_widget_hide(gif_overlay);
                    g_idle_add(show_fullscreen_gif, "congratulations1.gif");
                }
                else {
                    gtk_widget_hide(gif_overlay);
                    shift_tokens(buf);
                    g_idle_add(refresh_images_on_ui, NULL);
                }
            }
            else if (pos < sizeof(buf)-1) buf[pos++] = c;
        }
        usleep(10000);
    }
}

/***********************************************************
 * MAIN
 ***********************************************************/
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_overlay.glade");
    window        = GTK_WIDGET(gtk_builder_get_object(builder, "main"));
    top_label     = GTK_WIDGET(gtk_builder_get_object(builder, "top_label"));
    current_image = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image= GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image=GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    top_pane      = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));
    outermost     = GTK_WIDGET(gtk_builder_get_object(builder, "outermost"));
    outer         = GTK_WIDGET(gtk_builder_get_object(builder, "outer"));
    inner         = GTK_WIDGET(gtk_builder_get_object(builder, "inner"));
    ticker_fixed  = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label  = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));

    /* WRAP UI IN OVERLAY */
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(window));
    g_object_ref(child);
    gtk_container_remove(GTK_CONTAINER(window), child);

    root_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), root_overlay);
    gtk_container_add(GTK_CONTAINER(root_overlay), child);

    gif_overlay = gtk_drawing_area_new();
    gtk_widget_set_visible(gif_overlay, FALSE);
    gtk_widget_set_hexpand(gif_overlay, TRUE);
    gtk_widget_set_vexpand(gif_overlay, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay), gif_overlay);

    g_signal_connect(gif_overlay, "draw", G_CALLBACK(gif_player_draw), NULL);

    gtk_widget_show_all(window);
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    /* START SERIAL THREAD */
    pthread_t tid;
    pthread_create(&tid, NULL, serial_reader_thread, NULL);
    pthread_detach(tid);

    gtk_main();
    return 0;
}
