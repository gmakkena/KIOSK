// Modified: clears GIF animation before destroying window and ensures destruction happens on GTK main thread.

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
GtkWidget *gif_image = NULL;

// Ticker
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

// Tokens
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

// ---------- Safe destroy (unused for GIF) ----------
gboolean safe_destroy_window(gpointer data) {
    GtkWidget *win = GTK_WIDGET(data);
    if (GTK_IS_WIDGET(win)) {
        gtk_widget_destroy(win);
    }
    return G_SOURCE_REMOVE;
}

// ---------- Destroy gif window on main thread (clears animation first) ----------
gboolean destroy_gif_window(gpointer data) {
    // This runs on the GTK main thread via g_idle_add
    if (gif_window) {
        // If we have a gif_image, clear it to stop internal animation timers
        if (gif_image && GTK_IS_IMAGE(gif_image)) {
            gtk_image_clear(GTK_IMAGE(gif_image)); // removes any animation/pixbuf from the GtkImage
            // Note: gtk_image_clear will stop GTK's internal animation usage of the pixbuf
        } else {
            // if gif_image isn't set, try to find a child image in gif_window (defensive)
            if (GTK_IS_WINDOW(gif_window)) {
                GtkWidget *child = gtk_bin_get_child(GTK_BIN(gif_window));
                if (child && GTK_IS_IMAGE(child)) {
                    gtk_image_clear(GTK_IMAGE(child));
                }
            }
        }

        // Now destroy the window
        gtk_widget_destroy(gif_window);
        gif_window = NULL;
        gif_image = NULL;
    }
    return G_SOURCE_REMOVE;
}

// Keypress handler for GIF window (runs on main thread)
gboolean on_gif_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    // schedule safe destroy on main loop (or just call directly; we're already on main thread)
    destroy_gif_window(NULL);
    // Request a UI refresh of tokens
    g_idle_add((GSourceFunc)/*function*/ (gpointer) (GSourceFunc) gtk_widget_queue_draw, NULL);
    return TRUE;
}

// Helper to schedule a token UI refresh (wrap update_ui_from_serial)
gboolean schedule_update_ui_from_serial(gpointer data);

// ---------- Show Fullscreen GIF ----------
gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;

    // Schedule close if an older gif_window exists (safe via main thread)
    if (gif_window) {
        // schedule destruction of previous GIF first
        g_idle_add(destroy_gif_window, NULL);
        // we let that run; continue to create new one below
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

    // Create and load GIF into gif_image
    gif_image = gtk_image_new();
    GError *error = NULL;
    GdkPixbufAnimation *anim = gdk_pixbuf_animation_new_from_file(filename, &error);

    if (!anim) {
        g_printerr("GIF Error: %s\n", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        // clean up gif_image if created
        if (gif_image) {
            gtk_widget_destroy(gif_image);
            gif_image = NULL;
        }
        if (gif_window) {
            gtk_widget_destroy(gif_window);
            gif_window = NULL;
        }
        return FALSE;
    }

    gtk_image_set_from_animation(GTK_IMAGE(gif_image), anim);
    g_object_unref(anim);

    // Close on any key press (use our safe handler)
    g_signal_connect(gif_window, "key-press-event",
        G_CALLBACK(on_gif_keypress), NULL);

    gtk_container_add(GTK_CONTAINER(gif_window), gif_image);
    gtk_widget_show_all(gif_window);

    // Optional: auto-close after N seconds if you want a fallback
    // g_timeout_add_seconds(5, destroy_gif_window, NULL);

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
                         "DejaVuSans-Bold", "DejaVuSans");

    generate_token_image(preceding_image, preceding_token, "Preceding Draw", "preceding.png", "peachpuff", "brown",
                         0.92, 0.17, -0.08, -0.1, -0.05, 0.35,
                         "DejaVuSans-Bold", "DejaVuSans");

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
        "<span font_family='DejaVu Sans' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
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

// wrapper to schedule update_ui_from_serial easily from destroy callback
gboolean schedule_update_ui_from_serial(gpointer data) {
    g_idle_add(update_ui_from_serial, NULL);
    return G_SOURCE_REMOVE;
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
                // schedule showing GIF on main thread
                g_idle_add(show_fullscreen_gif, "congratulations1.gif");
                // Also schedule an auto restore after 5s in case no tokens come
                g_timeout_add_seconds(5, schedule_update_ui_from_serial, NULL);
                // optionally also schedule gif destruction after 5s
                g_timeout_add_seconds(5, destroy_gif_window, NULL);
            } else if (strcmp(token, "G1") == 0) {
                g_idle_add(show_fullscreen_gif, "gameover1.gif");
                strcpy(current_token, "--");
                strcpy(previous_token, "--");
                strcpy(preceding_token, "--");
                g_timeout_add_seconds(5, schedule_update_ui_from_serial, NULL);
                g_timeout_add_seconds(5, destroy_gif_window, NULL);
            } else {
                // If a normal token arrives, we want to close GIF (if any) and update UI.
                // Schedule the gif window destruction on main thread (safe)
                g_idle_add(destroy_gif_window, NULL);

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
    // ensure gif window destroyed on main thread
    if (gif_window) {
        // If we're already on main thread, destroy directly; else schedule
        if (g_main_context_is_owner(g_main_context_default())) {
            destroy_gif_window(NULL);
        } else {
            g_idle_add(destroy_gif_window, NULL);
        }
    }
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
