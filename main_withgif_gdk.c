#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

// Widgets
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

// GIF Playback
GtkWidget *gif_window = NULL;

// For fullscreen GIF scaling:
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GtkWidget *drawing_area;
    GTimer *timer;
} GifPlayer;
static GifPlayer *gif_player = NULL;

// Ticker
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

// Tokens
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

gboolean safe_destroy_window(gpointer data) {
    GtkWidget *win = GTK_WIDGET(data);
    if (GTK_IS_WIDGET(win)) {
        gtk_widget_destroy(win);
    }
    return G_SOURCE_REMOVE;
}

// --- GIF Fullscreen Scaling Helper Functions ---
static gboolean gif_player_advance(gpointer data) {
    if (!gif_player || !gif_player->iter) return G_SOURCE_REMOVE;

    gdouble elapsed = g_timer_elapsed(gif_player->timer, NULL) * 1000.0; // ms
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);

    if (elapsed >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_player->drawing_area);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_player || !gif_player->iter || !gif_player->animation) {
        g_print("Draw callback: Invalid gif_player state\n");
        return FALSE;
    }

    // Get the current frame
    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame || !GDK_IS_PIXBUF(frame)) {
        g_print("Draw callback: Invalid frame pixbuf\n");
        return FALSE;
    }

    int da_width = gtk_widget_get_allocated_width(widget);
    int da_height = gtk_widget_get_allocated_height(widget);

    if (da_width < 1 || da_height < 1) {
        g_print("Draw callback: Invalid drawing area dimensions\n");
        return FALSE;
    }

    // Create a new scaled pixbuf
    GError *error = NULL;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(frame, 
                                               da_width, 
                                               da_height, 
                                               GDK_INTERP_BILINEAR);
    
    if (!scaled || !GDK_IS_PIXBUF(scaled)) {
        g_print("Draw callback: Failed to create scaled pixbuf\n");
        return FALSE;
    }

    // Set background to black
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    // Draw the scaled pixbuf
    gdk_cairo_set_source_pixbuf(cr, scaled, 0, 0);
    cairo_paint(cr);

    g_object_unref(scaled);
    return FALSE;
}

static void gif_player_cleanup() {
    if (gif_player) {
        g_print("Cleaning up gif_player resources\n");
        
        if (gif_player->timeout_id) {
            g_source_remove(gif_player->timeout_id);
            gif_player->timeout_id = 0;
        }
        
        if (gif_player->iter) {
            g_object_unref(gif_player->iter);
            gif_player->iter = NULL;
        }
        
        if (gif_player->animation) {
            g_object_unref(gif_player->animation);
            gif_player->animation = NULL;
        }
        
        if (gif_player->timer) {
            g_timer_destroy(gif_player->timer);
            gif_player->timer = NULL;
        }
        
        if (gif_player->drawing_area) {
            gif_player->drawing_area = NULL;  // Will be destroyed with window
        }
        
        g_free(gif_player);
        gif_player = NULL;
    }
    
    if (gif_window) {
        gtk_widget_destroy(gif_window);
        gif_window = NULL;
    }
}

// ---------- Show Fullscreen GIF (now stretched to fit) ----------
gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;
    g_print("Attempting to show GIF: %s\n", filename);

    // Cleanup existing GIF window/player if open
    gif_player_cleanup();

    // Verify file exists first
    if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
        g_printerr("GIF file does not exist: %s\n", filename);
        return FALSE;
    }

    // Create new fullscreen window
    gif_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(gif_window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(gif_window));
    gtk_window_set_keep_above(GTK_WINDOW(gif_window), TRUE);

    // Black background
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, 
        "window { background-color: black; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(gif_window),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // Allocate gif player struct
    gif_player = g_new0(GifPlayer, 1);
    if (!gif_player) {
        g_printerr("Failed to allocate GifPlayer structure\n");
        gtk_widget_destroy(gif_window);
        gif_window = NULL;
        return FALSE;
    }

    // Load animation with detailed error checking
    GError *error = NULL;
    g_print("Loading animation from file: %s\n", filename);
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    
    if (error) {
        g_printerr("GIF Error loading %s: %s\n", filename, error->message);
        g_error_free(error);
        g_free(gif_player);
        gif_player = NULL;
        gtk_widget_destroy(gif_window);
        gif_window = NULL;
        return FALSE;
    }
    
    if (!gif_player->animation || !GDK_IS_PIXBUF_ANIMATION(gif_player->animation)) {
        g_printerr("Invalid animation object created for %s\n", filename);
        if (gif_player->animation) {
            g_object_unref(gif_player->animation);
        }
        g_free(gif_player);
        gif_player = NULL;
        gtk_widget_destroy(gif_window);
        gif_window = NULL;
        return FALSE;
    }
    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();

    // Drawing area to render gif
    gif_player->drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(gif_window), gif_player->drawing_area);

    g_signal_connect(gif_player->drawing_area, "draw", G_CALLBACK(gif_player_draw), NULL);
    g_signal_connect(gif_window, "destroy", G_CALLBACK(gif_player_cleanup), NULL);
    g_signal_connect(gif_window, "key-press-event", G_CALLBACK(gtk_widget_destroy), NULL);

    gtk_widget_show_all(gif_window);

    // Start animation timer
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);

    return FALSE;
}

// ---------- Generate Token Image ----------
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

// ---------- Refresh Images on UI ----------
gboolean refresh_images_on_ui(gpointer user_data) {
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");
    return FALSE;
}

// ---------- Thread: Generate images then refresh UI ----------
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

// ---------- Animate Ticker ----------
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

// ---------- Set Pane Ratios ----------
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

    pthread_t init_image_thread;
    pthread_create(&init_image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(init_image_thread);

    g_timeout_add(100, finalize_ticker_setup, NULL);
    return G_SOURCE_REMOVE;
}

// ---------- Shift Token Queue ----------
void shift_tokens(const char *new_token) {
    strncpy(preceding_token, previous_token, sizeof(preceding_token));
    strncpy(previous_token, current_token, sizeof(previous_token));
    strncpy(current_token, new_token, sizeof(current_token));
}

// ---------- On new token: spawn image generator thread ----------
gboolean update_ui_from_serial(gpointer user_data) {
    pthread_t image_thread;
    pthread_create(&image_thread, NULL, image_generator_thread, NULL);
    pthread_detach(image_thread);
    return FALSE;
}

// ---------- Serial Reading Thread ----------
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
                g_print("Received C1 token, showing congratulations GIF\n");
                char *gif_path = g_build_filename(g_get_current_dir(), "congratulations1.gif", NULL);
                g_print("Full path to congratulations GIF: %s\n", gif_path);
                g_idle_add(show_fullscreen_gif, gif_path);
                // Don't free gif_path as it's used by the idle callback
            } else if (strcmp(token, "G1") == 0) {
                g_print("Received G1 token, showing gameover GIF\n");
                char *gif_path = g_build_filename(g_get_current_dir(), "gameover1.gif", NULL);
                g_print("Full path to gameover GIF: %s\n", gif_path);
                g_idle_add(show_fullscreen_gif, gif_path);
                // Reset tokens safely
                strncpy(current_token, "--", sizeof(current_token));
                strncpy(previous_token, "--", sizeof(previous_token));
                strncpy(preceding_token, "--", sizeof(preceding_token));

                g_idle_add(update_ui_from_serial, NULL);
            } else {
                // Close GIF if normal token arrives
                if (gif_window) {
                    gtk_widget_destroy(gif_window);
                    gif_window = NULL;
                    gif_player_cleanup();
                }
                shift_tokens(token);
                g_idle_add(update_ui_from_serial, NULL);
            }
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
    gif_player_cleanup();
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
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_idle_add(set_paned_ratios, NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_images), NULL);
    gtk_widget_show_all(window);

    strcpy(current_token, "--");
    strcpy(previous_token, "--");
    strcpy(preceding_token, "--");
    g_timeout_add(100, (GSourceFunc)update_ui_from_serial, NULL);
    g_timeout_add(1000, (GSourceFunc)animate_ticker, NULL);
    pthread_t serial_thread;
    pthread_create(&serial_thread, NULL, serial_reader_thread, NULL);
    pthread_detach(serial_thread);

    gtk_main();
    return 0;
}