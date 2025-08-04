#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Global widgets
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *outer_pane, *inner_pane, *outermost_pane;
GtkWidget *ticker_label, *ticker_fixed;

// Ticker state
int ticker_x = 0;
int ticker_width = 0;
int ticker_area_width = 0;
guint ticker_timer_id = 0;

// ---------- Generate Token + Label Image ----------
void generate_token_image(GtkWidget *widget, const char *number, const char *label,
                          const char *filename, const char *bg, const char *fg) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    if (width < 100 || height < 100) return;

    // 80% height for number, 20% for label
    int number_pointsize = (int)(height * 0.6);
    int label_pointsize  = (int)(height * 0.12);
    int number_y_offset  = (int)(-height * 0.1);
    int label_y_offset   = (int)(height * 0.20);

    char command[1024];
    snprintf(command, sizeof(command),
        "convert -size %dx%d xc:%s "
        "-gravity center -fill %s -font Arial-Bold "
        "-pointsize %d -annotate +0+%d \"%s\" "
        "-pointsize %d -annotate +0+%d \"%s\" "
        "%s",
        width, height, bg,
        fg,
        number_pointsize, number_y_offset, number,
        label_pointsize, label_y_offset, label,
        filename);

    system(command);
}

// ---------- Update Token Images ----------
gboolean update_token_images(gpointer user_data) {
    generate_token_image(current_image, "49", "Current Draw", "current.png", "peachpuff", "red");
    gtk_image_set_from_file(GTK_IMAGE(current_image), "current.png");

    generate_token_image(previous_image, "24", "Previous Draw", "previous.png", "peachpuff", "blue");
    gtk_image_set_from_file(GTK_IMAGE(previous_image), "previous.png");

    generate_token_image(preceding_image, "05", "Preceding Draw", "preceding.png", "peachpuff", "brown");
    gtk_image_set_from_file(GTK_IMAGE(preceding_image), "preceding.png");

    return FALSE;
}

// ---------- Animate Ticker ----------
gboolean animate_ticker(gpointer data) {
    if (!GTK_IS_WIDGET(ticker_label) || !GTK_IS_WIDGET(ticker_fixed))
        return G_SOURCE_REMOVE;

    ticker_x -= 2;
    if (ticker_x + ticker_width < 0)
        ticker_x = ticker_area_width;

    gtk_fixed_move(GTK_FIXED(ticker_fixed), ticker_label, ticker_x, 10);
    return G_SOURCE_CONTINUE;
}

// ---------- Finalize ticker setup ----------
gboolean finalize_ticker_setup(gpointer data) {
    if (!GTK_IS_WIDGET(ticker_label) || !GTK_IS_WIDGET(ticker_fixed))
        return G_SOURCE_REMOVE;

    ticker_width = gtk_widget_get_allocated_width(ticker_label);
    ticker_area_width = gtk_widget_get_allocated_width(ticker_fixed);

    if (ticker_width <= 1 || ticker_area_width <= 1)
        return G_SOURCE_CONTINUE;

    ticker_x = ticker_area_width;
    ticker_timer_id = g_timeout_add(30, animate_ticker, NULL);
    return G_SOURCE_REMOVE;
}

// ---------- Set pane ratios ----------
gboolean set_paned_ratios(gpointer user_data) {
    GtkWidget *window = GTK_WIDGET(user_data);
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    int screen_width = gdk_window_get_width(gdk_window);
    int screen_height = gdk_window_get_height(gdk_window);

    gtk_paned_set_position(GTK_PANED(outermost_pane), (int)(screen_height * 0.90));
    gtk_paned_set_position(GTK_PANED(outer_pane), (int)(screen_width * 0.60));

    GtkAllocation alloc;
    gtk_widget_get_allocation(inner_pane, &alloc);
    gtk_paned_set_position(GTK_PANED(inner_pane), (int)(alloc.height * 0.60));

    g_timeout_add(50, update_token_images, NULL);

    gtk_label_set_text(GTK_LABEL(ticker_label), "Aurum Smart Tech");

    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_size_new((int)(screen_height * 0.25 * PANGO_SCALE)));
    gtk_label_set_attributes(GTK_LABEL(ticker_label), attrs);
    pango_attr_list_unref(attrs);

    g_timeout_add(100, finalize_ticker_setup, NULL);
    return FALSE;
}

// ---------- Main ----------
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder = gtk_builder_new_from_file("interface_image2.glade");
    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main"));

    current_image     = GTK_WIDGET(gtk_builder_get_object(builder, "current_image"));
    previous_image    = GTK_WIDGET(gtk_builder_get_object(builder, "previous_image"));
    preceding_image   = GTK_WIDGET(gtk_builder_get_object(builder, "preceding_image"));
    outer_pane        = GTK_WIDGET(gtk_builder_get_object(builder, "outer_pane"));
    inner_pane        = GTK_WIDGET(gtk_builder_get_object(builder, "inner_pane"));
    outermost_pane    = GTK_WIDGET(gtk_builder_get_object(builder, "Outermost"));
    ticker_fixed      = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_fixed"));
    ticker_label      = GTK_WIDGET(gtk_builder_get_object(builder, "ticker_label"));

    if (!window || !current_image || !previous_image || !preceding_image ||
        !outer_pane || !inner_pane || !outermost_pane ||
        !ticker_fixed || !ticker_label) {
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
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
