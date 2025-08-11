// Modified: uses mpv with IPC for GIF playback, preloads mpv to avoid startup delay.
// Liberation Sans fonts used for token images.

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/stat.h>

#define MPV_PATH "/usr/bin/mpv"

// Debug function
void debug_log(const char *msg) {
    FILE *f = fopen("debug.log", "a");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "[%ld] %s\n", now, msg);
        fclose(f);
    }
}

// Widgets
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

// Ticker
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

// Tokens
char current_token[32]   = "--";
char previous_token[32]  = "--";
char preceding_token[32] = "--";

// MPV IPC
pid_t mpv_pid = -1;
const char *mpv_socket_path = "/tmp/mpv_socket";
int mpv_ready = 0;

// ---------- MPV Control ----------
void ensure_mpv_running() {
    debug_log("Checking MPV status");
    
    // Check if the socket exists and is accessible
    if (access(mpv_socket_path, F_OK) != 0) {
        debug_log("MPV socket not found, starting MPV");
        if (mpv_pid > 0) {
            kill(mpv_pid, SIGTERM);
            usleep(500000);  // Wait for process to terminate
        }
        mpv_start_preloaded();
        usleep(1000000);  // Wait for MPV to initialize
    }
}
int mpv_send_command(const char *cmd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        debug_log("Failed to create socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mpv_socket_path, sizeof(addr.sun_path)-1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        debug_log("Failed to connect to MPV socket");
        close(sock);
        return -1;
    }

    write(sock, cmd, strlen(cmd));
    write(sock, "\n", 1);
    
    // Read response
    char response[1024];
    int n = read(sock, response, sizeof(response)-1);
    if (n > 0) {
        response[n] = '\0';
        debug_log(response);
    }
    
    close(sock);
    return 0;
}

void mpv_start_preloaded() {
    debug_log("Starting MPV preload");
    
    // Check if MPV exists
    if (access("/usr/bin/mpv", X_OK) != 0) {
        debug_log("Error: MPV not found at /usr/bin/mpv");
        return;
    }

    // Create a blank black.png if it doesn't exist
    if (access("black.png", F_OK) != 0) {
        system("convert -size 1920x1080 xc:black black.png");
        debug_log("Created black.png");
    }

    // Remove any stale socket
    unlink(mpv_socket_path);

    mpv_pid = fork();
    if (mpv_pid == 0) {
        // Child process
        debug_log("Child process starting");
        
        // Redirect stdout and stderr to /dev/null
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        
        execl("/usr/bin/mpv", "mpv",
              "--idle=yes",
              "--no-terminal",
              "--really-quiet",
              "--input-ipc-server=/tmp/mpv_socket",
              "--force-window=yes",
              "--fs",
              "--no-border",
              "--keep-open=yes",
              "--ontop",
              "--no-terminal",
              "--no-stop-screensaver",
              "--on-all-workspaces",
              "--no-keepaspect-window",
              "--geometry=0:0",
              "black.png",
              NULL);
              
        debug_log("MPV launch failed");
        perror("execl failed");
        _exit(1);
    }

    // Give mpv time to start and create socket
    for (int i = 0; i < 20; i++) {
        if (access(mpv_socket_path, F_OK) == 0) break;
        usleep(100000);
    }
}

void mpv_set_ontop(gboolean enable) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "{\"command\": [\"set_property\", \"ontop\", %s]}",
             enable ? "true" : "false");
    mpv_send_command(cmd);
}

void mpv_play_gif(const char *filename) {
    debug_log("Starting to play GIF");
    
    // First ensure the window is ready
    mpv_send_command("{\"command\": [\"set_property\", \"ontop\", true]}");
    mpv_send_command("{\"command\": [\"set_property\", \"on-all-workspaces\", true]}");
    mpv_send_command("{\"command\": [\"set_property\", \"keep-open\", true]}");
    mpv_send_command("{\"command\": [\"set_property\", \"fullscreen\", true]}");
    usleep(100000);  // Small delay

    // Force window to front
    mpv_send_command("{\"command\": [\"set_property\", \"focus\", true]}");
    mpv_send_command("{\"command\": [\"set_property\", \"border\", false]}");
    
    // Load and play the file
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "{\"command\": [\"loadfile\", \"%s\", \"replace\"]}",
             filename);
    mpv_send_command(cmd);
    
    // Ensure playback and visibility
    usleep(100000);  // Wait for file to load
    mpv_send_command("{\"command\": [\"set_property\", \"pause\", false]}");
    mpv_send_command("{\"command\": [\"set_property\", \"ontop\", true]}");
    mpv_send_command("{\"command\": [\"set_property\", \"focus\", true]}");
    debug_log("GIF playback started");
}

void mpv_stop_gif() {
    // Stop playback and load black screen
    mpv_send_command("{\"command\": [\"loadfile\", \"black.png\", \"replace\"]}");
    mpv_send_command("{\"command\": [\"set_property\", \"ontop\", false]}");
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
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");

    generate_token_image(preceding_image, preceding_token, "Preceding Draw", "preceding.png", "peachpuff", "brown",
                         0.92, 0.17, -0.08, -0.1, -0.05, 0.35,
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");

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
    mpv_play_gif("congratulations1.gif");
} else if (strcmp(token, "G1") == 0) {
    mpv_play_gif("gameover1.gif");
    strcpy(current_token, "--");
    strcpy(previous_token, "--");
    strcpy(preceding_token, "--");
} else {
    mpv_stop_gif();
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
    mpv_stop_gif();
    if (mpv_pid > 0) {
        kill(mpv_pid, SIGTERM);
        mpv_pid = -1;
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    debug_log("Starting program");
    
    // Create /tmp directory if it doesn't exist
    mkdir("/tmp", 0777);
    
    // Start mpv in idle mode
    mpv_start_preloaded();
    
    // Wait for MPV to initialize and check if it's running
    int attempts = 0;
    while (attempts < 20) {
        if (access(mpv_socket_path, F_OK) == 0) {
            debug_log("MPV initialized successfully");
            break;
        }
        usleep(100000);  // Wait 100ms
        attempts++;
    }
    
    if (attempts >= 20) {
        debug_log("Error: Failed to initialize MPV");
    }

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
