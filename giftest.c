#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gdk/gdkx.h>
#include <gst/video/videooverlay.h>

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    GtkWidget *drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(window), drawing_area);

    gst_element_set_state(gst_element_factory_make("playbin", "player"), GST_STATE_NULL);

    GstElement *pipeline = gst_element_factory_make("playbin", "player");

    // You can also use MP4 here — but this works with animated GIF too!
    g_object_set(pipeline, "uri", "/home/gmakkena/GLADE/Kiosk/congratulations1.gif", NULL);

    // Show window before linking video overlay
    gtk_widget_show_all(window);

    // Link GTK window to GStreamer video sink
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(pipeline),
        GDK_WINDOW_XID(gtk_widget_get_window(drawing_area)));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_main();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
}
