// aurum_display.c
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
#include <errno.h>

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

static gboolean app_running = TRUE;
static GMutex token_mutex;

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
// NOTE: filename_ptr must be a strdup'd char* and will be freed here.
static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    char *filename = (char *)filename_ptr;
    if (!gif_area) {
        g_free(filename);
        return FALSE;
    }

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
        g_free(filename);
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

    // free passed filename (we strdup'd before scheduling)
    g_free(filename);
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
    // Copy tokens under mutex to avoid races with serial thread
    char tok_current[32], tok_previous[32], tok_preceding[32];
    g_mutex_lock(&token_mutex);
    strncpy(tok_current, current_token, sizeof(tok_current)-1);
    tok_current[sizeof(tok_current)-1] = '\0';
    strncpy(tok_previous, previous_token, sizeof(tok_previous)-1);
    tok_previous[sizeof(tok_previous)-1] = '\0';
    strncpy(tok_preceding, preceding_token, sizeof(tok_preceding)-1);
    tok_preceding[sizeof(tok_preceding)-1] = '\0';
    g_mutex_unlock(&token_mutex);

    generate_token_image(current_image, tok_current, "Current Draw", "current.png", "peachpuff", "red",
                         0.78, 0.18, 0.1, -0.07, 0.05, 0.41,
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf");

    generate_token_image(previous_image, tok_previous, "Previous Draw", "previous.png", "peachpuff", "blue",
                         0.70, 0.10, -0.04, -0.03, -0.06, 0.30,
                         "Arial-Bold", "Arial");

    generate_token_image(preceding_image, tok_preceding, "Preceding Draw", "preceding.png", "peachpuff", "brown",
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
    if (ticker_timer_id) g_source_remove(ticker_timer_id);
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

    gtk_paned_set_position(GTK_PANED(top_pane), (int)(top_alloc.height * 0.08));
    gtk_paned_set_position(GTK_PANED(outermost), (int)(outermost_alloc.height * 0.85));
    gtk_paned_set_position(GTK_PANED(outer), (int)(outer_alloc.width * 0.72));
    gtk_paned_set_position(GTK_PANED(inner), (int)(inner_alloc.height * 0.70));

    // compute point sizes (Pango expects points * PANGO_SCALE)
    int top_font_points = (int)(outermost_alloc.height * 0.08 * 0.8);
    int ticker_font_points = (int)(outermost_alloc.height * 0.045 * 0.9);
    int top_pango_size = top_font_points * PANGO_SCALE;
    int ticker_pango_size = ticker_font_points * PANGO_SCALE;

    gtk_widget_set_margin_top(top_pane, (int)(outermost_alloc.height * 0.02));

    char markup_top[256];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Fira Sans' weight='bold' size='%d' foreground='#8B0000'>Aurum Mega Event</span>",
        top_pango_size);
    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);

    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        ticker_pango_size);
    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);

    pthread_t init_image_thread;
    pthread_create(&init_image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(init_image_thread);

    g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

// ===================== Tokens =====================
static void shift_tokens(const char *new_token) {
    g_mutex_lock(&token_mutex);
    strncpy(preceding_token, previous_token, sizeof(preceding_token)-1);
    preceding_token[sizeof(preceding_token)-1] = '\0';
    strncpy(previous_token, current_token, sizeof(previous_token)-1);
    previous_token[sizeof(previous_token)-1] = '\0';
    strncpy(current_token, new_token, sizeof(current_token)-1);
    current_token[sizeof(current_token)-1] = '\0';
    g_mutex_unlock(&token_mutex);
}

static gboolean update_ui_from_serial(gpointer user_data) {
    pthread_t image_thread;
    pthread_create(&image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(image_thread);
    return FALSE;
}

// ===================== Small GTK callbacks & helpers (free args) =====================

// set_led_number: receives a strdup'd string with the number to display
static gboolean set_led_number(gpointer number_ptr) {
    char *num = (char *)number_ptr;
    if (num) {
        g_mutex_lock(&token_mutex);
        // set current token to number (trim)
        strncpy(current_token, num, sizeof(current_token)-1);
        current_token[sizeof(current_token)-1] = '\0';
        g_mutex_unlock(&token_mutex);

        // regenerate images on UI thread
        g_idle_add(update_ui_from_serial, NULL);
        g_free(num);
    }
    return FALSE;
}

// init_led_rolling: receives strdup'd param (e.g. "6A") — here we just show a rolling GIF
static gboolean init_led_rolling(gpointer param_ptr) {
    char *param = (char *)param_ptr;
    if (param) {
        // you can parse param to control animation; for now we show rolling.gif
        char *gif = g_strdup("rolling.gif");
        g_idle_add(show_fullscreen_gif, gif); // gif will be freed inside show_fullscreen_gif
        g_free(param);
    }
    return FALSE;
}

// small wrappers for $M types - they take NULL
static gboolean show_tambola_main(gpointer unused) {
    // TODO: update main screen UI elements — currently prints
    g_print("Displaying Tambola main screen\n");
    return FALSE;
}
static gboolean show_please_wait(gpointer unused) {
    g_print("Please wait screen\n");
    return FALSE;
}
static gboolean show_game_over(gpointer unused) {
    // show game over gif
    char *g = g_strdup("gameover1.gif");
    g_idle_add(show_fullscreen_gif, g);
    return FALSE;
}
static gboolean show_game_starting(gpointer unused) {
    g_print("Game starting\n");
    return FALSE;
}

// ===================== Serial Thread =====================
static void *serial_reader_thread(void *arg) {
    const char *default_port = "/dev/serial0";
    const char *serial_port = default_port;
    int fd = open(serial_port, O_RDWR | O_NOCTTY | O_NONBLOCK);
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
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1; // tenths of seconds
    tcsetattr(fd, TCSANOW, &options);
    tcflush(fd, TCIFLUSH);

    #define LINEBUF_SIZE 1024
    char linebuf[LINEBUF_SIZE];
    size_t linepos = 0;
    char rbuf[128];

    while (app_running) {
        int n = read(fd, rbuf, sizeof(rbuf));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                char c = rbuf[i];
                if (c == '\r' || c == '\n') {
                    if (linepos == 0) continue; // skip empty lines
                    linebuf[linepos] = '\0';
                    // Process line
                    // Trim leading spaces
                    char *p = linebuf;
                    while (*p == ' ') p++;

                    // tokenize
                    char *saveptr = NULL;
                    char *field0 = strtok_r(p, " ", &saveptr);
                    char *field1 = strtok_r(NULL, " ", &saveptr);
                    char *field2 = strtok_r(NULL, " ", &saveptr);

                    if (field0 && field0[0] == ':') {
                        // format :xx cmd param
                        if (field1 && field2) {
                            if (strcmp(field1, "1") == 0) {
                                // :00 1 12 -> display number 12
                                char *num = g_strdup(field2);
                                g_idle_add(set_led_number, num);
                            } else if (strcmp(field1, "3") == 0) {
                                // :00 3 6A -> initialise rolling displays
                                char *pstr = g_strdup(field2);
                                g_idle_add(init_led_rolling, pstr);
                            }
                        }
                    } else if (field0 && strcmp(field0, "$N") == 0) {
                        // $N cur prev pre
                        char tmp_cur[32] = "--", tmp_prev[32] = "--", tmp_pre[32] = "--";
                        if (field1) strncpy(tmp_cur, field1, sizeof(tmp_cur)-1);
                        char *f3 = strtok_r(NULL, " ", &saveptr);
                        if (f3) strncpy(tmp_prev, f3, sizeof(tmp_prev)-1);
                        char *f4 = strtok_r(NULL, " ", &saveptr);
                        if (f4) strncpy(tmp_pre, f4, sizeof(tmp_pre)-1);

                        g_mutex_lock(&token_mutex);
                        strncpy(current_token, tmp_cur, sizeof(current_token)-1);
                        current_token[sizeof(current_token)-1] = '\0';
                        strncpy(previous_token, tmp_prev, sizeof(previous_token)-1);
                        previous_token[sizeof(previous_token)-1] = '\0';
                        strncpy(preceding_token, tmp_pre, sizeof(preceding_token)-1);
                        preceding_token[sizeof(preceding_token)-1] = '\0';
                        g_mutex_unlock(&token_mutex);

                        g_idle_add(update_ui_from_serial, NULL);
                    } else if (field0 && strcmp(field0, "$M") == 0) {
                        if (field1) {
                            if (strcmp(field1, "T1") == 0) g_idle_add(show_tambola_main, NULL);
                            else if (strcmp(field1, "P1") == 0) g_idle_add(show_please_wait, NULL);
                            else if (strcmp(field1, "G1") == 0) {
                                g_idle_add(show_game_over, NULL);
                                g_mutex_lock(&token_mutex);
                                strcpy(current_token, "--"); strcpy(previous_token, "--"); strcpy(preceding_token, "--");
                                g_mutex_unlock(&token_mutex);
                                g_idle_add(update_ui_from_serial, NULL);
                            } else if (strcmp(field1, "GS") == 0) g_idle_add(show_game_starting, NULL);
                            else if (strcmp(field1, "C1") == 0) {
                                char *g = g_strdup("congratulations1.gif");
                                g_idle_add(show_fullscreen_gif, g);
                            }
                        }
                    } else if (field0) {
                        // fallback: treat first token as token to shift
                        g_idle_add(hide_overlay_gif, NULL);
                        shift_tokens(field0);
                        g_idle_add(update_ui_from_serial, NULL);
                    }

                    linepos = 0; // reset
                } else {
                    if (linepos + 1 < LINEBUF_SIZE) linebuf[linepos++] = c;
                    else linepos = 0; // overflow: discard
                }
            }
        } else if (n == 0) {
            // no data right now
            usleep(20000);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(20000);
                continue;
            } else {
                perror("serial read");
                break;
            }
        }
    }

    close(fd);
    return NULL;
}

// ===================== Cleanup =====================
static void cleanup_images(void) {
    // stop gif and free
    gif_player_cleanup();
    // remove generated images (if they exist)
    remove("current.png");
    remove("previous.png");
    remove("preceding.png");
}

// called on window destroy to signal threads to stop
static void on_app_destroy(GtkWidget *widget, gpointer data) {
    app_running = FALSE;
    // give threads a moment to exit; gtk_main_quit is connected separately
}

// ===================== main =====================
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // NOTE: this expects your updated Glade with GtkOverlay + drawing area id="gif_area"
    GtkBuilder *builder = gtk_builder_new_from_file("interface_paned_overlay.glade");
    if (!builder) {
        g_printerr("Failed to load Glade file\n");
        return 1;
    }

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
        g_object_unref(builder);
        return 1;
    }

    // Ensure overlay DA covers whole window & can be drawn on
    gtk_widget_set_app_paintable(gif_area, TRUE);
    gtk_widget_set_hexpand(gif_area, TRUE);
    gtk_widget_set_vexpand(gif_area, TRUE);

    // Fullscreen & styling
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Layout sizing + cleanup
    g_idle_add(set_paned_ratios, NULL);

    // connect destroy: first set running flag, then quit and cleanup on destroy
    g_signal_connect(window, "destroy", G_CALLBACK(on_app_destroy), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_images), NULL);

    // Show and raise main window
    gtk_widget_show_all(window);
    // Ensure overlay starts hidden (in case Glade file toggles it)
    gtk_widget_set_visible(gif_area, FALSE);
    refocus_main_window(window);

    // Init tokens & ticker
    g_mutex_init(&token_mutex);
    g_mutex_lock(&token_mutex);
    strcpy(current_token, "--");
    strcpy(previous_token, "--");
    strcpy(preceding_token, "--");
    g_mutex_unlock(&token_mutex);

    g_timeout_add(100,  (GSourceFunc)update_ui_from_serial, NULL);
    g_timeout_add(1000, (GSourceFunc)animate_ticker, NULL);

    // Serial thread
    pthread_t serial_thread;
    if (pthread_create(&serial_thread, NULL, serial_reader_thread, NULL) == 0) {
        pthread_detach(serial_thread);
    } else {
        g_printerr("Failed to create serial thread\n");
    }

    // free builder & css provider (we already got widgets)
    g_object_unref(builder);
    g_object_unref(css_provider);

    gtk_main();

    // signal exit and allow threads to wind down
    app_running = FALSE;
    // small cleanup
    cleanup_images();
    g_mutex_clear(&token_mutex);

    return 0;
}
