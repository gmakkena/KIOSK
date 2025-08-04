#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Global widgets
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_label, *ticker_label;
GtkWidget *top_fixed, *bottom_fixed;
GtkWidget *outer_pane, *inner_pane;

// Ticker state
int ticker_x = 0;
int ticker_width = 0;
int ticker_area_width = 0;
guint ticker_timer_id = 0;

void generate_token_image(GtkWidget *widget, const char *number, const char *label,
                          const char *filename, const char *bg, const char *fg,
                          float number_size_pct, float label_size_pct,
                          float number_offset_pct, float label_offset_pct,
                          const char *number_font, const char *label_font) {
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w < 100 || h < 100) return;

    int num_pt = (int)(h * number_size_pct);
    int lab_pt = (int)(h * label_size_pct);
    int num_off = (int)(h * number_offset_pct);
    int lab_off = (int)(h * label_offset_pct);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "convert -size %dx%d xc:%s "
        "-gravity center -fill %s -font %s "
        "-pointsize %d -annotate +0+%d \"%s\" "
        "-font %s -pointsize %d -annotate +0+%d \"%s\" "
        "%s",
        w, h, bg,
        fg, number_font,
        num_pt, num_off, number,
        label_font, lab_pt, lab_off, label,
        filename);

    system(cmd);
}

gboolean update_token_images(gpointer data) {
    generate_token_image(current_image, "49", "Current Draw", "current.png", "peachpuff", "red",
                         0.78, 0.18, -0.07, 0.37, "Arial-Bold", "Arial");
    generate_token_image(previous_image, "24", "Previous Draw", "previous.png", "peachpuff", "blue",
                         0.72, 0.10, -0.03, 0.3, "Arial-Bold", "Arial");
    generate_token_image(preceding_image, "05", "Preceding Draw", "preceding.png", "peachpuff", "brown",
                         0.90, 0.085, -0.10, 0.30, "Arial-Bold", "Arial");

    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");
    return FALSE;
}

gboolean animate_ticker(gpointer data) {
    ticker_x -= 2;
    if (ticker_x + ticker_width < 0)
        ticker_x = ticker_area_width;

    gtk_fixed_move(GTK_FIXED(bottom_fixed), ticker_label, ticker_x, 0);
    return G_SOURCE_CONTINUE;
}

gboolean start_ticker(gpointer data) {
    ticker_width = gtk_widget_get_allocated_width(ticker_label);
    ticker_area_width = gtk_widget_get_allocated_width(bottom_fixed);

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    ticker_x = ticker_area_width;
    ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);
    return G_SOURCE_REMOVE;
}

gboolean finalize_layout(gpointer win_ptr) {
    int screen_h = gdk_screen_get_height(gdk_screen_get_default());
    int screen_w = gdk_screen_get_width(gdk_screen_get_default());

    gtk_widget_set_size_request(top_fixed, -1, (int)(0.08 * screen_h));
    gtk_widget_set_size_request(bottom_fixed, -1, (int)(0.08 * screen_h));

    GtkAllocation outer_alloc;
    gtk_widget_get_allocation(outer_pane, &outer_alloc);
    gtk_paned_set_position(GTK_PANED(outer_pane), (int)(outer_alloc.width * 0.80));

    GtkAllocation inner_alloc;
    gtk_widget_get_allocation(inner_pane, &inner_alloc);
    gtk_paned_set_position(GTK_PANED(inner_pane), (int)(inner_alloc.height * 0.60));

    g_timeout_add(50, update_token_images, NULL);

    char top_markup[256];
    snprintf(top_markup, sizeof(top_markup),
        "<span font_family='Arial' weight='bold' size='%d' foreground='black'>Mega Event</span>",
        (int)(screen_h * 0.05 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(top_label), top_markup);

    char ticker_markup[256];
    snprintf(ticker_markup, sizeof(ticker_markup),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        (int)(screen_h * 0.08 * PANGO_SCALE));
    gtk_label_set_markup(GTK_LABEL(ticker_label), ticker_markup);

    g_timeout_add(100, start_ticker, NULL);
    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *b = gtk_builder_new_from_file("interface_image3.glade");

    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(b, "main"));
    current_image     = GTK_WIDGET(gtk_builder_get_object(b, "current_token"));
    previous_image    = GTK_WIDGET(gtk_builder_get_object(b, "previous_token"));
    preceding_image   = GTK_WIDGET(gtk_builder_get_object(b, "preceding_token"));
    top_label         = GTK_WIDGET(gtk_builder_get_object(b, "top_label"));
    ticker_label      = GTK_WIDGET(gtk_builder_get_object(b, "ticker_label"));
    top_fixed         = GTK_WIDGET(gtk_builder_get_object(b, "top_text"));
    bottom_fixed      = GTK_WIDGET(gtk_builder_get_object(b, "bottom_text"));
    outer_pane        = GTK_WIDGET(gtk_builder_get_object(b, "outer"));
    inner_pane        = GTK_WIDGET(gtk_builder_get_object(b, "inner"));

    if (!window || !current_image || !previous_image || !preceding_image ||
        !top_label || !ticker_label || !top_fixed || !bottom_fixed ||
        !outer_pane || !inner_pane) {
        g_printerr("Glade widgets missing.\n");
        return 1;
    }

    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_path(css, "style.css", NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);

    g_signal_connect(window, "realize", G_CALLBACK(finalize_layout), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
