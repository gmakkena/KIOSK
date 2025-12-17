// ==========================================================
//  TOKEN DISPLAY + GAME OVER OVERLAY (GTK3)
// ==========================================================

#include <gtk/gtk.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <stdio.h>

// ==========================================================
// GLOBALS
// ==========================================================

GtkWidget *window;
GtkWidget *overlay;
GtkWidget *content_box;
GtkWidget *current_img, *prev_img, *prev2_img;
GtkWidget *overlay_area;

int serial_fd = -1;

// Token values
char current_token[16]  = "--";
char previous_token[16] = "--";
char preceding_token[16]= "--";

// Overlay mode
typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_GAME_OVER
} OverlayMode;

OverlayMode overlay_mode = OVERLAY_NONE;

// ==========================================================
// CAIRO TOKEN RENDER
// ==========================================================

static GdkPixbuf *render_token(const char *num, const char *label,
                               const char *num_color,
                               int w, int h)
{
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);

    // background
    cairo_set_source_rgb(cr, 1.0, 0.85, 0.75);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    // number
    char font[64];
    snprintf(font, sizeof(font), "Liberation Sans Bold %d", h * 0.45);
    PangoFontDescription *fd =
        pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, num, -1);

    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    unsigned r,g,b;
    sscanf(num_color, "#%02x%02x%02x", &r,&g,&b);
    cairo_set_source_rgb(cr, r/255.0, g/255.0, b/255.0);

    cairo_move_to(cr, (w-tw)/2, h*0.2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(fd);

    // label
    snprintf(font, sizeof(font), "Liberation Sans %d", h * 0.12);
    fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, label, -1);

    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_set_source_rgb(cr, 0.2,0.2,0.2);
    cairo_move_to(cr, (w-tw)/2, h*0.75);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(fd);
    g_object_unref(layout);

    GdkPixbuf *pb =
        gdk_pixbuf_get_from_surface(s,0,0,w,h);

    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return pb;
}

static void refresh_tokens(void)
{
    GdkPixbuf *p1 = render_token(current_token,"CURRENT","#C62828",600,300);
    GdkPixbuf *p2 = render_token(previous_token,"PREVIOUS","#1565C0",500,250);
    GdkPixbuf *p3 = render_token(preceding_token,"BEFORE","#4E342E",500,250);

    gtk_image_set_from_pixbuf(GTK_IMAGE(current_img), p1);
    gtk_image_set_from_pixbuf(GTK_IMAGE(prev_img), p2);
    gtk_image_set_from_pixbuf(GTK_IMAGE(prev2_img), p3);

    g_object_unref(p1);
    g_object_unref(p2);
    g_object_unref(p3);
}

// ==========================================================
// OVERLAY DRAW
// ==========================================================

static gboolean overlay_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    if (overlay_mode != OVERLAY_GAME_OVER)
        return FALSE;

    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    cairo_set_source_rgb(cr, 0,0,0);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    char font[64];
    snprintf(font,sizeof(font),"Liberation Sans Bold %d",(int)(H*0.18));
    PangoFontDescription *fd =
        pango_font_description_from_string(font);

    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, "GAME OVER", -1);

    int tw, th;
    pango_layout_get_pixel_size(layout,&tw,&th);

    cairo_set_source_rgb(cr,0.9,0.1,0.1);
    cairo_move_to(cr,(W-tw)/2,(H-th)/2);
    pango_cairo_show_layout(cr,layout);

    pango_font_description_free(fd);
    g_object_unref(layout);
    return FALSE;
}

// ==========================================================
// OVERLAY CONTROL
// ==========================================================

static gboolean show_game_over(gpointer d)
{
    overlay_mode = OVERLAY_GAME_OVER;
    gtk_widget_show(overlay_area);
    gtk_widget_queue_draw(overlay_area);
    return FALSE;
}

static gboolean hide_overlay(gpointer d)
{
    overlay_mode = OVERLAY_NONE;
    gtk_widget_hide(overlay_area);
    return FALSE;
}

// ==========================================================
// TOKEN SHIFT
// ==========================================================

static void shift_tokens(const char *n)
{
    strcpy(preceding_token, previous_token);
    strcpy(previous_token, current_token);
    strncpy(current_token, n, sizeof(current_token)-1);
}

// ==========================================================
// SERIAL THREAD
// ==========================================================

static void *serial_thread(void *d)
{
    char buf[128];
    int pos = 0;
    char c;

    while (1) {
        if (read(serial_fd,&c,1) <= 0) {
            usleep(20000);
            continue;
        }

        if (c=='\n' || c=='\r') {
            buf[pos]=0;
            pos=0;

            char *f0=strtok(buf," ");
            char *f1=strtok(NULL," ");
            char *f2=strtok(NULL," ");

            if (f0 && !strcmp(f0,":01") &&
                f1 && !strcmp(f1,"1") && f2) {

                g_idle_add(hide_overlay,NULL);
                shift_tokens(f2);
                g_idle_add((GSourceFunc)refresh_tokens,NULL);
            }

            else if (f0 && !strcmp(f0,":03") &&
                     f1 && !strcmp(f1,"1") &&
                     f2 && !strcmp(f2,"6A")) {

                g_idle_add(show_game_over,NULL);
            }
        }
        else if (pos < sizeof(buf)-1) {
            buf[pos++]=c;
        }
    }
    return NULL;
}

// ==========================================================
// MAIN
// ==========================================================

int main(int argc,char **argv)
{
    gtk_init(&argc,&argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window),FALSE);
    gtk_window_fullscreen(GTK_WINDOW(window));
    g_signal_connect(window,"destroy",gtk_main_quit,NULL);

    overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window),overlay);

    content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
    gtk_container_add(GTK_CONTAINER(overlay),content_box);

    current_img = gtk_image_new();
    prev_img    = gtk_image_new();
    prev2_img   = gtk_image_new();

    gtk_box_pack_start(GTK_BOX(content_box),current_img,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(content_box),prev_img,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(content_box),prev2_img,TRUE,TRUE,0);

    overlay_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(overlay_area,TRUE);
    gtk_widget_set_vexpand(overlay_area,TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),overlay_area);
    g_signal_connect(overlay_area,"draw",
                     G_CALLBACK(overlay_draw),NULL);
    gtk_widget_hide(overlay_area);

    gtk_widget_show_all(window);
    refresh_tokens();

    // Serial
    serial_fd = open("/dev/serial0",O_RDONLY|O_NOCTTY);
    struct termios t;
    tcgetattr(serial_fd,&t);
    cfsetispeed(&t,B9600);
    cfsetospeed(&t,B9600);
    t.c_cflag |= CLOCAL|CREAD;
    tcsetattr(serial_fd,TCSANOW,&t);

    pthread_t th;
    pthread_create(&th,NULL,serial_thread,NULL);
    pthread_detach(th);

    gtk_main();
    return 0;
}
