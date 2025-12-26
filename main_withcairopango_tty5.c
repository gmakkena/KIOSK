// ==========================
//  TOKEN DISPLAY + GIF OVERLAY (MODE A)
//  FIXED: GIF Auto-Hide After Completion
//  FIXED: No flashing on bulk token arrivals
//  ENHANCED: TTY5 "Please wait..." display during bulk loading
// ==========================

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
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

// ===================== GLOBAL SERIAL =====================
int serial_fd = -1;
pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

// Forward declaration (required)
static gboolean refresh_images_on_ui(gpointer user_data);

// ===================== GIF CONTROL FLAGS =====================
static gboolean gif_playing = FALSE;
static gulong gif_draw_handler_id = 0;

// ===================== Widgets =====================
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;
GtkWidget *gif_area = NULL;
static gboolean tty2_active = FALSE;
static gboolean tty4_active = FALSE;
static gboolean tty5_active = FALSE;  // NEW: TTY5 for "Please wait..."
int ticker_x = 0;
int ticker_width = 0;
int ticker_area_width = 0;
guint ticker_timer_id = 0;

static int flash_count = 0;
static guint flash_timer_id = 0;   
static guint flash_delay_id = 0;   
static gboolean number_visible = TRUE;

// ===================== BULK TOKEN DETECTION & STATE =====================
static struct timespec last_token_time = {0, 0};
#define BULK_TOKEN_THRESHOLD_MS 500  // Tokens within 500ms = bulk
#define BULK_FINISH_DELAY_MS 4000    // Wait 4 seconds after last token before finishing bulk
static gboolean bulk_loading = FALSE;
static gboolean first_token_received = FALSE;
static guint bulk_finish_timer_id = 0;
static gboolean first_ever_token = TRUE;  // Track very first token for startup flash

// ===================== GIF Player Struct =====================
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GTimer *timer;
} GifPlayer;

static GifPlayer *gif_player = NULL;

// ===================== TOKENS =====================
char current_token[32] = "--";
char previous_token[32] = "--";
char preceding_token[32] = "--";

static gboolean flash_opacity_callback(gpointer data) {
    if (flash_count >= 6) { 
        number_visible = TRUE; // Ensure visible at the end
        refresh_images_on_ui(NULL);
        flash_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    // Toggle the number visibility flag and redraw the canvas
    number_visible = !number_visible;
    refresh_images_on_ui(NULL);

    flash_count++;
    return G_SOURCE_CONTINUE;
}

static gboolean trigger_flash_after_delay(gpointer data) {
    flash_delay_id = 0;
    flash_count = 0;
    if (flash_timer_id > 0) g_source_remove(flash_timer_id);
    
    // Pulse every 400ms for a snappier look
    flash_timer_id = g_timeout_add(400, flash_opacity_callback, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== TTY5 PLEASE WAIT HELPERS =====================
static void show_please_wait_tty5(void) {
    g_print("Switching to TTY5 (Please wait...)\n");
    tty5_active = TRUE;
    system("sudo chvt 5");
}

static void hide_please_wait_return_tty1(void) {
    if (tty5_active) {
        g_print("Returning to TTY1 from TTY5\n");
        system("sudo chvt 1");
        usleep(150000);
        system("sudo chvt 1");  // Double switch for reliability
        tty5_active = FALSE;
    }
}

// ===================== BULK LOADING FINISH HANDLER =====================
static gboolean finish_bulk_loading(gpointer data) {
    bulk_loading = FALSE;
    bulk_finish_timer_id = 0;
    
    // Return to TTY1 from TTY5
    hide_please_wait_return_tty1();
    
    // Show token display area and token widgets
    if (outermost)
        gtk_widget_show(outermost);
    gtk_widget_show(current_image);
    gtk_widget_show(previous_image);
    gtk_widget_show(preceding_image);
    
    // Refresh token images to display actual numbers
    number_visible = TRUE;
    g_idle_add(refresh_images_on_ui, NULL);
    
    // If this is the very first token on startup, flash it
    if (first_ever_token) {
        first_ever_token = FALSE;  // Clear flag
        
        if (flash_timer_id > 0) {
            g_source_remove(flash_timer_id);
            flash_timer_id = 0;
        }
        if (flash_delay_id > 0)
            g_source_remove(flash_delay_id);
        
        flash_delay_id = g_timeout_add(300, trigger_flash_after_delay, NULL);
    }
    // Otherwise it's history reload - no flash
    
    return G_SOURCE_REMOVE;
}

// ===================== CONFIG READER =====================
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
            while (end > val &&
                  (*(end - 1) == '\n' ||
                   *(end - 1) == '\r' ||
                   *(end - 1) == ' ' ||
                   *(end - 1) == '\t'))
                end--;

            *end = '\0';

            if ((val[0] == '"' && *(end - 1) == '"') ||
                (val[0] == '\' && *(end - 1) == '\'')) {
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

// ===================== WINDOW FOCUS HELPER =====================
static void refocus_main_window(GtkWidget *win) {
    if (GTK_IS_WINDOW(win)) {
        gtk_window_present(GTK_WINDOW(win));
        if (gtk_widget_get_window(win))
            gdk_window_raise(gtk_widget_get_window(win));
    }
}

// ===================== SERIAL SENDER =====================
static void serial_send(const char *msg) {
    if (serial_fd < 0) return;

    pthread_mutex_lock(&serial_lock);
    write(serial_fd, msg, strlen(msg));
    pthread_mutex_unlock(&serial_lock);
}

static void clear_tokens(void)
{
    strncpy(current_token,   "--", sizeof(current_token));
    strncpy(previous_token,  "--", sizeof(previous_token));
    strncpy(preceding_token, "--", sizeof(preceding_token));
}

// ===========================================================
//                GIF PLAYER IMPLEMENTATION
// ===========================================================

static gboolean gif_player_advance(gpointer data) {
    if (!gif_player || !gif_player->iter || !gif_playing)
        return G_SOURCE_REMOVE;

    double elapsed_ms = g_timer_elapsed(gif_player->timer, NULL) * 1000.0;
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    
    if (delay < 0) delay = 100; // Default delay for static images or end of animation
    
    if (elapsed_ms >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_area);
        g_timer_start(gif_player->timer);
    }
    
    return G_SOURCE_CONTINUE;
}

static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_playing || !gif_player || !gif_player->animation)
        return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    double scale = (double)H / fh;
    int scaled_w = fw * scale;
    int x_offset = (W - scaled_w) / 2;

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

    if (gif_player->timeout_id)
        g_source_remove(gif_player->timeout_id);

    if (gif_player->animation)
        g_object_unref(gif_player->animation);

    if (gif_player->iter)
        g_object_unref(gif_player->iter);

    if (gif_player->timer)
        g_timer_destroy(gif_player->timer);

    g_free(gif_player);
    gif_player = NULL;
}

// ===================== HIDE GIF (MODE A) =====================
static gboolean hide_overlay_gif(gpointer user_data) {
    g_print("Hiding GIF overlay...\n");

    gif_playing = FALSE;

    // Stop animation timeouts
    gif_player_cleanup();

    // Disconnect draw handler (VERY important)
    if (gif_draw_handler_id != 0) {
        g_signal_handler_disconnect(gif_area, gif_draw_handler_id);
        gif_draw_handler_id = 0;
    }

    // Force overlay to stop drawing
    gtk_widget_set_no_show_all(gif_area, TRUE);
    gtk_widget_hide(gif_area);
    gtk_widget_set_no_show_all(gif_area, FALSE);

    // Make sure the underlying token widgets are visible
    gtk_widget_show(current_image);
    gtk_widget_show(previous_image);
    gtk_widget_show(preceding_image);
    gtk_widget_show(top_label);
    gtk_widget_show(ticker_fixed);
    gtk_widget_show(ticker_label);

    // Force overlay stack to redraw from bottom up
    gtk_widget_queue_draw(window);
    gtk_widget_queue_draw(gif_area);
    gtk_widget_queue_draw(top_pane);

    // Regenerate token images
    g_idle_add(refresh_images_on_ui, NULL);

    return FALSE;
}

// ===================== TTY2 GIF PLAYER =====================
static void play_gif_on_tty2(const char *gifpath) {
    char cmd[512];

    // Kill any previous mpv on tty2
    system("killall -q mpv");

    // Switch to tty2 first
    system("sudo chvt 2");

    // Build MPV command (loop forever, no audio, direct framebuffer)
    snprintf(cmd, sizeof(cmd),
        "mpv --really-quiet --loop --no-audio "
        "--vo=tct --force-window=no '%s' > /dev/tty2 2>&1 &",
        gifpath
    );

    system(cmd);

    // Note: We do NOT return to tty1 here.
    // We stay on tty2 until the next token arrives.
}


// ===================== SHOW GIF (MODE A) =====================
static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = filename_ptr;
    
    g_print("Loading GIF: %s\n", filename);

    gif_playing = TRUE;

    // Clean previous animation
    gif_player_cleanup();
    gif_player = g_new0(GifPlayer, 1);

    // Load GIF
    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (!gif_player->animation) {
        g_printerr("GIF load error: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        gif_playing = FALSE;
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();
    g_timer_start(gif_player->timer);

    // Install draw handler once
    if (gif_draw_handler_id == 0) {
        gif_draw_handler_id =
            g_signal_connect(gif_area, "draw", G_CALLBACK(gif_player_draw), NULL);
    }

    gtk_widget_show(gif_area);
    gtk_widget_queue_draw(gif_area);

    // Start animation loop - will keep looping until interrupted
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);
    
    return FALSE;
}

// ===========================================================
//                TOKEN RENDERING (CAIRO + PANGO)
// ===========================================================

static void set_cairo_color(cairo_t *cr, const char *hex)
{
    unsigned r, g, b;
    if (hex && sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3) {
        cairo_set_source_rgb(cr, r / 255.0, g / 255.0, b / 255.0);
    }
}

// Render token with number or "--"
static GdkPixbuf *render_token_pixbuf_cairo(GtkWidget *widget, 
                                            const char *number, 
                                            const char *label, 
                                            const char *bg_hex, 
                                            const char *num_hex, 
                                            const char *lab_hex, 
                                            double number_size_frac, 
                                            double label_size_frac, 
                                            double number_x_frac, 
                                            double number_y_frac, 
                                            double label_x_frac, 
                                            double label_y_frac, 
                                            const char *num_font, 
                                            const char *lab_font, 
                                            gboolean show_number) {
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w < 100 || h < 100) { w = 600; h = 300; }
    
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);
    
    set_cairo_color(cr, bg_hex);
    cairo_paint(cr);
    
    PangoLayout *layout = pango_cairo_create_layout(cr);
    char fontdesc[128];
    int tw, th;

    /* Draw NUMBER - Only if show_number is TRUE */
    if (show_number) {
        set_cairo_color(cr, num_hex);
        snprintf(fontdesc, sizeof(fontdesc), "%s %d", num_font, (int)(h * number_size_frac));
        PangoFontDescription *fd = pango_font_description_from_string(fontdesc);
        pango_layout_set_font_description(layout, fd);
        pango_layout_set_text(layout, number ? number : "--", -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_move_to(cr, (w / 2 + (int)(w * number_x_frac)) - tw / 2, 
                         (h / 2 + (int)(h * number_y_frac)) - th / 2);
        pango_cairo_show_layout(cr, layout);
        pango_font_description_free(fd);
    }

    /* Draw LABEL - Always shown */
    set_cairo_color(cr, lab_hex);
    snprintf(fontdesc, sizeof(fontdesc), "%s %d", lab_font, (int)(h * label_size_frac));
    PangoFontDescription *fd_lab = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout, fd_lab);
    pango_layout_set_text(layout, label ? label : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, (w / 2 + (int)(w * label_x_frac)) - tw / 2, 
                     (h / 2 + (int)(h * label_y_frac)) - th / 2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd_lab);

    g_object_unref(layout);
    cairo_surface_flush(surface);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return pix;
}


gboolean animate_ticker(gpointer data) {
    ticker_x -= 2;

    if (ticker_x + ticker_width < 0)
        ticker_x = ticker_area_width;

    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    return G_SOURCE_CONTINUE;
}

gboolean finalize_ticker_setup(gpointer data) {
    // Get screen width instead of container width
    GdkScreen *screen = gdk_screen_get_default();
    ticker_area_width = gdk_screen_get_width(screen);
    
    // Measure label
    GtkRequisition label_req;
    gtk_widget_get_preferred_size(ticker_label, NULL, &label_req);
    ticker_width = label_req.width;

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    // Start off-screen right
    ticker_x = ticker_area_width;
    
    // Lock label size
    gtk_widget_set_size_request(ticker_label, ticker_width, 60);

    if (ticker_timer_id == 0)
        ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);

    return G_SOURCE_REMOVE;
}



static gboolean refresh_images_on_ui(gpointer user_data) {
    // Current Image - show/hide based on number_visible flag
    GdkPixbuf *pb1 = render_token_pixbuf_cairo(current_image, 
        current_token, "Current Draw", 
        "#FFDAB9", "#FF0000", "#333333", 
        0.65, 0.15, -0.08, 0.03, -0.05, 0.41, 
        "Liberation Sans Bold", "Liberation Sans", 
        number_visible);
    
    // History images - always visible
    GdkPixbuf *pb2 = render_token_pixbuf_cairo(previous_image, 
        previous_token, "Previous Draw", 
        "#FFDAB9", "#0000FF", "#555555", 
        0.50, 0.07, -0.04, 0.03, -0.06, 0.30, 
        "Liberation Sans Bold", "Liberation Sans", 
        TRUE);
        
    GdkPixbuf *pb3 = render_token_pixbuf_cairo(preceding_image, 
        preceding_token, "Preceding Draw", 
        "#FFDAB9", "#3E2723", "#4F4F4F", 
        0.65, 0.11, -0.08, -0.03, -0.05, 0.3, 
        "Liberation Sans Bold", "Liberation Sans", 
        TRUE);

    if (pb1) { gtk_image_set_from_pixbuf(GTK_IMAGE(current_image), pb1); g_object_unref(pb1); }
    if (pb2) { gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image), pb2); g_object_unref(pb2); }
    if (pb3) { gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image), pb3); g_object_unref(pb3); }
    return FALSE;
}


// ===========================================================
//                     PANED RESIZE LOGIC
// ===========================================================
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

    gtk_paned_set_position(GTK_PANED(top_pane), top_alloc.height * 0.11);
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.85);
    gtk_paned_set_position(GTK_PANED(outer), outer_alloc.width * 0.71);
    gtk_paned_set_position(GTK_PANED(inner), inner_alloc.height * 0.65);

    gtk_widget_show(top_label);
    gtk_widget_show(ticker_fixed);
    gtk_widget_show(ticker_label);

    int top_font_size = (int)(outermost_alloc.height * 0.08 * 0.9 * PANGO_SCALE);
    int ticker_font_size = (int)(outermost_alloc.height * 0.042 * 0.9 * PANGO_SCALE);

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
    
    // CRITICAL: Lock ticker_fixed to its parent's width
    GtkAllocation parent_alloc;
    gtk_widget_get_allocation(top_pane, &parent_alloc);  // Use top_pane width
    if (parent_alloc.width > 100) {
        gtk_widget_set_size_request(ticker_fixed, parent_alloc.width, 60);
    }
    
    // DON'T set size_request on ticker_label yet - let it measure naturally first
    gtk_widget_set_size_request(ticker_label, -1, 60);
    
    g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

static void isolate_ticker_with_overlay(void) {
    GtkWidget *old_parent = gtk_widget_get_parent(ticker_fixed);
    if (!old_parent) return;
    
    GtkWidget *window_child = gtk_bin_get_child(GTK_BIN(window));
    
    GtkWidget *overlay = gtk_overlay_new();
    
    g_object_ref(window_child);
    gtk_container_remove(GTK_CONTAINER(window), window_child);
    gtk_container_add(GTK_CONTAINER(overlay), window_child);
    g_object_unref(window_child);
    
    // Create invisible placeholder to maintain paned structure
    GtkWidget *placeholder = gtk_event_box_new();
    gtk_widget_set_size_request(placeholder, -1, 60);
    gtk_widget_set_opacity(placeholder, 0.0);  // Invisible
    
    // Replace ticker with placeholder in original parent
    g_object_ref(ticker_fixed);
    gtk_container_remove(GTK_CONTAINER(old_parent), ticker_fixed);
    
    // Add placeholder where ticker was
    if (GTK_IS_PANED(old_parent)) {
        gtk_paned_pack2(GTK_PANED(old_parent), placeholder, FALSE, FALSE);
    } else if (GTK_IS_CONTAINER(old_parent)) {
        gtk_container_add(GTK_CONTAINER(old_parent), placeholder);
    }
    
    gtk_widget_show(placeholder);
    
    // Create eventbox wrapper for ticker overlay (for clipping)
    GtkWidget *ticker_box = gtk_event_box_new();
    gtk_widget_set_valign(ticker_box, GTK_ALIGN_END);  // Bottom of screen
    gtk_widget_set_halign(ticker_box, GTK_ALIGN_FILL);  // Full width
    gtk_widget_set_size_request(ticker_box, -1, 60);
    
    // Add ticker to overlay box
    gtk_container_add(GTK_CONTAINER(ticker_box), ticker_fixed);
    g_object_unref(ticker_fixed);
    
    // Add ticker overlay to main overlay
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ticker_box);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), ticker_box, TRUE);
    
    // Put overlay in window
    gtk_container_add(GTK_CONTAINER(window), overlay);
    
    gtk_widget_show_all(overlay);
}
// ===========================================================
//                        TOKEN LOGIC
// ===========================================================
static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

static gboolean update_ui_from_serial(gpointer user_data) {
    // Refresh token images if they're visible
    if (gtk_widget_get_visible(current_image)) {
        g_idle_add(refresh_images_on_ui, NULL);
    }
    return FALSE;
}

static gboolean hide_ticker_cb(gpointer data)
{
    gtk_widget_set_opacity(ticker_label, 0.0);
    return G_SOURCE_REMOVE;
}

static gboolean show_ticker_cb(gpointer data)
{
    gtk_widget_set_opacity(ticker_label, 1.0);
    return G_SOURCE_REMOVE;
}

static void mpv_load_gif(const char *gif)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/mpv.sock");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "loadfile %s replace\n", gif);
        write(fd, cmd, strlen(cmd));
    }

    close(fd);
}

// ===========================================================
//                SERIAL READER THREAD (MAIN LOGIC)
static void *serial_reader_thread(void *arg)
{
    char buf[256];
    size_t pos = 0;
    char rbuf[64];

    while (1) {

        int n = read(serial_fd, rbuf, sizeof(rbuf));

        if (n > 0) {

            for (int i = 0; i < n; i++) {

                char c = rbuf[i];

                if (c == '\r' || c == '\n') {

                    if (pos == 0)
                        continue;

                    buf[pos] = '\0';
                    pos = 0;

                    char *p = buf;
                    while (*p == ' ') p++;

                    char *save = NULL;
                    char *f0 = strtok_r(p, " ", &save);
                    char *f1 = strtok_r(NULL, " ", &save);
                    char *f2 = strtok_r(NULL, " ", &save);

                    /*
                     * FORMAT:
                     * :01 1 <token>
                     * :00 3 6A  → GAME OVER (tty2)
                     * :00 3 7A  → CONGRATS (tty4)
                     * :00 3 7B  → BACK TO TTY1
                     * :00 3 5A  → HIDE TICKER
                     * :00 3 5B  → SHOW TICKER
                     */

                    /* ==================================================
                     * TOKEN UPDATE WITH BULK DETECTION
                     * ================================================== */
                    if (f0 && strcmp(f0, ":01") == 0 &&
                        f1 && strcmp(f1, "1") == 0 &&
                        f2)
                    {
                        /* Return from ANY overlay VT */
                        if (tty2_active || tty4_active || tty5_active) {
                            system("sudo chvt 1");
                            usleep(150000);
                            system("sudo chvt 1");
                            tty2_active = FALSE;
                            tty4_active = FALSE;
                            tty5_active = FALSE;
                        }

                        shift_tokens(f2);
                        
                        /* BULK TOKEN DETECTION */
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);

                        long elapsed_ms = 0;
                        if (last_token_time.tv_sec != 0) {
                            elapsed_ms = (now.tv_sec - last_token_time.tv_sec) * 1000 +
                                        (now.tv_nsec - last_token_time.tv_nsec) / 1000000;
                        }

                        gboolean is_bulk_arrival = (elapsed_ms > 0 && 
                                                   elapsed_ms < BULK_TOKEN_THRESHOLD_MS);

                        last_token_time = now;
                        
                        if (!first_token_received) {
                            // First token ever - check if more coming
                            first_token_received = TRUE;
                            
                            if (is_bulk_arrival) {
                                // Start of bulk load (startup) - switch to TTY5
                                bulk_loading = TRUE;
                                number_visible = FALSE;  // Hide numbers during bulk
                                show_please_wait_tty5();
                            } else {
                                // Single first token on startup - flash it
                                bulk_loading = FALSE;
                                number_visible = TRUE;
                                first_ever_token = FALSE;  // Mark that we've shown first token
                                
                                // Show token display and widgets
                                if (outermost)
                                    gtk_widget_show(outermost);
                                gtk_widget_show(current_image);
                                gtk_widget_show(previous_image);
                                gtk_widget_show(preceding_image);
                                
                                // Trigger flash
                                if (flash_timer_id > 0) {
                                    g_source_remove(flash_timer_id);
                                    flash_timer_id = 0;
                                }
                                if (flash_delay_id > 0)
                                    g_source_remove(flash_delay_id);
                                
                                flash_delay_id = g_timeout_add(300, trigger_flash_after_delay, NULL);
                            }
                        } else {
                            if (is_bulk_arrival) {
                                // Continue bulk loading
                                bulk_loading = TRUE;
                                number_visible = FALSE;
                                
                                // Switch to TTY5 if not already there
                                if (!tty5_active) {
                                    show_please_wait_tty5();
                                }
                                
                                // Reset finish timer
                                if (bulk_finish_timer_id > 0) {
                                    g_source_remove(bulk_finish_timer_id);
                                }
                                // Set timer to finish bulk loading after 4 seconds of no tokens
                                bulk_finish_timer_id = g_timeout_add(BULK_FINISH_DELAY_MS, finish_bulk_loading, NULL);
                            } else {
                                // Single token (normal operation)
                                if (bulk_loading) {
                                    // Just finished bulk loading - simply show tokens without flash
                                    if (bulk_finish_timer_id > 0) {
                                        g_source_remove(bulk_finish_timer_id);
                                        bulk_finish_timer_id = 0;
                                    }
                                    finish_bulk_loading(NULL);
                                } else {
                                    // Regular single token - flash it
                                    number_visible = TRUE;
                                    
                                    // Show token display and widgets
                                    if (outermost)
                                        gtk_widget_show(outermost);
                                    gtk_widget_show(current_image);
                                    gtk_widget_show(previous_image);
                                    gtk_widget_show(preceding_image);
                                    
                                    if (flash_timer_id > 0) {
                                        g_source_remove(flash_timer_id);
                                        flash_timer_id = 0;
                                    }
                                    if (flash_delay_id > 0)
                                        g_source_remove(flash_delay_id);
                                    
                                    flash_delay_id = g_timeout_add(300, trigger_flash_after_delay, NULL);
                                }
                            }
                        }
                        
                        g_idle_add(update_ui_from_serial, NULL);
                    }

                    /* ==================================================
                     * CONTROL COMMANDS
                     * ================================================== */
                    else if (f0 && strcmp(f0, ":00") == 0 &&
                             f1 && strcmp(f1, "3") == 0 &&
                             f2)
                    {
                        /* ---------- GAME OVER ---------- */
                        if (strcmp(f2, "6A") == 0) {

                            clear_tokens();
                            first_token_received = FALSE;  // Reset state
                            bulk_loading = FALSE;
                            first_ever_token = TRUE;  // Reset for next game
                            tty2_active = TRUE;
                            tty4_active = FALSE;
                            tty5_active = FALSE;

                            g_idle_add(update_ui_from_serial, NULL);

                            system(
                                "printf \"playlist-clear\\n"
                                "loadfile /home/pi/KIOSK/gameover.gif replace\\n\" "
                                "| socat - /tmp/mpv.sock"
                            );

                            system("sudo chvt 2");
                        }

                        /* ---------- CONGRATULATIONS ---------- */
                        else if (strcmp(f2, "7A") == 0) {

                            tty4_active = TRUE;
                            tty2_active = FALSE;
                            tty5_active = FALSE;
                            system(
                                "printf \"playlist-clear\\n"
                                "loadfile /home/pi/KIOSK/congratulations1.gif replace\\n\" "
                                "| socat - /tmp/mpv.sock"
                            );

                            system("sudo chvt 4");
                        }

                        /* ---------- EXIT OVERLAY ---------- */
                        else if (strcmp(f2, "7B") == 0) {

                            if (tty2_active || tty4_active || tty5_active) {

                                system("sudo chvt 1");
                               usleep(150000);
                               system("sudo chvt 1");

                                tty2_active = FALSE;
                                tty4_active = FALSE;
                                tty5_active = FALSE;
                            }
                        }

                        /* ---------- HIDE TICKER ---------- */
                        else if (strcmp(f2, "5A") == 0) {
                            g_idle_add(hide_ticker_cb, NULL);
                        }

                        /* ---------- SHOW TICKER ---------- */
                        else if (strcmp(f2, "5B") == 0) {
                            g_idle_add(show_ticker_cb, NULL);
                        }
                    }
                }
                else if (pos + 1 < sizeof(buf)) {
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

// ===========================================================
//            SERIAL TX THREAD (every 1 second)
// ===========================================================
static void *serial_tx_thread(void *arg) {
    while (1) {
        serial_send("hdmi\r\n");
        usleep(1000000);
    }
    return NULL;
}

// ===========================================================
//                           MAIN
// ===========================================================
int main(int argc, char *argv[]) {

    gtk_init(&argc, &argv);
    system("unclutter -idle 0.1 -root &");
    
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


    // ---------------- GTK Builder Setup ----------------
    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_overlay.glade");

    window        = GTK_WIDGET(gtk_builder_get_object(builder, "main"));
    top_label     = GTK_WIDGET(gtk_builder_get_object(builder, "top_label"));
    current_image = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));

    top_pane  = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));
    outermost = GTK_WIDGET(gtk_builder_get_object(builder, "outermost"));
    outer     = GTK_WIDGET(gtk_builder_get_object(builder, "outer"));
    inner     = GTK_WIDGET(gtk_builder_get_object(builder, "inner"));

    ticker_fixed = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));
    gif_area     = GTK_WIDGET(gtk_builder_get_object(builder, "gif_area"));

    isolate_ticker_with_overlay();

    // ---------------- Configurable Top Label ----------------
    char *cfg_label = read_config_value("/boot/firmware/aurum.txt", "AURUM_TOP_LABEL");
    if (cfg_label) {
        gtk_label_set_text(GTK_LABEL(top_label), cfg_label);
        g_print("Loaded top label from config: %s\n", cfg_label);
        free(cfg_label);
    }

    // ---------------- CSS Load ----------------
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    gtk_widget_show_all(window);
    
    // CRITICAL: Hide widgets AFTER show_all, otherwise they get shown again
    gtk_widget_hide(current_image);
    gtk_widget_hide(previous_image);
    gtk_widget_hide(preceding_image);
    gtk_widget_hide(outermost);  // Hide entire token display area on startup
    gtk_widget_hide(gif_area);   // Hide GIF area

    // ---------------- Fullscreen Window ----------------
    GdkScreen *screen = gdk_screen_get_default();
    gtk_window_resize(GTK_WINDOW(window),
                      gdk_screen_get_width(screen),
                      gdk_screen_get_height(screen));
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    // Apply pane ratios after layout stabilizes
    g_timeout_add(200, set_paned_ratios, NULL);

    // ---------------- Show "Please wait..." on TTY5 at startup ----------------
    show_please_wait_tty5();

    // ---------------- Start Background Threads ----------------
    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    pthread_t tx_thread;
    pthread_create(&tx_thread, NULL, serial_tx_thread, NULL);
    pthread_detach(tx_thread);


    // ---------------- Main GTK Loop ----------------
    gtk_main();
    return 0;
}