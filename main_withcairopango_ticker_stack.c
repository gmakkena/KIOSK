


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

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CMD_TOKEN        ":01"
#define CMD_CONTROL      ":00"

#define CTRL_HIDE_TICKER "5A"
#define CTRL_SHOW_TICKER "5B"
#define CTRL_GAME_OVER   "6A"
#define CTRL_CONGRATS    "7A"
#define CTRL_EXIT        "7B"
#define GIF_GAME_OVER "/Users/gsm/Documents/KIOSK/gameover.gif"
#define GIF_CONGRATS  "/Users/gsm/Documents/KIOSK/congratulations.gif"


// ===================== GLOBAL SERIAL =====================
int serial_fd = -1;
pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;


// ===================== GIF CONTROL FLAGS =====================
static gboolean gif_playing = FALSE;
static gulong gif_draw_handler_id = 0;
static gboolean switch_to_gif_view(gpointer unused);

GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;
GtkWidget *gif_area = NULL;

int ticker_x = 0;
int ticker_width = 0;
int ticker_area_width = 0;
guint ticker_timer_id = 0;

GtkStack *main_stack = NULL;
static gboolean refresh_images_on_ui(gpointer user_data);
static void gif_player_cleanup(void);
static gboolean set_paned_ratios(gpointer user_data);



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
                (val[0] == '\'' && *(end - 1) == '\'')) {
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

static gboolean hide_overlay_gif(gpointer user_data)
{
    g_print("[GIF] Back to main UI\n");

    gif_playing = FALSE;
    gif_player_cleanup();

    if (gif_draw_handler_id) {
        g_signal_handler_disconnect(gif_area, gif_draw_handler_id);
        gif_draw_handler_id = 0;
    }

    gtk_stack_set_visible_child_name(main_stack, "main_ui");

    /* refresh token UI only */
    g_idle_add(refresh_images_on_ui, NULL);

    return FALSE;
}


static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    static int cnt = 0;
    g_print("[DRAW] gif_player_draw called %d\n", ++cnt);

    if (!gif_playing || !gif_player || !gif_player->animation) {
        g_print("[DRAW] skipped (playing=%d player=%p anim=%p)\n",
                gif_playing, gif_player,
                gif_player ? gif_player->animation : NULL);
        return FALSE;
    }

    GdkPixbuf *frame =
        gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);

    if (!frame) {
        g_print("[DRAW] frame is NULL\n");
        return FALSE;
    }

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

    g_print("[DRAW] widget=%dx%d frame=%dx%d\n", W, H, fw, fh);

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

static gboolean show_fullscreen_gif(gpointer filename_ptr)
{
    const char *filename = filename_ptr;

    g_print("[GIF] Loading %s\n", filename);

    gif_playing = TRUE;
    gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);

    gif_player->animation =
        gdk_pixbuf_animation_new_from_file(filename, NULL);
    if (!gif_player->animation) {
        gif_playing = FALSE;
        return FALSE;
    }

    gif_player->iter =
        gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);

    gif_player->timer = g_timer_new();

    if (gif_draw_handler_id == 0) {
        gif_draw_handler_id =
            g_signal_connect(gif_area, "draw",
                             G_CALLBACK(gif_player_draw), NULL);
    }

    gif_player->timeout_id =
        g_timeout_add(16, gif_player_advance, NULL);

    /* 🔥 SWITCH STACK SAFELY */
    g_idle_add(switch_to_gif_view, NULL);

    return FALSE;
}


static void set_cairo_color(cairo_t *cr, const char *hex)
{
    unsigned r, g, b;
    if (hex && sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3) {
        cairo_set_source_rgb(cr, r / 255.0, g / 255.0, b / 255.0);
    }
}


static GdkPixbuf *
render_token_pixbuf_cairo(GtkWidget *widget,
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
                          const char *lab_font)
{
    /* --- Determine render size --- */
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    if (w < 100 || h < 100) {   // safe fallback before first layout
        w = 600;
        h = 300;
    }

    /* --- Create Cairo surface --- */
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);

    /* --- Paint background --- */
    set_cairo_color(cr, bg_hex);
    cairo_paint(cr);

    /* --- Create Pango layout --- */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    int tw, th;
    char fontdesc[128];

    /* =====================================================
     * Draw NUMBER
     * ===================================================== */
    set_cairo_color(cr, num_hex);

    snprintf(fontdesc, sizeof(fontdesc),
             "%s %d",
             num_font,
             (int)(h * number_size_frac));

    PangoFontDescription *fd_num =
        pango_font_description_from_string(fontdesc);

    pango_layout_set_font_description(layout, fd_num);
    pango_layout_set_text(layout, number ? number : "--", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    int cx = w / 2 + (int)(w * number_x_frac);
    int cy = h / 2 + (int)(h * number_y_frac);

    cairo_move_to(cr, cx - tw / 2, cy - th / 2);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fd_num);

    /* =====================================================
     * Draw LABEL
     * ===================================================== */
    set_cairo_color(cr, lab_hex);

    snprintf(fontdesc, sizeof(fontdesc),
             "%s %d",
             lab_font,
             (int)(h * label_size_frac));

    PangoFontDescription *fd_lab =
        pango_font_description_from_string(fontdesc);

    pango_layout_set_font_description(layout, fd_lab);
    pango_layout_set_text(layout, label ? label : "", -1);
    pango_layout_get_pixel_size(layout, &tw, &th);

    cx = w / 2 + (int)(w * label_x_frac);
    cy = h / 2 + (int)(h * label_y_frac);

    cairo_move_to(cr, cx - tw / 2, cy - th / 2);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fd_lab);

    /* --- Cleanup layout --- */
    g_object_unref(layout);

    /* --- Convert to GdkPixbuf --- */
    cairo_surface_flush(surface);
    GdkPixbuf *pix =
        gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);

    /* --- Cleanup cairo --- */
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


gboolean finalize_ticker_setup(gpointer data)
{
    GtkAllocation alloc;

    /* Use the ACTUAL ticker container width */
    gtk_widget_get_allocation(ticker_fixed, &alloc);
    ticker_area_width = alloc.width;

    /* Measure label */
    GtkRequisition label_req;
    gtk_widget_get_preferred_size(ticker_label, NULL, &label_req);
    ticker_width = label_req.width;

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    /* Start off-screen right (relative to container) */
    ticker_x = ticker_area_width;

    /* Lock label size ONCE */
    gtk_widget_set_size_request(ticker_label, ticker_width, 60);

    if (ticker_timer_id == 0)
        ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);

    return G_SOURCE_REMOVE;
}




static gboolean refresh_images_on_ui(gpointer user_data) {
    GdkPixbuf *pb1 = render_token_pixbuf_cairo(current_image,
                              current_token,
                              "Current Draw",
                              "#FFDAB9",   // background
                              "#FF0000",   // number (red)
                              "#333333",   // label (dark gray)
                              0.65, 0.15,
                              -0.08, 0.03,
                              -0.05,  0.41,
                              "Liberation Sans Bold",
                              "Liberation Sans");

    GdkPixbuf *pb2 = render_token_pixbuf_cairo(previous_image,
                              previous_token,
                              "Previous Draw",
                              "#FFDAB9",   // background
                              "#0000FF",   // number (blue)
                              "#555555",   // label (muted gray)
                              0.50, 0.07,  // smaller fonts
                             -0.04, 0.03,
                             -0.06,  0.30,
                              "Liberation Sans Bold",
                              "Liberation Sans");

    GdkPixbuf *pb3 = render_token_pixbuf_cairo(preceding_image,
                              preceding_token,
                              "Preceding Draw",
                              "#FFDAB9",   // background
                              "#3E2723 ",   // number (brown)
                              "#4F4F4F",   // label (dark gray)
                              0.65, 0.11,  // large number
                             -0.08, -0.03,
                             -0.05,  0.3,
                              "Liberation Sans Bold",
                              "Liberation Sans");

    if (pb1) { gtk_image_set_from_pixbuf(GTK_IMAGE(current_image), pb1); g_object_unref(pb1); }
    if (pb2) { gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image), pb2); g_object_unref(pb2); }
    if (pb3) { gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image), pb3); g_object_unref(pb3); }

    return FALSE;
}



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


static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

static gboolean update_ui_from_serial(gpointer user_data) {
    g_idle_add(refresh_images_on_ui, NULL);
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

static void *serial_reader_thread(void *arg)
{
    char buf[256];
    size_t pos = 0;
    char rbuf[64];

    while (1) {

        int n = read(serial_fd, rbuf, sizeof(rbuf));
        if (n <= 0) {
            usleep(20000);
            continue;
        }

        for (int i = 0; i < n; i++) {

            char c = rbuf[i];

            if (c != '\r' && c != '\n') {
                if (pos + 1 < sizeof(buf))
                    buf[pos++] = c;
                continue;
            }

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

            if (!f0 || !f1 || !f2)
                continue;

            /* ================= TOKEN UPDATE ================= */
            if (strcmp(f0, CMD_TOKEN) == 0 &&
                strcmp(f1, "1") == 0)
            {
                shift_tokens(f2);
                g_idle_add(update_ui_from_serial, NULL);
                continue;
            }

            /* ================= CONTROL COMMANDS ================= */
            if (strcmp(f0, CMD_CONTROL) != 0 ||
                strcmp(f1, "3") != 0)
                continue;

            /* ---------- HIDE TICKER ---------- */
            if (strcmp(f2, CTRL_HIDE_TICKER) == 0) {
                g_idle_add(hide_ticker_cb, NULL);
            }

            /* ---------- SHOW TICKER ---------- */
            else if (strcmp(f2, CTRL_SHOW_TICKER) == 0) {
                g_idle_add(show_ticker_cb, NULL);
            }

            /* ---------- GAME OVER GIF ---------- */
            else if (strcmp(f2, CTRL_GAME_OVER) == 0) {
                g_idle_add(show_fullscreen_gif,
                           (gpointer)GIF_GAME_OVER);
            }

            /* ---------- CONGRATS GIF ---------- */
            else if (strcmp(f2, CTRL_CONGRATS) == 0) {
                g_idle_add(show_fullscreen_gif,
                           (gpointer)GIF_CONGRATS);
            }

            /* ---------- BACK TO MAIN UI ---------- */
            else if (strcmp(f2, CTRL_EXIT) == 0) {
                g_idle_add(hide_overlay_gif, NULL);
            }
        }
    }

    return NULL;
}



static void *serial_tx_thread(void *arg) {
    while (1) {
        serial_send("hdmi\r\n");
        usleep(1000000);
    }
    return NULL;
}


/* ===================== KEYBOARD TEST HANDLER =====================
 * For macOS / development testing only
 * ---------------------------------------------------------------
 * 1 → Game Over GIF
 * 2 → Congrats GIF
 * 3 → Back to Main UI
 * 4 → Update token (demo)
 * h → Hide ticker
 * s → Show ticker
 * =============================================================== */

static gboolean key_test_cb(GtkWidget *widget,
                            GdkEventKey *event,
                            gpointer user_data)
{
    switch (event->keyval) {

        case GDK_KEY_1:
            g_print("[KEY] GAME OVER\n");
            g_idle_add(show_fullscreen_gif,
                       (gpointer)GIF_GAME_OVER);
            break;

        case GDK_KEY_2:
            g_print("[KEY] CONGRATS\n");
            g_idle_add(show_fullscreen_gif,
                       (gpointer)GIF_CONGRATS);
            break;

        case GDK_KEY_3:
            g_print("[KEY] BACK TO MAIN UI\n");
            g_idle_add(hide_overlay_gif, NULL);
            break;

        case GDK_KEY_4:
            g_print("[KEY] TOKEN UPDATE\n");
            shift_tokens("99");
            g_idle_add(update_ui_from_serial, NULL);
            break;

        case GDK_KEY_h:
        case GDK_KEY_H:
            g_print("[KEY] HIDE TICKER\n");
            g_idle_add(hide_ticker_cb, NULL);
            break;

        case GDK_KEY_s:
        case GDK_KEY_S:
            g_print("[KEY] SHOW TICKER\n");
            g_idle_add(show_ticker_cb, NULL);
            break;

        default:
            return FALSE;
    }

    return TRUE;
}

static gboolean stack_debug_switch(gpointer data)
{
    static int flip = 0;

    if (flip) {
        g_print(">>> SHOW main_ui\n");
        gtk_stack_set_visible_child_name(main_stack, "main_ui");
    } else {
        g_print(">>> SHOW gif_view\n");
        gtk_stack_set_visible_child_name(main_stack, "gif_view");
    }

    flip ^= 1;
    return G_SOURCE_CONTINUE;
}
static gboolean switch_to_gif_view(gpointer unused)
{
    gtk_stack_set_visible_child_name(main_stack, "gif_view");
    gtk_widget_queue_draw(gif_area);
    return G_SOURCE_REMOVE;
}


int main(int argc, char *argv[]) {

    gtk_init(&argc, &argv);
    //system("unclutter -idle 0.1 -root &");
    // ---------------- Serial Setup ----------------
    const char *serial_port = "/dev/serial0";

    serial_fd = open(serial_port, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        perror("Failed to open serial port");
       // return 1;
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
    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_stack.glade");

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
    main_stack   = GTK_STACK(gtk_builder_get_object(builder, "main_stack"));
    gif_area     = GTK_WIDGET(gtk_builder_get_object(builder, "gif_drawing_area"));
/* Get widgets */
main_stack = GTK_STACK(gtk_builder_get_object(builder, "main_stack"));
gif_area   = GTK_WIDGET(gtk_builder_get_object(builder, "gif_drawing_area"));
top_pane   = GTK_WIDGET(gtk_builder_get_object(builder, "top_pane"));

/* Detach from Glade parents */
gtk_widget_unparent(gif_area);
gtk_widget_unparent(top_pane);

/* Attach stack pages explicitly */
gtk_stack_add_named(main_stack, gif_area, "gif_view");
gtk_stack_add_named(main_stack, top_pane, "main_ui");

/* Make stack layout sane */
gtk_stack_set_homogeneous(main_stack, TRUE);
gtk_stack_set_vhomogeneous(main_stack, TRUE);

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

   

    gtk_widget_set_can_focus(window, TRUE);
gtk_widget_grab_focus(window);

gtk_widget_add_events(window, GDK_KEY_PRESS_MASK);


/* ===================== KEYBOARD TEST (DEV ONLY) ===================== */
g_signal_connect(window,
                 "key-press-event",
                 G_CALLBACK(key_test_cb),
                 NULL);

 gtk_widget_show_all(window);
gtk_stack_set_visible_child_name(main_stack, "main_ui");

/* ===== FULLSCREEN AFTER SHOW ===== */
gtk_window_fullscreen(GTK_WINDOW(window));
gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    // ---------------- Fullscreen Window ----------------
    GdkScreen *screen = gdk_screen_get_default();
    gtk_window_resize(GTK_WINDOW(window),
                      gdk_screen_get_width(screen),
                      gdk_screen_get_height(screen));
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    // Apply pane ratios after layout stabilizes
    g_timeout_add(200, set_paned_ratios, NULL);

   // g_timeout_add_seconds(2, stack_debug_switch, NULL);



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

