/* A pen and a touch screen as an X program sees them (test/pen-gate.sh):
 * lists the X input devices, then opens a window and prints every XI2
 * event's device, kind and its pressure and tilt valuators -- what GTK, Qt
 * and Wine read. One line per event:
 *   motion|press|release dev=NAME pressure=P tiltx=X tilty=Y button=B
 *   touch-begin|touch-update|touch-end dev=NAME id=N x=X y=Y
 * Pressure and tilt are as the device reports them (Xwayland: pressure
 * 0..65535, tilt in degrees), -1 when the event has none.
 * Build: cc -o penxi2-probe penxi2-probe.c -lX11 -lXi */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dev
{
    int id;
    char name[128];
    int pressure, tiltx, tilty; /* valuator numbers, -1: none */
};

static struct dev devs[64];
static int ndevs;

static struct dev *find_dev( int id )
{
    for (int i = 0; i < ndevs; i++) if (devs[i].id == id) return &devs[i];
    return NULL;
}

static double valuator( const XIValuatorState *v, int n )
{
    const double *val = v->values;
    if (n < 0 || n >= v->mask_len * 8 || !XIMaskIsSet( v->mask, n )) return -1;
    for (int i = 0; i < n; i++) if (XIMaskIsSet( v->mask, i )) val++;
    return *val;
}

int main( int argc, char **argv )
{
    const char *disp = getenv( "DISPLAY" );
    Display *d;
    int op, ev, err, major = 2, minor = 2, n;
    XIDeviceInfo *info;
    unsigned char bits[XIMaskLen( XI_LASTEVENT )] = {0};
    XIEventMask mask = {XIAllDevices, sizeof(bits), bits};
    Window w;

    if (!disp || !strcmp( disp, ":0" )) { fprintf( stderr, "refusing DISPLAY %s\n", disp ? disp : "(none)" ); return 2; }
    if (!(d = XOpenDisplay( NULL ))) return 2;
    if (!XQueryExtension( d, "XInputExtension", &op, &ev, &err ) || XIQueryVersion( d, &major, &minor )) return 3;

    info = XIQueryDevice( d, XIAllDevices, &n );
    for (int i = 0; i < n && ndevs < 64; i++)
    {
        struct dev *dv = &devs[ndevs++];
        dv->id = info[i].deviceid;
        snprintf( dv->name, sizeof(dv->name), "%s", info[i].name );
        dv->pressure = dv->tiltx = dv->tilty = -1;
        for (int c = 0; c < info[i].num_classes; c++)
        {
            XIValuatorClassInfo *v = (XIValuatorClassInfo *)info[i].classes[c];
            char *label;
            if (v->type != XIValuatorClass || !v->label) continue;
            label = XGetAtomName( d, v->label );
            if (label && !strcmp( label, "Abs Pressure" )) dv->pressure = v->number;
            if (label && !strcmp( label, "Abs Tilt X" )) dv->tiltx = v->number;
            if (label && !strcmp( label, "Abs Tilt Y" )) dv->tilty = v->number;
            if (label) XFree( label );
        }
        printf( "device %d \"%s\" use=%d pressure-axis=%d tilt-axes=%d,%d\n", dv->id, dv->name,
                info[i].use, dv->pressure, dv->tiltx, dv->tilty );
    }
    XIFreeDeviceInfo( info );
    fflush( stdout );

    w = XCreateSimpleWindow( d, DefaultRootWindow( d ), 0, 0,
                             argc > 1 ? atoi( argv[1] ) : 400, argc > 2 ? atoi( argv[2] ) : 300, 0, 0, 0xffffff );
    XStoreName( d, w, "penxi2-probe" );
    XISetMask( bits, XI_Motion );
    XISetMask( bits, XI_ButtonPress );
    XISetMask( bits, XI_ButtonRelease );
    XISetMask( bits, XI_TouchBegin );
    XISetMask( bits, XI_TouchUpdate );
    XISetMask( bits, XI_TouchEnd );
    XISelectEvents( d, w, &mask, 1 );
    XMapWindow( d, w );
    XFlush( d );

    for (;;)
    {
        XEvent e;
        XGenericEventCookie *c = &e.xcookie;
        XNextEvent( d, &e );
        if (c->type != GenericEvent || c->extension != op || !XGetEventData( d, c )) continue;
        {
            XIDeviceEvent *de = c->data;
            /* the slave device the event came from: its axes */
            struct dev *dv = find_dev( de->sourceid );
            const char *kind = NULL;
            switch (de->evtype)
            {
            case XI_Motion: kind = "motion"; break;
            case XI_ButtonPress: kind = "press"; break;
            case XI_ButtonRelease: kind = "release"; break;
            case XI_TouchBegin: kind = "touch-begin"; break;
            case XI_TouchUpdate: kind = "touch-update"; break;
            case XI_TouchEnd: kind = "touch-end"; break;
            }
            /* each event comes once per device it is selected for (master
             * and slave): print only the slave's */
            if (kind && de->deviceid == de->sourceid)
            {
                if (de->evtype >= XI_TouchBegin && de->evtype <= XI_TouchEnd)
                    printf( "%s dev=%s id=%d x=%.0f y=%.0f\n", kind, dv ? dv->name : "?", de->detail,
                            de->event_x, de->event_y );
                else
                    printf( "%s dev=%s pressure=%.0f tiltx=%.0f tilty=%.0f button=%d x=%.0f y=%.0f\n", kind,
                            dv ? dv->name : "?", dv ? valuator( &de->valuators, dv->pressure ) : -1,
                            dv ? valuator( &de->valuators, dv->tiltx ) : -1,
                            dv ? valuator( &de->valuators, dv->tilty ) : -1,
                            de->evtype == XI_Motion ? 0 : de->detail, de->event_x, de->event_y );
                fflush( stdout );
            }
        }
        XFreeEventData( d, c );
    }
}
