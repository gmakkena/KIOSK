#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ===================== Widgets =====================
GtkWidget *window;
GtkWidget *top_label;
GtkWidget *current_image, *previous_image, *preceding_image;
GtkWidget *top_pane, *outermost, *outer, *inner;
GtkWidget *ticker_fixed, *ticker_label;
GtkWidget *gif_area = NULL;

// ===================== GIF Player =====================
typedef struct {
    GdkPixbufAnimation *animation;
    GdkPixbufAnimationIter *iter;
    guint timeout_id;
    GtkWidget *drawing_area;
    GTimer *timer;
} GifPlayer;

static GifPlayer *gif_player = NULL;

// ===================== Tokens =====================
char current_token[32] = "--";
char previous_token[32] = "--";
char preceding_token[32] = "--";

// ===================== Helper =====================
static void refocus_main_window(GtkWidget *win) {
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_present(GTK_WINDOW(win));
        if (gtk_widget_get_window(win))
            gdk_window_raise(gtk_widget_get_window(win));
    }
}

// ===================== GIF HANDLERS =====================
static gboolean gif_player_advance(gpointer data) {
    if (!gif_player || !gif_player->iter) return G_SOURCE_REMOVE;
    gdouble elapsed_ms = g_timer_elapsed(gif_player->timer, NULL) * 1000.0;
    int delay = gdk_pixbuf_animation_iter_get_delay_time(gif_player->iter);
    if (delay < 0) delay = 100;
    if (elapsed_ms >= delay) {
        gdk_pixbuf_animation_iter_advance(gif_player->iter, NULL);
        gtk_widget_queue_draw(gif_player->drawing_area);
        g_timer_start(gif_player->timer);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean gif_player_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!gif_player || !gif_player->iter) return FALSE;
    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(gif_player->iter);
    if (!frame) return FALSE;
    int da_w = gtk_widget_get_allocated_width(widget);
    int da_h = gtk_widget_get_allocated_height(widget);
    int fw = gdk_pixbuf_get_width(frame);
    int fh = gdk_pixbuf_get_height(frame);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    double scale = (double)da_h / (double)fh;
    int scaled_w = (int)(fw * scale);
    int x_offset = (da_w - scaled_w) / 2;
    cairo_save(cr);
    cairo_translate(cr, x_offset, 0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, frame, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static void gif_player_cleanup(void) {
    if (!gif_player) return;
    if (gif_player->timeout_id) g_source_remove(gif_player->timeout_id);
    if (gif_player->animation) g_object_unref(gif_player->animation);
    if (gif_player->iter) g_object_unref(gif_player->iter);
    if (gif_player->timer) g_timer_destroy(gif_player->timer);
    g_free(gif_player);
    gif_player = NULL;
}

static gboolean show_fullscreen_gif(gpointer filename_ptr) {
    const char *filename = (const char *)filename_ptr;
    if (!gif_area) return FALSE;
    if (gif_player) gif_player_cleanup();

    gif_player = g_new0(GifPlayer, 1);
    gif_player->drawing_area = gif_area;
    g_signal_connect(gif_area, "draw", G_CALLBACK(gif_player_draw), NULL);

    GError *error = NULL;
    gif_player->animation = gdk_pixbuf_animation_new_from_file(filename, &error);
    if (error || !gif_player->animation) {
        g_printerr("GIF Error: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        gif_player_cleanup();
        return FALSE;
    }
    gif_player->iter = gdk_pixbuf_animation_get_iter(gif_player->animation, NULL);
    gif_player->timer = g_timer_new();
    g_timer_start(gif_player->timer);
    gtk_widget_set_visible(gif_area, TRUE);
    gif_player->timeout_id = g_timeout_add(10, gif_player_advance, NULL);
    refocus_main_window(window);
    return FALSE;
}

static gboolean hide_overlay_gif(gpointer user_data) {
    if (gif_area) gtk_widget_set_visible(gif_area, FALSE);
    gif_player_cleanup();
    refocus_main_window(window);
    return FALSE;
}

// ===================== TOKEN DRAWING (CAIRO + PANGO) =====================
static GdkPixbuf *render_token_pixbuf(GtkWidget *widget,
                                     const char *number, const char *label,
                                     const char *bg_hex,
                                     const char *num_fg_hex,
                                     const char *label_fg_hex,
                                     double number_frac, double label_frac,
                                     const char *num_font, const char *lab_font)
{
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w < 100 || h < 100) { w = 600; h = 300; }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);

    unsigned int r,g,b;
    double br=1,bg=1,bb=1;
    if (bg_hex && sscanf(bg_hex,"#%02x%02x%02x",&r,&g,&b)==3)
        br=r/255.0,bg=g/255.0,bb=b/255.0;
    cairo_set_source_rgb(cr,br,bg,bb);
    cairo_paint(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    char fontdesc[128];

    // number
    snprintf(fontdesc,sizeof(fontdesc),"%s %d",num_font,(int)(h*number_frac));
    PangoFontDescription *fdn = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout,fdn);
    pango_layout_set_text(layout, number ? number : "--",-1);
    int tw,th; pango_layout_get_pixel_size(layout,&tw,&th);
    int tx=(w-tw)/2, ty=(h-th)/2 - (h*0.05);
    double fr=1,fgc=0,fb=0;
    if (fg_hex && sscanf(fg_hex,"#%02x%02x%02x",&r,&g,&b)==3)
        fr=r/255.0,fgc=g/255.0,fb=b/255.0;
    cairo_set_source_rgb(cr,fr,fgc,fb);
    cairo_move_to(cr,tx,ty);
    pango_cairo_show_layout(cr,layout);
    pango_font_description_free(fdn);

    // label
    snprintf(fontdesc,sizeof(fontdesc),"%s %d",lab_font,(int)(h*label_frac));
    PangoFontDescription *fdl = pango_font_description_from_string(fontdesc);
    pango_layout_set_font_description(layout,fdl);
    pango_layout_set_text(layout,label ? label : "",-1);
    pango_layout_get_pixel_size(layout,&tw,&th);
    tx=(w-tw)/2; ty=(h*0.8);
    //cairo_set_source_rgb(cr,0.2,0.2,0.2);
    double lr = 0.2, lg = 0.2, lb = 0.2;
unsigned int r,g,b;
if (label_fg_hex && sscanf(label_fg_hex,"#%02x%02x%02x",&r,&g,&b)==3)
    lr = r/255.0, lg = g/255.0, lb = b/255.0;
cairo_set_source_rgb(cr, lr, lg, lb);

    cairo_move_to(cr,tx,ty);
    pango_cairo_show_layout(cr,layout);
    pango_font_description_free(fdl);
    g_object_unref(layout);

    cairo_surface_flush(surf);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(surf,0,0,w,h);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return pix;
}

static gboolean refresh_images_on_ui(gpointer user_data) {
  GdkPixbuf *pb1 = render_token_pixbuf(
    current_image, current_token, "Current Draw",
    "#FFDAB9",        // background
    "#FF0000",        // number color (red)
    "#202020",        // label color (dark gray)
    0.7, 0.15, "Liberation Sans Bold", "Liberation Sans");

GdkPixbuf *pb2 = render_token_pixbuf(
    previous_image, previous_token, "Previous Draw",
    "#FFDAB9",
    "#0000FF",        // number color (blue)
    "#000080",        // label color (navy)
    0.6, 0.12, "Liberation Sans Bold", "Liberation Sans");

GdkPixbuf *pb3 = render_token_pixbuf(
    preceding_image, preceding_token, "Preceding Draw",
    "#FFDAB9",
    "#8B4513",        // number color (brown)
    "#5C4033",        // label color (dark brown)
    0.6, 0.12, "Liberation Sans Bold", "Liberation Sans");
 if(pb1){gtk_image_set_from_pixbuf(GTK_IMAGE(current_image),pb1);g_object_unref(pb1);}
    if(pb2){gtk_image_set_from_pixbuf(GTK_IMAGE(previous_image),pb2);g_object_unref(pb2);}
    if(pb3){gtk_image_set_from_pixbuf(GTK_IMAGE(preceding_image),pb3);g_object_unref(pb3);}
    return FALSE;
}

// ===================== PANED RATIOS =====================
static gboolean set_paned_ratios(gpointer user_data) {
    gtk_paned_set_wide_handle(GTK_PANED(top_pane), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outermost), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(outer), FALSE);
    gtk_paned_set_wide_handle(GTK_PANED(inner), FALSE);

    GtkAllocation top_alloc, outermost_alloc, outer_alloc, inner_alloc;
    gtk_widget_get_allocation(top_pane, &top_alloc);
    gtk_widget_get_allocation(outermost, &outermost_alloc);
    gtk_widget_get_allocation(outer, &outer_alloc);
    gtk_widget_get_allocation(inner, &inner_alloc);

    // Provide sensible fallbacks if GTK hasn’t allocated yet
    if (top_alloc.height == 0) top_alloc.height = 100;
    if (outermost_alloc.height == 0) outermost_alloc.height = 800;
    if (outer_alloc.width == 0) outer_alloc.width = 1200;
    if (inner_alloc.height == 0) inner_alloc.height = 600;

    // Proper proportional layout (similar to your original Glade look)
    gtk_paned_set_position(GTK_PANED(top_pane),  top_alloc.height * 0.12); // top label visible
    gtk_paned_set_position(GTK_PANED(outermost), outermost_alloc.height * 0.88);
    gtk_paned_set_position(GTK_PANED(outer),     outer_alloc.width * 0.72);
    gtk_paned_set_position(GTK_PANED(inner),     inner_alloc.height * 0.70);

    // Force label and ticker visible
    gtk_widget_show(top_label);
    gtk_widget_show(ticker_fixed);
    gtk_widget_show(ticker_label);

    // Update fonts
    int top_font_size = (int)(outermost_alloc.height * 0.08 * 0.9 * PANGO_SCALE);
    int ticker_font_size = (int)(outermost_alloc.height * 0.045 * 0.9 * PANGO_SCALE);

    char markup_top[256];
    snprintf(markup_top, sizeof(markup_top),
        "<span font_family='Fira Sans' weight='bold' size='%d' foreground='#8B0000'>Aurum Mega Event</span>",
        top_font_size);
    gtk_label_set_markup(GTK_LABEL(top_label), markup_top);

    char markup_ticker[256];
    snprintf(markup_ticker, sizeof(markup_ticker),
        "<span font_family='Arial' weight='bold' size='%d' foreground='#2F4F4F'>Aurum Smart Tech</span>",
        ticker_font_size);
    gtk_label_set_markup(GTK_LABEL(ticker_label), markup_ticker);

    // Refresh tokens
    g_idle_add(refresh_images_on_ui, NULL);
    return G_SOURCE_REMOVE;
}


// ===================== SERIAL READER =====================
static void shift_tokens(const char *new_token){
    strncpy(preceding_token,previous_token,sizeof(preceding_token));
    strncpy(previous_token,current_token,sizeof(previous_token));
    strncpy(current_token,new_token,sizeof(current_token));
}

static gboolean update_ui_from_serial(gpointer user_data){
    g_idle_add(refresh_images_on_ui,NULL);
    return FALSE;
}

static void *serial_reader_thread(void *arg){
    const char *serial_port="/dev/serial0";
    int fd=open(serial_port,O_RDWR|O_NOCTTY|O_NDELAY);
    if(fd==-1){perror("serial open");return NULL;}

    struct termios options;
    tcgetattr(fd,&options);
    cfsetispeed(&options,B9600);
    cfsetospeed(&options,B9600);
    options.c_cflag|=(CLOCAL|CREAD);
    options.c_cflag&=~CSIZE;options.c_cflag|=CS8;
    options.c_cflag&=~PARENB;options.c_cflag&=~CSTOPB;
    options.c_cflag&=~CRTSCTS;options.c_iflag=IGNPAR;
    options.c_oflag=0;options.c_lflag=0;
    options.c_cc[VMIN]=0;options.c_cc[VTIME]=1;
    tcsetattr(fd,TCSANOW,&options);

    char buf[256];size_t pos=0;char rbuf[64];
    while(1){
        int n=read(fd,rbuf,sizeof(rbuf));
        if(n>0){
            for(int i=0;i<n;i++){
                char c=rbuf[i];
                if(c=='\r'||c=='\n'){
                    if(pos==0)continue;
                    buf[pos]='\0';pos=0;
                    char *p=buf;while(*p==' ')p++;
                    char *save=NULL;
                    char *f0=strtok_r(p," ",&save);
                    char *f1=strtok_r(NULL," ",&save);
                    char *f2=strtok_r(NULL," ",&save);
                    if(f0&&f0[0]==':'){
                        if(f1&&f2&&strcmp(f1,"1")==0){
                            shift_tokens(f2);
                            g_idle_add(update_ui_from_serial,NULL);
                        }else if(f1&&strcmp(f1,"3")==0){
                            g_idle_add(show_fullscreen_gif,(gpointer)"rolling.gif");
                        }
                    }else if(f0&&strcmp(f0,"$M")==0&&f1){
                        if(!strcmp(f1,"G1")){
                            g_idle_add(show_fullscreen_gif,(gpointer)"gameover1.gif");
                            strcpy(current_token,"--");
                            strcpy(previous_token,"--");
                            strcpy(preceding_token,"--");
                            g_idle_add(update_ui_from_serial,NULL);
                        }else if(!strcmp(f1,"C1")){
                            g_idle_add(show_fullscreen_gif,(gpointer)"congratulations1.gif");
                        }else g_idle_add(hide_overlay_gif,NULL);
                    }else if(f0){
                        shift_tokens(f0);
                        g_idle_add(update_ui_from_serial,NULL);
                    }
                }else if(pos+1<sizeof(buf))buf[pos++]=c;
            }
        }else usleep(20000);
    }
    close(fd);
    return NULL;
}

// ===================== MAIN =====================
int main(int argc,char *argv[]){
    gtk_init(&argc,&argv);

    GtkBuilder *builder=gtk_builder_new_from_file("interface_paned_overlay.glade");
    window=GTK_WIDGET(gtk_builder_get_object(builder,"main"));
    top_label=GTK_WIDGET(gtk_builder_get_object(builder,"top_label"));
    current_image=GTK_WIDGET(gtk_builder_get_object(builder,"current_image"));
    previous_image=GTK_WIDGET(gtk_builder_get_object(builder,"previous_image"));
    preceding_image=GTK_WIDGET(gtk_builder_get_object(builder,"preceding_image"));
    top_pane=GTK_WIDGET(gtk_builder_get_object(builder,"top_pane"));
    outermost=GTK_WIDGET(gtk_builder_get_object(builder,"outermost"));
    outer=GTK_WIDGET(gtk_builder_get_object(builder,"outer"));
    inner=GTK_WIDGET(gtk_builder_get_object(builder,"inner"));
    ticker_fixed=GTK_WIDGET(gtk_builder_get_object(builder,"ticker_fixed"));
    ticker_label=GTK_WIDGET(gtk_builder_get_object(builder,"ticker_label"));
    gif_area=GTK_WIDGET(gtk_builder_get_object(builder,"gif_area"));

    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_path(css,"style.css",NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_widget_show_all(window);
    GdkScreen *screen=gdk_screen_get_default();
    gtk_window_resize(GTK_WINDOW(window),gdk_screen_get_width(screen),gdk_screen_get_height(screen));
    gtk_window_fullscreen(GTK_WINDOW(window));
    gtk_window_set_decorated(GTK_WINDOW(window),FALSE);

    g_timeout_add(200, set_paned_ratios, NULL);
g_timeout_add(800, set_paned_ratios, NULL);
    gtk_widget_set_visible(gif_area,FALSE);

    pthread_t serial_thread;
    pthread_create(&serial_thread,NULL,serial_reader_thread,NULL);
    pthread_detach(serial_thread);

    gtk_main();
    return 0;
}
