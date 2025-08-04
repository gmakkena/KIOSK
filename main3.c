#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>

// Global widgets
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *outer_pane, *inner_pane;

// --------- ImageMagick Token Generation ---------
void generate_token_image(GtkWidget *widget, const char *token_text, const char *filename,
                          const char *bg, const char *fg, const char *stroke) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    if (width < 10 || height < 10) return;

    char command[512];
    snprintf(command, sizeof(command),
        "convert -background %s -fill %s -font Arial-Bold -gravity center "
        "-stroke %s -strokewidth 2 -size %dx%d caption:'%s' "
        "-extent %dx%d %s",
        bg, fg, stroke, width, height, token_text, width, height, filename);
    system(command);
}

// --------- Update Token Images ---------
gboolean update_token_images(gpointer user_data) {
    generate_token_image(current_image, "49", "current.png", "peachpuff", "red", "none");
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");

    generate_token_image(previous_image, "24", "previous.png", "peachpuff", "blue", "none");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");

    generate_token_image(preceding_image, "05", "preceding.png", "peachpuff", "brown", "none");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");

    return FALSE;
}

// --------- Set GtkPaned Positions Dynamically ---------
gboolean set_paned_ratios(gpointer user_data) {
    if (!outer_pane || !inner_pane) return FALSE;

    GdkScreen *screen = gdk_screen_get_default();
    int screen_width = gdk_screen_get_width(screen);
    int screen_height = gdk_screen_get_height(screen);

    gtk_paned_set_position(GTK_PANED(outer_pane), (int)(screen_width * 0.70));

    int inner_height = gtk_widget_get_allocated_height(inner_pane);
    if (inner_height > 0) {
        gtk_paned_set_position(GTK_PANED(inner_pane), (int)(inner_height * 0.70));
    } else {
        gtk_paned_set_position(GTK_PANED(inner_pane), (int)(screen_height * 0.70));
    }

    g_timeout_add(50, update_token_images, NULL);
    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_image.glade");
    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));

    // Load widgets
    current_image   = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image  = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    outer_pane      = GTK_WIDGET(gtk_builder_get_object(builder, "outer_pane"));
    inner_pane      = GTK_WIDGET(gtk_builder_get_object(builder, "inner_pane"));

    if (!window || !current_image || !previous_image || !preceding_image || !outer_pane || !inner_pane) {
        g_printerr("Error: Missing widgets in Glade.\n");
        return 1;
    }

    // Fullscreen kiosk
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    // Load CSS styling
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );

    // Show all widgets
    gtk_widget_show_all(window);

    // Set paned sizes and trigger image updates
    g_timeout_add(100, set_paned_ratios, NULL);

    gtk_main();
    return 0;
}
