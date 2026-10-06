/* A GTK 3 program drawing with the pen (test/pen-gate.sh): prints each
 * motion, button and touch event GTK gives its window, with the device's
 * kind and the pressure and tilt GTK read --
 *   motion|press|release source=pen|eraser|mouse|touch|other dev=NAME pressure=P xtilt=X ytilt=Y
 *   touch-begin|touch-update|touch-end
 * (P from 0 to 1, -1: none). GTK's own headers are not needed (the image
 * build host has the library only), so the few calls are declared here.
 * Build: cc -o pengtk-probe pengtk-probe.c -l:libgtk-3.so.0 -l:libgdk-3.so.0 -l:libgobject-2.0.so.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _GtkWidget GtkWidget;
typedef union _GdkEvent GdkEvent;
typedef struct _GdkDevice GdkDevice;
typedef int gboolean;

extern void gtk_init( int *argc, char ***argv );
extern GtkWidget *gtk_window_new( int type );
extern void gtk_window_set_title( GtkWidget *w, const char *title );
extern void gtk_window_set_default_size( GtkWidget *w, int width, int height );
extern void gtk_widget_add_events( GtkWidget *w, int events );
extern void gtk_widget_show_all( GtkWidget *w );
extern void gtk_main( void );
extern unsigned long g_signal_connect_data( void *instance, const char *signal, void *handler, void *data,
                                            void *destroy, int flags );
extern int gdk_event_get_event_type( const GdkEvent *e );
extern GdkDevice *gdk_event_get_source_device( const GdkEvent *e );
extern gboolean gdk_event_get_axis( const GdkEvent *e, int axis_use, double *value );
extern int gdk_device_get_source( GdkDevice *d );
extern const char *gdk_device_get_name( GdkDevice *d );

enum { GDK_MOTION_NOTIFY = 3, GDK_BUTTON_PRESS = 4, GDK_BUTTON_RELEASE = 7,
       GDK_TOUCH_BEGIN = 37, GDK_TOUCH_UPDATE = 38, GDK_TOUCH_END = 39 };
enum { GDK_AXIS_PRESSURE = 3, GDK_AXIS_XTILT = 4, GDK_AXIS_YTILT = 5 };
enum { GDK_POINTER_MOTION_MASK = 1 << 2, GDK_BUTTON_PRESS_MASK = 1 << 8, GDK_BUTTON_RELEASE_MASK = 1 << 9,
       GDK_TOUCH_MASK = 1 << 22 };

static const char *source_name( int s )
{
    switch (s)
    {
    case 0: return "mouse";
    case 1: return "pen";
    case 2: return "eraser";
    case 5: return "touch";
    default: return "other";
    }
}

static gboolean on_event( GtkWidget *w, GdkEvent *e, void *data )
{
    GdkDevice *dev = gdk_event_get_source_device( e );
    int type = gdk_event_get_event_type( e );
    double p = -1, xt = -1, yt = -1;
    const char *kind = NULL;

    switch (type)
    {
    case GDK_MOTION_NOTIFY: kind = "motion"; break;
    case GDK_BUTTON_PRESS: kind = "press"; break;
    case GDK_BUTTON_RELEASE: kind = "release"; break;
    case GDK_TOUCH_BEGIN: kind = "touch-begin"; break;
    case GDK_TOUCH_UPDATE: kind = "touch-update"; break;
    case GDK_TOUCH_END: kind = "touch-end"; break;
    }
    if (!kind) return 0;
    if (!gdk_event_get_axis( e, GDK_AXIS_PRESSURE, &p )) p = -1;
    if (!gdk_event_get_axis( e, GDK_AXIS_XTILT, &xt )) xt = -1;
    if (!gdk_event_get_axis( e, GDK_AXIS_YTILT, &yt )) yt = -1;
    printf( "%s source=%s dev=%s pressure=%.3f xtilt=%.3f ytilt=%.3f\n", kind,
            dev ? source_name( gdk_device_get_source( dev ) ) : "none", dev ? gdk_device_get_name( dev ) : "none",
            p, xt, yt );
    fflush( stdout );
    return 1;
}

int main( int argc, char **argv )
{
    const char *disp = getenv( "DISPLAY" );
    GtkWidget *win;

    if (!disp || !strcmp( disp, ":0" )) { fprintf( stderr, "refusing DISPLAY %s\n", disp ? disp : "(none)" ); return 2; }
    gtk_init( &argc, &argv );
    win = gtk_window_new( 0 );
    gtk_window_set_title( win, "pengtk-probe" );
    gtk_window_set_default_size( win, argc > 1 ? atoi( argv[1] ) : 400, argc > 2 ? atoi( argv[2] ) : 300 );
    gtk_widget_add_events( win, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                GDK_TOUCH_MASK );
    g_signal_connect_data( win, "motion-notify-event", on_event, NULL, NULL, 0 );
    g_signal_connect_data( win, "button-press-event", on_event, NULL, NULL, 0 );
    g_signal_connect_data( win, "button-release-event", on_event, NULL, NULL, 0 );
    g_signal_connect_data( win, "touch-event", on_event, NULL, NULL, 0 );
    gtk_widget_show_all( win );
    printf( "ready\n" );
    fflush( stdout );
    gtk_main();
    return 0;
}
