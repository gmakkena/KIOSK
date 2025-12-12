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

// ===================== GLOBAL SERIAL =====================
int serial_fd = -1;
pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

static gulong gif_draw_handler_id = 0;
static gboolean gif_playing = FALSE;  

// ==========================================
// Read KEY="value" from config
// ==========================================
static char *read_config_value(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char *line = NULL;
    size_t len = 0;
    char *result = NULL;
    size_t keylen = strlen(key);

    while (getline(&line, &len, f) != -1) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            char *val = p + keylen + 1;
            while (*val == ' ' || *val == '\t') val++;
            char *end = val + strlen(val);
            while (end > val && (*(end - 1) == '\n' || *(end - 1) == '\r' ||
                                 *(end - 1) == ' ' || *(end - 1) == '\t'))
                end--;
            *end = '\0';

            if ((val[0] == '"' && end > val + 1 && *(end - 1) == '"') ||
                (val[0] == '\'' && end > val + 1 && *(end - 1) == '\'')) {
                val++;
                *(end - 1) = '\0';
            }
            result = strdup(val);
            break;
        }
    }

    free(line);
    fclose(f);
    return result;
}

// ===================== Widgets =====================
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;
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

// ===================== Tokens =====================
char current_token[32] = "--";
char previous_token[32] = "--";
char preceding_token[32] = "--";

// ===================== Helper =====================
static void refocus_main_window(GtkWidget *win) {
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_present(GTK_WINDOW(win));
        if (gtk_widget_get_window(win))
            gdk_window_raise(gtk_widget_get_window(win));
    }
}

// ===================== Serial TX =====================
static void serial_send(const char *msg) {
    if (serial_fd < 0) {
        g_printerr("Serial not open — cannot send\n");
        return;
    }

    pthread_mutex_lock(&serial_lock);
    write(serial_fd, msg, strlen(msg));
    pthread_mutex_unlock(&serial_lock);
}

// ===================== GIF HANDLERS =====================
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

static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    // if not playing — do nothing (allow underlying widgets to show)
    if (!gif_playing || !gif_player || !gif_player->iter || !gif_player->animation)
        return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;

    int da_w = gtk_widget_get_allocated_width(widget);
    int da_h = gtk_widget_get_allocated_height(widget);
    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

    // paint black only while playing
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

    // stop timeout if present
    if (gif_player->timeout_id) {
        g_source_remove(gif_player->timeout_id);
        gif_player->timeout_id = 0;
    }

    // unref animation and iter
    if (gif_player->animation) {
        g_object_unref(gif_player->animation);
        gif_player->animation = NULL;
    }
    if (gif_player->iter) {
        g_object_unref(gif_player->iter);
        gif_player->iter = NULL;
    }
    if (gif_player->timer) {
        g_timer_destroy(gif_player->timer);
        gif_player->timer = NULL;
    }

    g_free(gif_player);
    gif_player = NULL;

    // disconnect draw handler so it will no longer be invoked (safe if handler id = 0)
    if (gif_draw_handler_id != 0 && gif_area && G_IS_OBJECT(gif_area)) {
        // ignore result — may already be disconnected
        g_signal_handler_disconnect(gif_area, gif_draw_handler_id);
        gif_draw_handler_id = 0;
    }

    // mark as not playing before we return
    gif_playing = FALSE;
}

static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;

    // -----------------------------------------------
    // If gif_area was removed earlier → recreate it
    // -----------------------------------------------
    if (!gif_area) {
        gif_area = gtk_drawing_area_new();
        gtk_widget_set_app_paintable(gif_area, TRUE);
        gtk_widget_set_hexpand(gif_area, TRUE);
        gtk_widget_set_vexpand(gif_area, TRUE);

        // locate overlay; window's main child is GtkOverlay
        GtkWidget *main_child = gtk_bin_get_child(GTK_BIN(window));
        if (GTK_IS_OVERLAY(main_child)) {
            gtk_overlay_add_overlay(GTK_OVERLAY(main_child), gif_area);
        } else {
            g_printerr("ERROR: main child is not GtkOverlay\n");
            return FALSE;
        }
    }

    // -----------------------------------------------
    // Cleanup previous animation completely
    // -----------------------------------------------
    gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);
    gif_player->drawing_area = gif_area;

    // Load GIF
    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (error || !gif_player->animation) {
        g_printerr("GIF Load Error: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        gif_player_cleanup();
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();
    g_timer_start(gif_player->timer);

    gif_playing = TRUE;

    // -----------------------------------------------
    // Connect draw handler ONLY once
    // -----------------------------------------------
    if (gif_draw_handler_id == 0) {
        gif_draw_handler_id = g_signal_connect(gif_area, "draw",
                                               G_CALLBACK(gif_player_draw), NULL);
    }

    gtk_widget_show(gif_area);
    gtk_widget_queue_draw(gif_area);
    gtk_widget_grab_focus(gif_area);

    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);

    refocus_main_window(window);
    return FALSE;
}

static gboolean hide_overlay_gif(gpointer user_data) {

    gif_playing = FALSE;
    gif_player_cleanup();

    if (gif_area) {
        gtk_widget_hide(gif_area);

        GtkWidget *parent = gtk_widget_get_parent(gif_area);
        if (parent) {
            gtk_container_remove(GTK_CONTAINER(parent), gif_area);
        }
        gif_area = NULL;
    }

    g_idle_add(refresh_images_on_ui, NULL);
    refocus_main_window(window);
    return FALSE;
}

// ===================== TOKEN RENDER =====================
static GdkPixbuf *render_token_pixbuf(GtkWidget *widget,
                                     const char *number, const char *label,
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
    double br = 1, bgc = 1, bb = 1;

    if (bg_hex && sscanf(bg_hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        br = r/255.0, bgc = g/255.0, bb = b/255.0;

    cairo_set_source_rgb(cr, br, bgc, bb);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    char fontdesc[128];

    // NUMBER
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", num_font, (int)(h * number_frac));
    PangoFontDescription *fdn = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout, fdn);
    pango_layout_set_text(layout, number ? number : "--", -1);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    int tx = (w - tw) / 2;
    int ty = (h - th) / 2 - (h * 0.05);

    double fr = 1, fg = 0, fb = 0;

    if (fg_hex && sscanf(fg_hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        fr = r/255.0, fg = g/255.0, fb = b/255.0;

    cairo_set_source_rgb(cr, fr, fg, fb);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fdn);

    // LABEL
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", lab_font, (int)(h * label_frac));
    PangoFontDescription *fdl = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout, fdl);
    pango_layout_set_text(layout, label ? label : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    tx = (w - tw) / 2;
    ty = (h * 0.8);

   cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
//cairo_set_source_rgb(cr, 0.255, 0.412, 0.882);
   cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fdl);
    g_object_unref(layout);

    cairo_surface_flush(surf);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    return pix;
}

static gboolean refresh_images_on_ui(gpointer user_data) {
    GdkPixbuf *pb1 = render_token_pixbuf(current_image, current_token, "Current Draw",
                                         "#FFDAB9", "#FF0000", 0.7, 0.15,
                                         "Liberation Sans Bold", "Liberation Sans");

    GdkPixbuf *pb2 = render_token_pixbuf(previous_image, previous_token, "Previous Draw",
                                         "#FFDAB9", "#0000FF", 0.45, 0.08,
                                         "Liberation Sans Bold", "Liberation Sans");

    GdkPixbuf *pb3 = render_token_pixbuf(preceding_image, preceding_token, "Preceding Draw",
                                         "#FFDAB9", "#8B4513", 0.6, 0.12,
                                         "Liberation Sans Bold", "Liberation Sans");

    if (pb1) { gtk_image_set_from_pixbuf(GTK_IMAGE(current_image), pb1); g_object_unref(pb1); }
    if (pb2) { gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image), pb2); g_object_unref(pb2); }
    if (pb3) { gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image), pb3); g_object_unref(pb3); }

    return FALSE;
}

// ===================== PANED RATIOS =====================
static gboolean set_paned_ratios(gpointer user_data) {
    gtk_paned_set_wide_handle(GTK_PANED(top_pane), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outermost), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outer), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(inner), FALSE);

    GtkAllocation top_alloc, outermost_alloc, outer_alloc, inner_alloc;
    gtk_widget_get_allocation(top_pane, &top_alloc);
    gtk_widget_get_allocation(outermost, &outermost_alloc);
    gtk_widget_get_allocation(outer, &outer_alloc);
    gtk_widget_get_allocation(inner, &inner_alloc);

    if (top_alloc.height == 0) top_alloc.height = 100;
    if (outermost_alloc.height == 0) outermost_alloc.height = 800;
    if (outer_alloc.width == 0) outer_alloc.width = 1200;
    if (inner_alloc.height == 0) inner_alloc.height = 600;

    gtk_paned_set_position(GTK_PANED(top_pane), top_alloc.height * 0.12);
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.88);
    gtk_paned_set_position(GTK_PANED(outer), outer_alloc.width * 0.72);
    gtk_paned_set_position(GTK_PANED(inner), inner_alloc.height * 0.70);

    gtk_widget_show(top_label);
    gtk_widget_show(ticker_fixed);
    gtk_widget_show(ticker_label);

    int top_font_size = (int)(outermost_alloc.height * 0.08 * 0.9 * PANGO_SCALE);
    int ticker_font_size = (int)(outermost_alloc.height * 0.045 * 0.9 * PANGO_SCALE);

    const char *plain = gtk_label_get_text(GTK_LABEL(top_label));
    if (!plain) plain = "";

    char *escaped = g_markup_escape_text(plain, -1);

    char markup_top[512];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Fira Sans' weight='bold' size='%d' foreground='#8B0000'>%s</span>",
        top_font_size, escaped);

    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);
    g_free(escaped);

    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        ticker_font_size);

    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);

    g_idle_add(refresh_images_on_ui, NULL);

    return G_SOURCE_REMOVE;
}

// ===================== Serial Token Logic =====================
static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

static gboolean update_ui_from_serial(gpointer user_data) {
     

    g_idle_add(refresh_images_on_ui, NULL);
    return FALSE;
}

// ===================== Serial Reader Thread =====================
static void *serial_reader_thread(void *arg) {
    char buf[256];
    size_t pos = 0;
    char rbuf[64];

    while (1) {
        int n = read(serial_fd, rbuf, sizeof(rbuf));

        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = rbuf[i];

                if (c == '\r' || c == '\n') {
                    if (pos == 0) continue;

                    buf[pos] = '\0';
                    pos = 0;

                    char *p = buf;
                    while (*p == ' ') p++;

                    char *save = NULL;
                    char *f0 = strtok_r(p, " ", &save);
                    char *f1 = strtok_r(NULL, " ", &save);
                    char *f2 = strtok_r(NULL, " ", &save);

                    if (f0 && f0[0] == ':') {
                        if (f1 && f2 && strcmp(f1, "1") == 0) {
                            shift_tokens(f2);
                            g_idle_add(update_ui_from_serial, NULL);
                        } else if (f1 && strcmp(f1, "3") == 0) {
                            g_idle_add(show_fullscreen_gif, (gpointer)"rolling.gif");
                        }
                    }
                    else if (f0 && strcmp(f0, "$M") == 0 && f1) {
                        if (!strcmp(f1, "G1")) {
                            g_idle_add(show_fullscreen_gif, (gpointer)"gameover1.gif");
                            strcpy(current_token, "--");
                            strcpy(previous_token, "--");
                            strcpy(preceding_token, "--");
                            g_idle_add(update_ui_from_serial, NULL);
                        } else if (!strcmp(f1, "C1")) {
                            g_idle_add(show_fullscreen_gif, (gpointer)"congratulations1.gif");
                        } else {
                            g_idle_add(hide_overlay_gif, NULL);
                        }
                    }
                    else if (f0) {
                         if (gif_playing) {
        g_idle_add(hide_overlay_gif, NULL);
    }

    shift_tokens(f0);
    g_idle_add(update_ui_from_serial, NULL);
                    }

                } else if (pos + 1 < sizeof(buf)) {
                    buf[pos++] = c;
                }
            }
        }
        else {
            usleep(20000);
        }
    }

    return NULL;
}

// ===================== Serial TX Thread (send HDMI every 1 sec) =====================
static void *serial_tx_thread(void *arg) {
    while (1) {
        serial_send("hdmi\n");   // 🚀 send every 1 second
        usleep(1000000);
    }
    return NULL;
}

// ===================== MAIN =====================
int main(int argc,char *argv[]) {
    gtk_init(&argc,&argv);

    // ---------------- Serial Setup ----------------
    const char *serial_port = "/dev/serial0";

    serial_fd = open(serial_port, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        perror("Failed to open serial port");
        return 1;
    }

    struct termios options;
    tcgetattr(serial_fd, &options);

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

    options.c_cc[VMIN]  = 0;
    options.c_cc[VTIME] = 1;

    tcsetattr(serial_fd, TCSANOW, &options);

    // ---------------- GTK Setup ----------------
    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_overlay.glade");

    window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));
    top_label = GTK_WIDGET(gtk_builder_get_object(builder, "top_label"));
    current_image = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    top_pane = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));
    outermost = GTK_WIDGET(gtk_builder_get_object(builder, "outermost"));
    outer = GTK_WIDGET(gtk_builder_get_object(builder, "outer"));
    inner = GTK_WIDGET(gtk_builder_get_object(builder, "inner"));
    ticker_fixed = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));
    gif_area = GTK_WIDGET(gtk_builder_get_object(builder, "gif_area"));

    char *cfg_label = read_config_value("/boot/firmware/aurum.txt", "AURUM_TOP_LABEL");
    if (cfg_label) {
        gtk_label_set_text(GTK_LABEL(top_label), cfg_label);
        g_print("Loaded top label from config: %s\n", cfg_label);
        free(cfg_label);
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    gtk_widget_show_all(window);

    GdkScreen *screen = gdk_screen_get_default();
    gtk_window_resize(GTK_WINDOW(window), gdk_screen_get_width(screen), gdk_screen_get_height(screen));
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    g_timeout_add(200, set_paned_ratios, NULL);

    gtk_widget_set_visible(gif_area, FALSE);
    gtk_widget_hide(ticker_fixed);
    gtk_widget_hide(ticker_label);

    // ---------------- Start Threads ----------------
    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    pthread_t tx_thread;
    pthread_create(&tx_thread, NULL, serial_tx_thread, NULL);
    pthread_detach(tx_thread);

    gtk_main();
    return 0;
}
