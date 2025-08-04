#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Widgets
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;

// Ticker
int ticker_x = 0, ticker_width = 0, ticker_area_width = 0;
guint ticker_timer_id = 0;

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

gboolean update_token_images(gpointer user_data) {
    generate_token_image(current_image, "49", "Current Draw", "current.png", "peachpuff", "red",
                         0.78, 0.18, 0.1, -0.07, 0.07, 0.40,
                         "Arial-Bold", "Arial");

    generate_token_image(previous_image, "24", "Previous Draw", "previous.png", "peachpuff", "blue",
                         0.70, 0.10, -0.04, -0.03, -0.05, 0.30,
                         "Arial-Bold", "Arial");

    generate_token_image(preceding_image, "05", "Preceding Draw", "preceding.png", "peachpuff", "brown",
                         0.92, 0.10, -0.08, -0.1, -0.05, 0.30,
                         "Arial-Bold", "Arial");

    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");

    return FALSE;
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
gboolean set_paned_ratios( gpointer user_data) {
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

    GdkRectangle monitor_geometry;
    GdkMonitor *monitor = gdk_display_get_primary_monitor(gdk_display_get_default());
    gdk_monitor_get_geometry(monitor, &monitor_geometry);
    int h = monitor_geometry.height;

    // Top label
    char markup_top[256];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#8B0000'>Aurum Mega Event</span>",
        (int)(h * 0.059 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);

    // Ticker
    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        (int)(h * 0.045 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);

    g_timeout_add(50, update_token_images, NULL);
    g_timeout_add(100, finalize_ticker_setup, NULL);

    return G_SOURCE_REMOVE;
}

// ---------- Main ----------
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

    // Optional CSS
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css_provider, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_idle_add(set_paned_ratios, NULL);
  //g_signal_connect(window, "realize", G_CALLBACK(set_paned_ratios), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
