/*
 * win32swiftui.dll: attaches translated controls and runs their subclass.
 *
 * user32 calls W2SWindowCreated for every window created while the native UI
 * is on. For a window the map translates, we:
 *   - subclass it (under any subclass the app adds later),
 *   - drop its non-client area, so the native view covers everything,
 *   - give it an empty window region, so nothing wine draws for it shows
 *     (button and combo box code paints outside WM_PAINT too) and the parent
 *     paints what is behind it,
 *   - ask winemac for a host view over it (W2S_ESCAPE_GET_HOST),
 *   - create the SwiftUI view in win32swiftui.so from a JSON snapshot.
 * The Win32 control keeps running and stays authoritative. After each
 * state-changing message (the entry's state_in list from the map) a fresh
 * snapshot is sent; native events come back as w2s_wake_message and are
 * applied as the Win32 actions they stand for.
 */
#include "w2s_pe.h"
#include "w2s_map_tables.h"

int w2s_debug;
UINT w2s_wake_message;
UINT w2s_os_major = 12, w2s_os_minor;
static BOOL unix_ready;
static const WCHAR prop_name[] = L"Win32ToSwiftUI.Control";

/* messages that can change what any control shows */
static const UINT base_state_in[] =
{
    WM_SETTEXT, WM_ENABLE, WM_SETFONT, WM_STYLECHANGED, WM_SIZE, WM_KEYDOWN, WM_KEYUP, WM_CHAR,
    WM_LBUTTONUP, WM_UPDATEUISTATE,
};

static const struct w2s_map_entry *map_entry( const char *id )
{
    unsigned int i;
    for (i = 0; i < ARRAYSIZE(w2s_map_entries); i++)
        if (!strcmp( w2s_map_entries[i].id, id )) return &w2s_map_entries[i];
    return NULL;
}

static BOOL is_state_message( struct w2s_control *ctl, UINT msg )
{
    unsigned int i;
    for (i = 0; i < ARRAYSIZE(base_state_in); i++) if (base_state_in[i] == msg) return TRUE;
    for (i = 0; i < ctl->state_in_count; i++) if (ctl->state_in[i] == msg) return TRUE;
    return FALSE;
}

BOOL w2s_get_host( HWND hwnd, struct w2s_host *host )
{
    struct w2s_host_request request = { w2s_wake_message, W2S_MACDRV_VERSION };
    /* not GetDC: CS_PARENTDC controls would hand out their parent's DC, and
     * winemac finds the window from the DC */
    HDC hdc = GetDCEx( hwnd, 0, DCX_CACHE );
    int ret;

    if (!hdc) return FALSE;
    memset( host, 0, sizeof(*host) );
    ret = ExtEscape( hdc, W2S_ESCAPE_GET_HOST, sizeof(request), (const char *)&request,
                     sizeof(*host), (char *)host );
    ReleaseDC( hwnd, hdc );
    return ret > 0 && host->view;
}

void w2s_release_host( HWND hwnd, const struct w2s_host *host )
{
    HDC hdc = GetDCEx( hwnd, 0, DCX_CACHE );
    if (!hdc) return;
    ExtEscape( hdc, W2S_ESCAPE_RELEASE_HOST, sizeof(*host), (const char *)host, 0, NULL );
    ReleaseDC( hwnd, hdc );
}

/* An empty window region clips everything wine draws for the control, inside
 * WM_PAINT or not, and takes the control out of its parent's clipping, so the
 * parent paints its own background behind the native view. SetWindowRgn owns
 * the region and sends SWP_FRAMECHANGED, so the non-client area goes (or comes
 * back) through our WM_NCCALCSIZE at the same time. */
static HRGN wine_region( struct w2s_control *ctl )
{
    HRGN rgn = ctl->kind && ctl->kind->region ? ctl->kind->region( ctl ) : NULL;
    return rgn ? rgn : CreateRectRgn( 0, 0, 0, 0 );
}

static void clip_wine_drawing( struct w2s_control *ctl, BOOL clip )
{
    HWND hwnd = ctl->hwnd, parent = GetParent( hwnd );
    RECT rc;

    SetWindowRgn( hwnd, clip ? wine_region( ctl ) : NULL, FALSE );
    if (!parent || !IsWindowVisible( hwnd )) return;
    GetWindowRect( hwnd, &rc );
    MapWindowPoints( NULL, parent, (POINT *)&rc, 2 );
    RedrawWindow( parent, &rc, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
}

void w2s_common_snapshot( struct w2s_control *ctl, struct json *j )
{
    WCHAR text[1024];
    HFONT font = (HFONT)SendMessageW( ctl->hwnd, WM_GETFONT, 0, 0 );
    LOGFONTW lf;
    RECT rc;

    json_str_a( j, "entry", ctl->kind->entry );
    if (!(ctl->kind->flags & W2S_OWN_TEXT))
    {
        GetWindowTextW( ctl->hwnd, text, ARRAYSIZE(text) );
        json_str( j, "text", text );
    }
    json_bool( j, "enabled", IsWindowEnabled( ctl->hwnd ) );
    if (!font) font = GetStockObject( DEFAULT_GUI_FONT );
    if (GetObjectW( font, sizeof(lf), &lf )) json_int( j, "fontPx", abs( lf.lfHeight ) ? abs( lf.lfHeight ) : 11 );
    GetClientRect( ctl->hwnd, &rc );
    json_int( j, "widthPx", rc.right );
    json_int( j, "heightPx", rc.bottom );
}

static char *build_snapshot( struct w2s_control *ctl )
{
    struct json j;
    char *copy;

    ctl->snapshotting++;
    json_init( &j );
    json_obj_begin( &j );
    w2s_common_snapshot( ctl, &j );
    json_int( &j, "ack", ctl->ack );
    ctl->kind->snapshot( ctl, &j );
    json_obj_end( &j );
    ctl->snapshotting--;
    copy = HeapAlloc( GetProcessHeap(), 0, j.len + 1 );
    memcpy( copy, j.buf, j.len + 1 );
    json_free( &j );
    return copy;
}

void w2s_push( struct w2s_control *ctl, BOOL force )
{
    struct w2s_control_update_params params;
    char *snap;

    if (!ctl->handle) return;
    snap = build_snapshot( ctl );
    if (!force && ctl->last && !strcmp( ctl->last, snap ))
    {
        HeapFree( GetProcessHeap(), 0, snap );
    }
    else
    {
        if (ctl->kind->region)
        {
            /* the part wine draws may have moved with the change */
            HRGN now = CreateRectRgn( 0, 0, 0, 0 ), want = wine_region( ctl );
            if (GetWindowRgn( ctl->hwnd, now ) == ERROR || !EqualRgn( now, want ))
            {
                SetWindowRgn( ctl->hwnd, want, TRUE );
                want = NULL;
            }
            DeleteObject( now );
            if (want) DeleteObject( want );
        }
        params.handle = ctl->handle;
        params.json = snap;
        params.json_len = strlen( snap );
        w2s_call( unix_w2s_control_update, &params );
        if (ctl->last) HeapFree( GetProcessHeap(), 0, ctl->last );
        ctl->last = snap;
        TRACE( "push %p %s\n", ctl->hwnd, snap );
    }
    if (ctl->follower) w2s_push( ctl->follower, force );
}

/***********************************************************************
 *      w2s_native_state
 *
 * What the native view published for the entry's answers (a JSON object),
 * fetched again only when it changed. Never waits for the main thread.
 */
const char *w2s_native_state( struct w2s_control *ctl, BOOL *changed )
{
    struct w2s_control_state_params params;

    if (changed) *changed = FALSE;
    if (!ctl->handle) return NULL;
    params.handle = ctl->handle;
    params.version = ctl->native_version;
    params.buffer = ctl->native;
    params.size = ctl->native ? ctl->native_size : 0;
    params.len = 0;
    w2s_call( unix_w2s_control_state, &params );
    if (params.len > params.size)
    {
        char *buf = ctl->native ? HeapReAlloc( GetProcessHeap(), 0, ctl->native, params.len + 256 )
                                : HeapAlloc( GetProcessHeap(), 0, params.len + 256 );
        if (!buf) return ctl->native;
        ctl->native = buf;
        ctl->native_size = params.len + 256;
        params.version = ctl->native_version;
        params.buffer = ctl->native;
        params.size = ctl->native_size;
        params.len = 0;
        w2s_call( unix_w2s_control_state, &params );
        if (params.len > params.size) return NULL;  /* grew again meanwhile; next time */
    }
    if (params.len)
    {
        ctl->native_version = params.version;
        if (changed) *changed = TRUE;
    }
    return ctl->native;
}

void w2s_notify_parent_command( HWND hwnd, UINT code )
{
    SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), code ), (LPARAM)hwnd );
}

LRESULT w2s_notify_parent( HWND hwnd, UINT code, NMHDR *hdr )
{
    hdr->hwndFrom = hwnd;
    hdr->idFrom = GetDlgCtrlID( hwnd );
    hdr->code = code;
    return SendMessageW( GetParent( hwnd ), WM_NOTIFY, hdr->idFrom, (LPARAM)hdr );
}

static void apply_events( struct w2s_control *ctl )
{
    struct w2s_pop_events_params params;
    struct w2s_event *events;
    char small[1024];
    char *buf = small;
    int i, count;

    if (!ctl->handle) return;
    params.handle = ctl->handle;
    params.buffer = small;
    params.size = sizeof(small);
    params.len = 0;
    w2s_call( unix_w2s_pop_events, &params );
    if (params.len > params.size)
    {
        buf = HeapAlloc( GetProcessHeap(), 0, params.len );
        params.buffer = buf;
        params.size = params.len;
        w2s_call( unix_w2s_pop_events, &params );
    }
    if (!params.len || params.len > params.size)
    {
        if (buf != small) HeapFree( GetProcessHeap(), 0, buf );
        return;
    }

    count = json_parse_events( buf, &events );
    TRACE( "events %p %s\n", ctl->hwnd, buf );
    ctl->applying++;
    for (i = 0; i < count; i++)
    {
        /* taken, even if the control is gone or the app refuses it */
        if (events[i].seq > ctl->ack) ctl->ack = events[i].seq;
        if (!IsWindow( ctl->hwnd ) || !ctl->active) continue;
        if (!strcmp( events[i].type, "focus" ))
        {
            if (GetFocus() != ctl->hwnd) SetFocus( ctl->hwnd );
        }
        else ctl->kind->apply( ctl, &events[i] );
    }
    ctl->applying--;
    json_free_events( events, count );
    if (buf != small) HeapFree( GetProcessHeap(), 0, buf );
    /* the native view already shows what the user did; bring the rest back in line
     * (the app may have refused it, like a vetoed tab change). The ack in it
     * tells the native view this snapshot has seen all of its events. */
    if (IsWindow( ctl->hwnd )) w2s_push( ctl, FALSE );
}

static const char *family( const struct w2s_kind *kind )
{
    return kind->family ? kind->family : kind->entry;
}

/* the native view goes; the control stays subclassed (a style change may
 * bring it back) and wine draws it again */
static void deactivate( struct w2s_control *ctl, BOOL destroying )
{
    struct w2s_control_destroy_params params = { ctl->handle };

    if (ctl->handle) w2s_call( unix_w2s_control_destroy, &params );
    ctl->handle = 0;
    if (ctl->host.surface) w2s_release_host( ctl->hwnd, &ctl->host );
    memset( &ctl->host, 0, sizeof(ctl->host) );
    if (ctl->last) HeapFree( GetProcessHeap(), 0, ctl->last );
    ctl->last = NULL;
    if (ctl->data)
    {
        if (ctl->kind && ctl->kind->release) ctl->kind->release( ctl );
        else HeapFree( GetProcessHeap(), 0, ctl->data );
    }
    ctl->data = NULL;
    if (ctl->native) HeapFree( GetProcessHeap(), 0, ctl->native );
    ctl->native = NULL;
    ctl->native_version = 0;
    if (ctl->active && !destroying) clip_wine_drawing( ctl, FALSE );
    ctl->active = FALSE;
}

static BOOL set_kind( struct w2s_control *ctl, const struct w2s_kind *kind )
{
    const struct w2s_map_entry *entry = map_entry( kind->entry );

    if (!entry)
    {
        TRACE( "%s is not in the map\n", kind->entry );
        return FALSE;
    }
    ctl->kind = kind;
    ctl->state_in = entry->state_in;
    ctl->state_in_count = entry->state_in_count;
    ctl->answers = entry->answers;
    ctl->answers_count = entry->answers_count;
    return TRUE;
}

/* the native view comes: clip wine, get a host view, create the SwiftUI view */
static BOOL activate( struct w2s_control *ctl, const struct w2s_kind *kind )
{
    struct w2s_control_create_params params;
    char *snap;

    if (!set_kind( ctl, kind )) return FALSE;
    ctl->active = TRUE;
    clip_wine_drawing( ctl, TRUE );

    if (!w2s_get_host( ctl->hwnd, &ctl->host ))
    {
        TRACE( "no host for %p\n", ctl->hwnd );
        deactivate( ctl, FALSE );
        return FALSE;
    }
    snap = build_snapshot( ctl );
    params.host_view = ctl->host.view;
    params.window = ctl->host.window;
    params.post_wake = ctl->host.post_wake;
    params.hwnd = (UINT_PTR)ctl->hwnd;
    params.entry = kind->entry;
    params.json = snap;
    params.json_len = strlen( snap );
    params.handle = 0;
    w2s_call( unix_w2s_control_create, &params );
    ctl->handle = params.handle;
    ctl->last = snap;
    if (!ctl->handle)
    {
        deactivate( ctl, FALSE );
        return FALSE;
    }
    TRACE( "attached %p as %s: %s\n", ctl->hwnd, kind->entry, snap );
    return TRUE;
}

/* a style change may make the control another entry, or one we don't translate
 * (a list view switched to LVS_OWNERDATA, LVS_EX_CHECKBOXES added) */
static void reselect_kind( struct w2s_control *ctl )
{
    const struct w2s_kind *kind = w2s_select_kind( ctl->hwnd );

    if (kind == ctl->kind && ctl->active) return;
    if (kind && ctl->active && !strcmp( family( kind ), family( ctl->kind ) ))
    {
        if (set_kind( ctl, kind )) w2s_push( ctl, FALSE );
        return;
    }
    TRACE( "%p: %s -> %s\n", ctl->hwnd, ctl->active ? ctl->kind->entry : "(wine)", kind ? kind->entry : "(wine)" );
    deactivate( ctl, FALSE );
    if (kind) activate( ctl, kind );
}

static BOOL is_reselect_message( UINT msg )
{
    return msg == WM_STYLECHANGED || msg == BM_SETSTYLE || msg == LVM_SETEXTENDEDLISTVIEWSTYLE || msg == LVM_SETVIEW;
}

static BOOL is_answer( struct w2s_control *ctl, UINT msg )
{
    unsigned int i;
    for (i = 0; i < ctl->answers_count; i++) if (ctl->answers[i] == msg) return TRUE;
    return FALSE;
}

struct w2s_control *w2s_control_from_hwnd( HWND hwnd )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    return ctl && ctl->active ? ctl : NULL;
}

static LRESULT CALLBACK subclass_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    LRESULT ret;

    if (!ctl) return DefWindowProcW( hwnd, msg, wparam, lparam );
    if (w2s_debug > 1) TRACE( "  msg %p %#x\n", hwnd, msg );

    if (msg == w2s_wake_message && w2s_wake_message)
    {
        apply_events( ctl );
        return 0;
    }

    if (msg == WM_NCDESTROY)
    {
        WNDPROC orig = ctl->orig;
        deactivate( ctl, TRUE );
        RemovePropW( hwnd, prop_name );
        HeapFree( GetProcessHeap(), 0, ctl );
        return CallWindowProcW( orig, hwnd, msg, wparam, lparam );
    }
    if (!ctl->active)
    {
        ret = CallWindowProcW( ctl->orig, hwnd, msg, wparam, lparam );
        if (is_reselect_message( msg ) && unix_ready) reselect_kind( ctl );
        return ret;
    }

    /* The empty window region already hides whatever wine paints, in and out
     * of WM_PAINT, and lets the parent paint behind the control, so painting
     * needs nothing from us. Two things still do: */
    switch (msg)
    {
    case WM_NCCALCSIZE:
        /* the client area, which the host view follows, is the whole window:
         * the native control draws its own border and scrolls itself */
        return 0;
    case WM_PRINTCLIENT:
        /* someone draws us into their DC (a child's DrawThemeParentBackground,
         * a tab page inside a tab control): give them what is behind us, not
         * wine's look of the control */
        DrawThemeParentBackground( hwnd, (HDC)wparam, NULL );
        return 0;
    }

    /* queries the map says the native view answers (the caret and line layout
     * of a multi-line edit); building a snapshot asks the Win32 control */
    if (ctl->kind->answer && !ctl->snapshotting && is_answer( ctl, msg ) &&
        ctl->kind->answer( ctl, msg, wparam, lparam, &ret ))
    {
        ctl->answered++;
        return ret;
    }

    ret = CallWindowProcW( ctl->orig, hwnd, msg, wparam, lparam );

    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
    {
        struct w2s_control_focus_params params = { ctl->handle, msg == WM_SETFOCUS };
        w2s_call( unix_w2s_control_focus, &params );
    }
    if (ctl->kind->observe) ctl->kind->observe( ctl, msg, wparam, lparam );
    if (is_reselect_message( msg ) && !ctl->applying) reselect_kind( ctl );
    if (ctl->active && !ctl->applying && !ctl->snapshotting && is_state_message( ctl, msg )) w2s_push( ctl, FALSE );
    return ret;
}

/***********************************************************************
 *      w2s_attach
 *
 * Subclasses a window and puts the native view of kind over it.
 */
BOOL w2s_attach( HWND hwnd, const struct w2s_kind *kind )
{
    struct w2s_control *ctl;

    if (!unix_ready || GetPropW( hwnd, prop_name ) || !map_entry( kind->entry )) return FALSE;
    TRACE( "attach %p as %s\n", hwnd, kind->entry );
    ctl = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ctl) );
    ctl->hwnd = hwnd;
    SetPropW( hwnd, prop_name, ctl );
    ctl->orig = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)subclass_proc );
    if (activate( ctl, kind )) return TRUE;

    SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)ctl->orig );
    RemovePropW( hwnd, prop_name );
    HeapFree( GetProcessHeap(), 0, ctl );
    return FALSE;
}

/***********************************************************************
 *      W2SWindowCreated  (win32swiftui.@)
 */
void WINAPI W2SWindowCreated( HWND hwnd )
{
    const struct w2s_kind *kind;

    if (!unix_ready || !(kind = w2s_select_kind( hwnd ))) return;
    w2s_attach( hwnd, kind );
}

/***********************************************************************
 *      W2SDebugQuery / W2SDebugInject  (win32swiftui.@)
 *
 * For tests/gallery: read the native view's model, or play a native event.
 * The result is HeapAlloc'd UTF-8 JSON; free it with W2SDebugFree.
 */
static char *debug_call( HWND hwnd, const char *json )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    struct w2s_debug_params params;
    char *buf;

    /* no window: the open alert or panel */
    if (hwnd && (!ctl || !ctl->handle)) return NULL;
    params.handle = hwnd ? ctl->handle : 0;
    params.json = json;
    params.json_len = strlen( json );
    params.size = 65536;
    params.buffer = buf = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, params.size );
    params.len = 0;
    w2s_call( unix_w2s_debug, &params );
    return buf;
}

char * WINAPI W2SDebugQuery( HWND hwnd )
{
    char *ret = debug_call( hwnd, "{\"op\":\"query\"}" );
    struct w2s_control *ctl = hwnd ? GetPropW( hwnd, prop_name ) : NULL;
    size_t len = ret ? strlen( ret ) : 0;

    /* what only this side knows */
    if (ctl && len > 1 && ret[len - 1] == '}' && len + 32 < 65536)
        snprintf( ret + len - 1, 34, ",\"peAnswers\":%u}", ctl->answered );
    return ret;
}

char * WINAPI W2SDebugInject( HWND hwnd, const char *event_json )
{
    char *json, *ret;
    size_t n = strlen( event_json ) + 64;

    json = HeapAlloc( GetProcessHeap(), 0, n );
    snprintf( json, n, "{\"op\":\"inject\",\"event\":%s}", event_json );
    ret = debug_call( hwnd, json );
    HeapFree( GetProcessHeap(), 0, json );
    return ret;
}

void WINAPI W2SDebugFree( char *p )
{
    if (p) HeapFree( GetProcessHeap(), 0, p );
}

BOOL WINAPI W2SIsTranslated( HWND hwnd )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    return ctl && ctl->handle;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char value[8];
        struct w2s_init_params params = { W2S_PROTOCOL_VERSION };

        DisableThreadLibraryCalls( instance );
        if (GetEnvironmentVariableA( "W2S_DEBUG", value, sizeof(value) )) w2s_debug = atoi( value );
        if (__wine_init_unix_call())
        {
            TRACE( "no unix side\n" );
            return TRUE;
        }
        w2s_wake_message = RegisterWindowMessageW( L"Win32ToSwiftUI.Wake" );
        unix_ready = !w2s_call( unix_w2s_init, &params ) && params.ok;
        if (unix_ready)
        {
            w2s_os_major = params.os_major;
            w2s_os_minor = params.os_minor;
        }
        TRACE( "ready %d on macOS %u.%u, wake message %#x\n", unix_ready, w2s_os_major, w2s_os_minor, w2s_wake_message );
    }
    return TRUE;
}
