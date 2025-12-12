#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

GtkWidget *window;
GtkWidget *image;

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    if (argc < 2) {
        g_printerr("Usage: gif_viewer <gif_file>\n");
        return 1;
    }

    const char *gif_file = argv[1];

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(window));

    image = gtk_image_new_from_file(gif_file);
    gtk_container_add(GTK_CONTAINER(window), image);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);

    gtk_main();
    return 0;
}
