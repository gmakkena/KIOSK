#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

#define IMAGE_PATH "token_image.png"

// Function to generate the image using ImageMagick (convert)
void generate_token_image(const char *token_text, int width, int height) {
    char command[512];
   snprintf(command, sizeof(command),
    "convert -background lightblue -fill blue -font Arial "
    "-gravity center -stroke black -strokewidth 2 "
    "-size %dx%d caption:'%s' -extent %dx%d %s",
    width, height, token_text, width, height, IMAGE_PATH);

    system(command);
}

int main(int argc, char *argv[]) {
    GtkBuilder *builder;
    GtkWidget *window;
    GtkImage *image;
    GdkDisplay *display;
    GdkMonitor *monitor;
    GdkRectangle geometry;
    int screen_width, screen_height;

    gtk_init(&argc, &argv);

    // Load Glade UI
    builder = gtk_builder_new_from_file("interface_image.glade");
    window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));
     gtk_window_fullscreen(GTK_WINDOW(window)); 
    image = GTK_IMAGE(gtk_builder_get_object(builder, "current_image"));

    // Get screen size using monitor geometry
    display = gdk_display_get_default();
    monitor = gdk_display_get_primary_monitor(display);
    gdk_monitor_get_geometry(monitor, &geometry);
    screen_width = geometry.width;
    screen_height = geometry.height;

    // Example token number
    const char *token_number = "49";

    // Generate image using ImageMagick
    generate_token_image(token_number, screen_width / 1.25, screen_height / 1.25);

    // Load generated image into GtkImage
    gtk_image_set_from_file(image, IMAGE_PATH);

    // Let the image scale with available space
    gtk_widget_set_hexpand(GTK_WIDGET(image), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(image), TRUE);

    // Show window
    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
