#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===================== GIF FILES ===================== */
#define GIF_GAME_OVER "/Users/gsm/Documents/KIOSK/gameover.gif"
#define GIF_CONGRATS  "/Users/gsm/Documents/KIOSK/congratulations.gif"

/* ===================== GLOBALS ===================== */
GtkWidget *window;
GtkStack  *main_stack;
GtkWidget *gif_area;
GtkWidget *top_pane;
GtkWidget *outermost, *outer, *inner;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_label;
GtkWidget *ticker_fixed, *ticker_label;
static gboolean gif_playing = FALSE;
static gboolean ticker_initialized = FALSE;  // ADDED: Prevent multiple inits
int ticker_x = 0;
int ticker_width = 0;
int ticker_area_width = 0;
guint ticker_timer_id = 0;

/* ===================== GIF PLAYER ===================== */
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
} GifPlayer;

static GifPlayer *gif_player = NULL;

// ===================== TOKENS =====================
char current_token[32] = "--";
char previous_token[32] = "--";
char preceding_token[32] = "--";

/* ===================== FORWARD DECLARATIONS ===================== */
static void start_ticker(void);
static void stop_ticker(void);
gboolean animate_ticker(gpointer data);
static gboolean refresh_images_on_ui(gpointer user_data);

/* ===================== TICKER FUNCTIONS ===================== */

static void stop_ticker(void)
{
    if (ticker_timer_id > 0) {
        g_source_remove(ticker_timer_id);
        ticker_timer_id = 0;
        g_print("[DEBUG] Ticker stopped\n");
    }
}

gboolean animate_ticker(gpointer data)
{
    if (!ticker_label || !ticker_fixed) {
        ticker_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    // Move left by 2 pixels
    ticker_x -= 2;
    
    // Reset when completely scrolled off
    if (ticker_x < -ticker_width) {
        ticker_x = ticker_area_width;
    }
    
    // Update position
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    
    return G_SOURCE_CONTINUE;
}

static void start_ticker(void)
{
    // CRITICAL: Don't start if already running
    if (ticker_timer_id > 0) {
        g_print("[DEBUG] Ticker already running (id=%d), skipping start\n", ticker_timer_id);
        return;
    }
    
    if (!ticker_label || !ticker_fixed) {
        g_print("[ERROR] ticker_label or ticker_fixed is NULL\n");
        return;
    }
    
    // Get widths
    GtkAllocation label_alloc, fixed_alloc;
    gtk_widget_get_allocation(ticker_label, &label_alloc);
    gtk_widget_get_allocation(ticker_fixed, &fixed_alloc);
    
    ticker_width = label_alloc.width;
    ticker_area_width = fixed_alloc.width;
    
    g_print("[DEBUG] Starting ticker: label_width=%d, area_width=%d\n", 
            ticker_width, ticker_area_width);
    
    // Start from right edge
    ticker_x = ticker_area_width;
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    
    // Start animation (50ms = 20fps)
    ticker_timer_id = g_timeout_add(50, animate_ticker, NULL);
}

static gboolean finalize_ticker_setup(gpointer user_data)
{
    // CRITICAL: Only initialize once
    if (ticker_initialized) {
        g_print("[DEBUG] Ticker already initialized, skipping\n");
        return G_SOURCE_REMOVE;
    }
    
    if (!ticker_label || !ticker_fixed) {
        g_print("[ERROR] Ticker widgets not found\n");
        return G_SOURCE_REMOVE;
    }
    
    ticker_initialized = TRUE;
    g_print("[DEBUG] Initializing ticker (ONCE)\n");
    
    // Get natural size
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(ticker_label, NULL, &natural_size);
    g_print("[DEBUG] Ticker label natural width: %d\n", natural_size.width);
    
    // Start ticker
    start_ticker();
    
    return G_SOURCE_REMOVE;
}

/* ===================== GIF ENGINE ===================== */

static void gif_player_cleanup(void)
{
    if (!gif_player) return;

    if (gif_player->timeout_id)
        g_source_remove(gif_player->timeout_id);

    if (gif_player->animation)
        g_object_unref(gif_player->animation);

    if (gif_player->iter)
        g_object_unref(gif_player->iter);

    g_free(gif_player);
    gif_player = NULL;
}

static gboolean gif_player_advance(gpointer data)
{
    if (!gif_playing || !gif_player)
        return G_SOURCE_REMOVE;

    gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
    gtk_widget_queue_draw(gif_area);

    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    if (delay < 0) delay = 100;

    gif_player->timeout_id =
        g_timeout_add(delay, gif_player_advance, NULL);

    return G_SOURCE_REMOVE;
}

static gboolean gif_player_draw(GtkWidget *widget,
                                cairo_t *cr,
                                gpointer user_data)
{
    if (!gif_playing || !gif_player || !gif_player->iter)
        return FALSE;

    GdkPixbuf *frame =
        gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);

    if (!frame)
        return FALSE;

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    double scale = (double)H / gdk_pixbuf_get_height(frame);
    int scaled_w = gdk_pixbuf_get_width(frame) * scale;
    int x = (W - scaled_w) / 2;

    cairo_save(cr);
    cairo_translate(cr, x, 0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, frame, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    return FALSE;
}

static gboolean show_fullscreen_gif(gpointer file)
{
    const char *filename = file;
    
    g_print("[DEBUG] Showing GIF: %s\n", filename);
    
    // CRITICAL: Stop ticker before switching
    stop_ticker();
    
    // CRITICAL: Stop and cleanup any existing GIF first
    gif_playing = FALSE;
    gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);
    gif_player->animation =
        gdk_pixbuf_animation_new_from_file(filename, NULL);

    if (!gif_player->animation) {
        g_print("[ERROR] Failed to load GIF: %s\n", filename);
        // Return to main screen on error
        gtk_stack_set_visible_child(main_stack, top_pane);
        start_ticker();
        return FALSE;
    }

    gif_player->iter =
        gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);

    gif_playing = TRUE;

    // Force the stack to switch
    gtk_stack_set_visible_child(main_stack, gif_area);
    
    // Show the widget to force visibility
    gtk_widget_show_all(gif_area);
    
    // Queue a redraw instead of forcing event processing
    gtk_widget_queue_draw(gif_area);
    
    g_print("[DEBUG] GIF screen active\n");

    gif_player->timeout_id =
        g_timeout_add(10, gif_player_advance, NULL);

    return FALSE;
}

static gboolean hide_overlay_gif(gpointer data)
{
    g_print("[DEBUG] hide_overlay_gif called, gif_playing=%d\n", gif_playing);
    
    // Don't do anything if we're not showing a GIF
    if (!gif_playing) {
        g_print("[DEBUG] No GIF playing, ignoring hide request\n");
        return FALSE;
    }
    
    gif_playing = FALSE;
    gif_player_cleanup();
    
    gtk_stack_set_visible_child(main_stack, top_pane);
    
    // Show the widget to force visibility
    gtk_widget_show_all(top_pane);
    
    // Queue a redraw of the main screen
    gtk_widget_queue_draw(top_pane);
    
    g_print("[DEBUG] Returned to main screen\n");
    
    // CRITICAL: Restart ticker when returning
    start_ticker();
    
    return FALSE;
}

/* ===================== TOKEN MANAGEMENT ===================== */

static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

/* ===================== CAIRO RENDERING ===================== */

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
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    if (w < 100 || h < 100) {
        w = 600;
        h = 300;
    }

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);

    set_cairo_color(cr, bg_hex);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    int tw, th;
    char fontdesc[128];

    /* Draw NUMBER */
    set_cairo_color(cr, num_hex);
    snprintf(fontdesc, sizeof(fontdesc),
             "%s %d", num_font, (int)(h * number_size_frac));
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

    /* Draw LABEL */
    set_cairo_color(cr, lab_hex);
    snprintf(fontdesc, sizeof(fontdesc),
             "%s %d", lab_font, (int)(h * label_size_frac));
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

    g_object_unref(layout);

    cairo_surface_flush(surface);
    GdkPixbuf *pix =
        gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return pix;
}

static gboolean refresh_images_on_ui(gpointer user_data) {
    GdkPixbuf *pb1 = render_token_pixbuf_cairo(current_image,
                              current_token, "Current Draw",
                              "#FFDAB9", "#FF0000", "#333333",
                              0.65, 0.15, -0.08, 0.03, -0.05, 0.41,
                              "Liberation Sans Bold", "Liberation Sans");

    GdkPixbuf *pb2 = render_token_pixbuf_cairo(previous_image,
                              previous_token, "Previous Draw",
                              "#FFDAB9", "#0000FF", "#555555",
                              0.50, 0.07, -0.04, 0.03, -0.06, 0.30,
                              "Liberation Sans Bold", "Liberation Sans");

    GdkPixbuf *pb3 = render_token_pixbuf_cairo(preceding_image,
                              preceding_token, "Preceding Draw",
                              "#FFDAB9", "#3E2723", "#4F4F4F",
                              0.65, 0.11, -0.08, -0.03, -0.05, 0.3,
                              "Liberation Sans Bold", "Liberation Sans");

    if (pb1) { gtk_image_set_from_pixbuf(GTK_IMAGE(current_image), pb1); g_object_unref(pb1); }
    if (pb2) { gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image), pb2); g_object_unref(pb2); }
    if (pb3) { gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image), pb3); g_object_unref(pb3); }

    return FALSE;
}

/* ===================== LAYOUT FIX ===================== */

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
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.82);
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
    
    GtkAllocation parent_alloc;
    gtk_widget_get_allocation(top_pane, &parent_alloc);
    if (parent_alloc.width > 100) {
        gtk_widget_set_size_request(ticker_fixed, parent_alloc.width, 60);
    }
    
    gtk_widget_set_size_request(ticker_label, -1, 60);
    
    // Start ticker ONCE
    g_timeout_add(200, finalize_ticker_setup, NULL);
    
    return G_SOURCE_REMOVE;
}

/* ===================== KEY HANDLER ===================== */

static gboolean key_press(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    g_print("[DEBUG] Key pressed: %d (gif_playing=%d)\n", e->keyval, gif_playing);
    
    if (e->keyval == GDK_KEY_1) {
        g_print("[DEBUG] Key 1: Show Game Over GIF\n");
        show_fullscreen_gif(GIF_GAME_OVER);
    }
    else if (e->keyval == GDK_KEY_2) {
        g_print("[DEBUG] Key 2: Show Congrats GIF\n");
        show_fullscreen_gif(GIF_CONGRATS);
    }
    else if (e->keyval == GDK_KEY_3) {
        g_print("[DEBUG] Key 3: Request to return to main UI\n");
        hide_overlay_gif(NULL);
    }
    else if (e->keyval == GDK_KEY_4 || e->keyval == GDK_KEY_KP_4) {
        char rand_str[32];
        int r = rand() % 100;
        snprintf(rand_str, sizeof(rand_str), "%02d", r);
        shift_tokens(rand_str);
        g_idle_add(refresh_images_on_ui, NULL);
        g_print("[DEBUG] Random token generated: %s\n", rand_str);
    }
    else if (e->keyval == GDK_KEY_Escape) {
        g_print("[DEBUG] ESC pressed - forcing return to main UI\n");
        gif_playing = FALSE;
        gif_player_cleanup();
        gtk_stack_set_visible_child(main_stack, top_pane);
        gtk_widget_show_all(top_pane);
        start_ticker();
    }

    return TRUE;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    GtkBuilder *b =
        gtk_builder_new_from_file("interface_paned_stack.glade");

    window     = GTK_WIDGET(gtk_builder_get_object(b, "main"));
    main_stack = GTK_STACK(gtk_builder_get_object(b, "main_stack"));
    gif_area   = GTK_WIDGET(gtk_builder_get_object(b, "gif_drawing_area"));
    top_pane   = GTK_WIDGET(gtk_builder_get_object(b, "top_pane"));
    outermost  = GTK_WIDGET(gtk_builder_get_object(b, "outermost"));
    outer      = GTK_WIDGET(gtk_builder_get_object(b, "outer"));
    inner      = GTK_WIDGET(gtk_builder_get_object(b, "inner"));
    current_image = GTK_WIDGET(gtk_builder_get_object(b, "current_image"));
    top_label = GTK_WIDGET(gtk_builder_get_object(b, "top_label"));
    previous_image = GTK_WIDGET(gtk_builder_get_object(b, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(b, "preceding_image"));
    ticker_fixed = GTK_WIDGET(gtk_builder_get_object(b, "ticker_fixed"));
    ticker_label = GTK_WIDGET(gtk_builder_get_object(b, "ticker_label"));

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    g_signal_connect(window, "key-press-event",
                     G_CALLBACK(key_press), NULL);
    g_signal_connect(window, "destroy",
                     G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(gif_area, "draw",
                     G_CALLBACK(gif_player_draw), NULL);
    
    // Configure gif_area for proper display
    gtk_widget_set_app_paintable(gif_area, TRUE);

    gtk_stack_set_visible_child(main_stack, top_pane);
    
    // Configure stack transitions for instant switching
    gtk_stack_set_transition_type(main_stack, GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_transition_duration(main_stack, 0);

    gtk_widget_show_all(window);
    gtk_window_fullscreen(GTK_WINDOW(window));
    
    g_timeout_add(1000, set_paned_ratios, NULL);

    gtk_main();
    return 0;
}