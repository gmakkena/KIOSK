#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Global widgets
GtkWidget *current_image, *previous_image, *preceding_image, *ticker_image;
GtkWidget *outer_pane, *inner_pane, *outermost_pane;

// --------- Generate Token + Label Image ---------
void generate_token_image(GtkWidget *widget, const char *number, const char *label,
                          const char *filename, const char *bg, const char *fg) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    if (width < 100 || height < 100) return;

    int base = width < height ? width : height;
    int number_size = (int)(base * 0.55);
    int label_size  = (int)(base * 0.05);

    char markup[768];
    snprintf(markup, sizeof(markup),
        "<span font='Arial Bold %d' foreground='%s'>%s</span>\n"
        "<span font='Arial %d' foreground='%s'>%s</span>",
        number_size, fg, number, label_size, fg, label);

    char command[1024];
    snprintf(command, sizeof(command),
        "convert -background %s -fill %s -gravity center "
        "-define pango:wrap=word -define pango:auto-dir=false "
        "pango:\"%s\" -extent %dx%d %s",
        bg, fg, markup, width, height, filename);

    system(command);
}

// --------- Generate Ticker Image ---------
void generate_ticker_image(GtkWidget *widget, const char *text,
                           const char *filename, const char *bg, const char *fg) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    if (width < 100 || height < 20) return;

    int font_size = (int)(height * 0.6);
    char markup[512];
    snprintf(markup, sizeof(markup),
        "<span font='Arial Bold %d' foreground='%s'>%s</span>",
        font_size, fg, text);

    char command[1024];
    snprintf(command, sizeof(command),
        "convert -background %s -fill %s -gravity west "
        "-define pango:wrap=word -define pango:auto-dir=false "
        "pango:\"%s\" -extent %dx%d %s",
        bg, fg, markup, width, height, filename);

    system(command);
}

// --------- Update All Images ---------
gboolean update_token_images(gpointer user_data) {
    generate_token_image(current_image,  "49", "Current Draw",   "current.png",   "peachpuff", "red");
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");

    generate_token_image(previous_image, "24", "Previous Draw",  "previous.png",  "peachpuff", "blue");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");

    generate_token_image(preceding_image,"05", "Preceding Draw", "preceding.png", "peachpuff", "brown");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");

    generate_ticker_image(ticker_image, "Aurum Smart Tech", "ticker.png", "peachpuff", "darkred");
    gtk_image_set_from_file(GTK_IMAGE(ticker_image), "ticker.png");

    return FALSE;
}

// --------- Set Pane Ratios ---------
gboolean set_paned_ratios(gpointer user_data) {
    GtkWidget *window = GTK_WIDGET(user_data);
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    int screen_width = gdk_window_get_width(gdk_window);
    int screen_height = gdk_window_get_height(gdk_window);

    gtk_paned_set_position(GTK_PANED(outermost_pane), (int)(screen_height * 0.90));
    gtk_paned_set_position(GTK_PANED(outer_pane),     (int)(screen_width * 0.65));

    GtkAllocation alloc;
    gtk_widget_get_allocation(inner_pane, &alloc);
    gtk_paned_set_position(GTK_PANED(inner_pane), (int)(alloc.height * 0.68));

    g_timeout_add(50, update_token_images, NULL);
    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_image2.glade");

    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));
    if (!window) { g_printerr("Missing: main\n"); return 1; }

    current_image     = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image    = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image   = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    ticker_image      = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_image"));

    outer_pane        = GTK_WIDGET(gtk_builder_get_object(builder, "outer_pane"));
    inner_pane        = GTK_WIDGET(gtk_builder_get_object(builder, "inner_pane"));
    outermost_pane    = GTK_WIDGET(gtk_builder_get_object(builder, "Outermost"));

    if (!current_image || !previous_image || !preceding_image || !ticker_image ||
        !outer_pane || !inner_pane || !outermost_pane) {
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
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );

    g_signal_connect(window, "realize", G_CALLBACK(set_paned_ratios), window);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
