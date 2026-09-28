/*
 * win32swiftui.dll: the app's own state on macOS (map: taskbar.progress).
 *
 * ITaskbarList3's progress belongs to a window (its taskbar button) on
 * Windows. A Mac app has one Dock icon: it shows the progress of the window
 * that set one last, and the one before when that one ends or goes away.
 */
#include "w2s_pe.h"

struct window_progress
{
    HWND hwnd;
    TBPFLAG state;
    ULONGLONG done, total;
    DWORD stamp;            /* the last change: the newest one shows */
};

#define MAX_PROGRESS 16
static struct window_progress progress[MAX_PROGRESS];
static DWORD last_stamp;
static SRWLOCK progress_lock = SRWLOCK_INIT;
static char shown[64];      /* what the Dock has, not to send it again */

static const char *state_name( TBPFLAG state )
{
    switch (state)
    {
    case TBPF_INDETERMINATE: return "indeterminate";
    case TBPF_ERROR: return "error";
    case TBPF_PAUSED: return "paused";
    default: return "normal";
    }
}

/* the Dock tile for the newest progress (progress_lock held) */
static void show_progress(void)
{
    struct w2s_app_state_params params;
    struct window_progress *top = NULL;
    const char *state = "none";
    int permille = 0, i;
    struct json j;
    char key[64];

    for (i = 0; i < MAX_PROGRESS; i++)
    {
        struct window_progress *p = &progress[i];
        if (!p->hwnd) continue;
        if (p->state == TBPF_NOPROGRESS || !IsWindow( p->hwnd ))
        {
            memset( p, 0, sizeof(*p) );
            continue;
        }
        if (!top || p->stamp > top->stamp) top = p;
    }
    if (top)
    {
        state = state_name( top->state );
        if (top->total) permille = (int)(top->done * 1000 / top->total);
    }
    /* apps report often (every byte copied): the Dock only redraws for a visible change */
    snprintf( key, sizeof(key), "%s/%d", state, permille );
    if (!strcmp( key, shown )) return;
    strcpy( shown, key );

    json_init( &j );
    json_obj_begin( &j );
    json_str_a( &j, "t", "progress" );
    json_str_a( &j, "state", state );
    json_num( &j, "value", permille / 1000.0 );
    json_obj_end( &j );
    params.json = W2S_PTR( j.buf );
    params.json_len = strlen( j.buf );
    params.pad = 0;
    w2s_call( unix_w2s_app_state, &params );
    json_free( &j );
}

/***********************************************************************
 *      W2STaskbarProgress  (win32swiftui.@)
 *
 * ITaskbarList3::SetProgressState (set_state) and SetProgressValue for hwnd,
 * as explorerframe gets them. A value turns "no progress" and
 * "indeterminate" into a normal bar, as on Windows.
 */
void WINAPI W2STaskbarProgress( HWND hwnd, BOOL set_state, TBPFLAG state, ULONGLONG done, ULONGLONG total )
{
    struct window_progress *p = NULL, *oldest = NULL;
    int i;

    if (!hwnd) return;
    AcquireSRWLockExclusive( &progress_lock );
    for (i = 0; i < MAX_PROGRESS && !p; i++) if (progress[i].hwnd == hwnd) p = &progress[i];
    if (!p && !(set_state && state == TBPF_NOPROGRESS))
    {
        for (i = 0; i < MAX_PROGRESS && !p; i++) if (!progress[i].hwnd) p = &progress[i];
        for (i = 0; i < MAX_PROGRESS && !p; i++)
            if (!oldest || progress[i].stamp < oldest->stamp) oldest = &progress[i];
        if (!p) p = oldest;
        memset( p, 0, sizeof(*p) );
        p->hwnd = hwnd;
    }
    if (p)
    {
        if (set_state) p->state = state;
        else
        {
            p->total = total;
            p->done = min( done, total );
            if (p->state == TBPF_NOPROGRESS || p->state == TBPF_INDETERMINATE) p->state = TBPF_NORMAL;
        }
        p->stamp = ++last_stamp;
    }
    show_progress();
    ReleaseSRWLockExclusive( &progress_lock );
}
