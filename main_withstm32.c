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
GtkWidget *window; // main window is now global
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

// Overlay GIF drawing area (from Glade)
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

// Keep window on top without bouncing geometry
static void refocus_main_window(GtkWidget *win) {
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
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter); // may be -1
    if (delay < 0) delay = 100; // fallback

    if (elapsed_ms >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_player->drawing_area);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

// --- GIF draw: scale to height, center horizontally, black background ---
static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_player || !gif_player->iter) return FALSE;

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame || !GDK_IS_PIXBUF(frame)) return FALSE;

    int da_w = gtk_widget_get_allocated_width(widget);
    int da_h = gtk_widget_get_allocated_height(widget);
    if (da_w < 1 || da_h < 1) return FALSE;

    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);
    if (fw <= 0 || fh <= 0) return FALSE;

    // Fill background black
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    // Fit to height, keep aspect, center horizontally
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

// --- Free player resources (does NOT destroy any window) ---
static void gif_player_cleanup(void) {
    if (!gif_player) return;

    if (gif_player->timeout_id) {
        g_source_remove(gif_player->timeout_id);
        gif_player->timeout_id = 0;
    }
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
    gif_player->drawing_area = NULL;

    g_free(gif_player);
    gif_player = NULL;
}

// ---------- Show GIF in overlay (reuses name used by your serial thread) ----------
static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;
    if (!gif_area) return FALSE;

    // If already playing, stop timer & unref prev frames
    if (gif_player) {
        if (gif_player->timeout_id) {
            g_source_remove(gif_player->timeout_id);
            gif_player->timeout_id = 0;
        }
        if (gif_player->iter) { g_object_unref(gif_player->iter); gif_player->iter = NULL; }
        if (gif_player->animation) { g_object_unref(gif_player->animation); gif_player->animation = NULL; }
        if (gif_player->timer) { g_timer_destroy(gif_player->timer); gif_player->timer = NULL; }
    } else {
        gif_player = g_new0(GifPlayer, 1);
        gif_player->drawing_area = gif_area;
        // connect draw ONCE (safe to connect on first use)
        g_signal_connect(gif_area, "draw", G_CALLBACK(gif_player_draw), NULL);
    }

    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (error || !gif_player->animation || !GDK_IS_PIXBUF_ANIMATION(gif_player->animation)) {
        g_printerr("GIF Error loading %s: %s\n", filename, error ? error->message : "Invalid animation");
        if (error) g_error_free(error);
        gif_player_cleanup();
        return FALSE;
    }

    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();
    g_timer_start(gif_player->timer);

    // Show overlay and start advancing
    gtk_widget_set_visible(gif_area, TRUE);
    gtk_widget_queue_draw(gif_area);
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);

    // Keep main window on top without geometry bounce
    refocus_main_window(window);
    return FALSE;
}

// ---------- Hide GIF overlay ----------
static gboolean hide_overlay_gif(gpointer user_data) {
    if (gif_area) gtk_widget_set_visible(gif_area, FALSE);
    gif_player_cleanup();
    refocus_main_window(window);
    return FALSE;
}

// ===================== Token Images =====================
static void generate_token_image(GtkWidget *widget, const char *number, const char *label,
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

static gboolean refresh_images_on_ui(gpointer user_data) {
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");
    return FALSE;
}

static void *image_generator_thread(void *arg) {
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

// ===================== Ticker =====================
static gboolean animate_ticker(gpointer data) {
    ticker_x -= 2;
    if (ticker_x + ticker_width < 0)
        ticker_x = ticker_area_width;
    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 0);
    return G_SOURCE_CONTINUE;
}

static gboolean finalize_ticker_setup(gpointer data) {
    ticker_width = gtk_widget_get_allocated_width(ticker_label);
    ticker_area_width = gtk_widget_get_allocated_width(ticker_fixed);

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    ticker_x = ticker_area_width;
    ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== Layout sizing =====================
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

    gtk_paned_set_position(GTK_PANED(top_pane), top_alloc.height * 0.08);
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.85);
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

    pthread_t init_image_thread;
    pthread_create(&init_image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(init_image_thread);
    gtk_widget_hide(ticker_fixed);

    //g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== Tokens =====================
static void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

static gboolean update_ui_from_serial(gpointer user_data) {
    pthread_t image_thread;
    pthread_create(&image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(image_thread);
    return FALSE;
}

// ===================== Serial Thread =====================
// Replace your existing serial_reader_thread with this function:

static void *serial_reader_thread(void *arg) {
    const char *serial_port = "/dev/serial0";
    int fd = open(serial_port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("Unable to open serial port");
        return NULL;
    }

    struct termios options;
    if (tcgetattr(fd, &options) == -1) {
        perror("tcgetattr");
        close(fd);
        return NULL;
    }
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
    // make reads non-blocking-ish (you used O_NDELAY)
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1; // 0.1s
    tcsetattr(fd, TCSANOW, &options);
    tcflush(fd, TCIFLUSH);

    #define LINEBUF_SIZE 1024
    char linebuf[LINEBUF_SIZE];
    size_t linepos = 0;
    char rbuf[128];

    while (1) {
        int n = read(fd, rbuf, sizeof(rbuf));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                char c = rbuf[i];

                // treat CR or LF as end-of-line
                if (c == '\r' || c == '\n') {
                    if (linepos == 0) {
                        // skip empty line
                        continue;
                    }

                    // terminate current line
                    linebuf[linepos] = '\0';

                    // trim leading spaces
                    char *p = linebuf;
                    while (*p == ' ') p++;

                    // Tokenize safely
                    char *saveptr = NULL;
                    char *field0 = strtok_r(p, " ", &saveptr); // e.g. ":00" or "$N" or arbitrary token
                    char *field1 = strtok_r(NULL, " ", &saveptr); // e.g. "1" or "T1"
                    char *field2 = strtok_r(NULL, " ", &saveptr); // param or number

                    // 1) : commands e.g. ":00 1 12" or ":00 3 6A"
                    if (field0 && field0[0] == ':') {
                        if (field1 && field2) {
                            if (strcmp(field1, "1") == 0) {
                                 // :00 1 12 -> treat as a NEW draw: shift tokens (preserve history)
                                    shift_tokens(field2);                    // previous <- current, preceding <- previous, current <- field2
                                    g_idle_add(update_ui_from_serial, NULL);
                            } else if (strcmp(field1, "3") == 0) {
                                // initialise displays (start rolling) -> show a rolling GIF overlay
                                // ensure rolling.gif exists in working dir (or change name/path)
                                g_idle_add(show_fullscreen_gif, (gpointer)"rolling.gif");
                            }
                        }
                    }
                    // 2) $N current previous preceding
                    else if (field0 && strcmp(field0, "$N") == 0) {
                        // parse three fields (if present) and set tokens
                        char tmp_cur[32] = "--", tmp_prev[32] = "--", tmp_pre[32] = "--";
                        if (field1) strncpy(tmp_cur, field1, sizeof(tmp_cur)-1);
                        char *f3 = strtok_r(NULL, " ", &saveptr);
                        if (f3) strncpy(tmp_prev, f3, sizeof(tmp_prev)-1);
                        char *f4 = strtok_r(NULL, " ", &saveptr);
                        if (f4) strncpy(tmp_pre, f4, sizeof(tmp_pre)-1);

                        strncpy(current_token, tmp_cur, sizeof(current_token)-1);
                        current_token[sizeof(current_token)-1] = '\0';
                        strncpy(previous_token, tmp_prev, sizeof(previous_token)-1);
                        previous_token[sizeof(previous_token)-1] = '\0';
                        strncpy(preceding_token, tmp_pre, sizeof(preceding_token)-1);
                        preceding_token[sizeof(preceding_token)-1] = '\0';

                        g_idle_add(update_ui_from_serial, NULL);
                    }
                    // 3) $M messages: T1, P1, G1, GS, C1
                    else if (field0 && strcmp(field0, "$M") == 0) {
                        if (field1) {
                            if (strcmp(field1, "T1") == 0) {
                                // Tambola main screen
                                // If you have a callback, call it; otherwise you can set top label or similar.
                                // For now hide overlay and refresh UI main screen
                                g_idle_add(hide_overlay_gif, NULL);
                                // (If you have a dedicated function, call it here)
                            } else if (strcmp(field1, "P1") == 0) {
                                // Please wait
                                g_idle_add(hide_overlay_gif, NULL);
                                // optionally set a "please wait" UI state (not implemented)
                            } else if (strcmp(field1, "G1") == 0) {
                                // Game over: show gameover gif, reset tokens
                                g_idle_add(show_fullscreen_gif, (gpointer)"gameover1.gif");
                                strncpy(current_token, "--", sizeof(current_token));
                                strncpy(previous_token, "--", sizeof(previous_token));
                                strncpy(preceding_token, "--", sizeof(preceding_token));
                                g_idle_add(update_ui_from_serial, NULL);
                            } else if (strcmp(field1, "GS") == 0) {
                                // Game starting
                                g_idle_add(hide_overlay_gif, NULL);
                            } else if (strcmp(field1, "C1") == 0) {
                                // Congratulations
                                g_idle_add(show_fullscreen_gif, (gpointer)"congratulations1.gif");
                            }
                        }
                    }
                    // 4) fallback — treat first token as plain token (old behavior)
                    else if (field0) {
                        g_idle_add(hide_overlay_gif, NULL);
                        shift_tokens(field0);
                        g_idle_add(update_ui_from_serial, NULL);
                    }

                    // reset line buffer
                    linepos = 0;
                    linebuf[0] = '\0';
                } else {
                    // normal character: append if space remains
                    if (linepos + 1 < LINEBUF_SIZE) {
                        linebuf[linepos++] = c;
                    } else {
                        // overflow: discard buffer
                        linepos = 0;
                        linebuf[0] = '\0';
                    }
                }
            } // end for
        } else if (n == 0) {
            // no data available right now
            usleep(20000);
        } else {
            // read error
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(20000);
                continue;
            } else {
                perror("serial read");
                break;
            }
        }
    } // end while

    close(fd);
    return NULL;
}

// ===================== Cleanup =====================
static void cleanup_images(void) {
    remove("current.png");
    remove("previous.png");
    remove("preceding.png");
    gif_player_cleanup();
}

// ===================== main =====================
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // NOTE: this expects your updated Glade with GtkOverlay + drawing area id="gif_area"
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

    // NEW: overlay drawing area for GIF
    gif_area       = GTK_WIDGET(gtk_builder_get_object(builder, "gif_area"));

    if (!window || !top_label || !current_image || !previous_image || !preceding_image ||
        !top_pane || !outermost || !outer || !inner || !ticker_fixed || !ticker_label || !gif_area) {
        g_printerr("Error: Missing widgets from Glade (check gif_area exists).\n");
        return 1;
    }

    // Ensure overlay DA covers whole window & can be drawn on
    gtk_widget_set_app_paintable(gif_area, TRUE);
    gtk_widget_set_hexpand(gif_area, TRUE);
    gtk_widget_set_vexpand(gif_area, TRUE);

    // Fullscreen & styling
        // Show and raise main window
       // Show and raise main window
    gtk_widget_show_all(window);

    /* --- Force the window to match the screen root size (kiosk-fallback) --- */
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) {
        int screen_w = gdk_screen_get_width(screen);
        int screen_h = gdk_screen_get_height(screen);
        g_print("DEBUG: screen size %d x %d\n", screen_w, screen_h);

        /* Force a resize to the full screen geometry and then fullscreen.
           Doing this after show_all avoids layout race conditions in which
           containers constrain allocation. */
        gtk_window_resize(GTK_WINDOW(window), screen_w, screen_h);
        /* small yield to allow allocation to happen before fullscreen */
        while (gtk_events_pending()) gtk_main_iteration();
        gtk_window_fullscreen(GTK_WINDOW(window));
    }

    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Layout sizing + cleanup
    g_idle_add(set_paned_ratios, NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_images), NULL);

    // Show and raise main window
    gtk_widget_show_all(window);
    // Ensure overlay starts hidden (in case Glade file toggles it)
    gtk_widget_set_visible(gif_area, FALSE);
    refocus_main_window(window);

    // Init tokens & ticker
    strcpy(current_token, "--");
    strcpy(previous_token, "--");
    strcpy(preceding_token, "--");

    g_timeout_add(100,  (GSourceFunc)update_ui_from_serial, NULL);
    g_timeout_add(1000, (GSourceFunc)animate_ticker, NULL);

    // Serial thread
    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    gtk_main();
    return 0;
}
