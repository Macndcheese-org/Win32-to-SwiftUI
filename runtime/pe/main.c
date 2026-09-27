/*
 * win32swiftui.dll: attaches translated controls and runs their subclass.
 *
 * user32 calls W2SWindowCreated for every window created while the native UI
 * is on. For a window the map translates, we:
 *   - subclass it (under any subclass the app adds later),
 *   - drop its non-client area, so the native view covers everything,
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

void w2s_common_snapshot( struct w2s_control *ctl, struct json *j )
{
    WCHAR text[1024];
    HFONT font = (HFONT)SendMessageW( ctl->hwnd, WM_GETFONT, 0, 0 );
    LOGFONTW lf;
    RECT rc;

    GetWindowTextW( ctl->hwnd, text, ARRAYSIZE(text) );
    json_str_a( j, "entry", ctl->kind->entry );
    json_str( j, "text", text );
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
        return;
    }
    params.handle = ctl->handle;
    params.json = snap;
    params.json_len = strlen( snap );
    TRACE( "  update %p -> unix\n", ctl->hwnd );
    w2s_call( unix_w2s_control_update, &params );
    TRACE( "  update %p <- unix\n", ctl->hwnd );
    if (ctl->last) HeapFree( GetProcessHeap(), 0, ctl->last );
    ctl->last = snap;
    TRACE( "push %p %s\n", ctl->hwnd, snap );
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
    if (!params.len) return;

    count = json_parse_events( buf, &events );
    TRACE( "events %p %s\n", ctl->hwnd, buf );
    ctl->applying++;
    for (i = 0; i < count && IsWindow( ctl->hwnd ); i++)
    {
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
     * (the app may have refused it, like a vetoed tab change) */
    if (IsWindow( ctl->hwnd )) w2s_push( ctl, FALSE );
}

static void detach( struct w2s_control *ctl )
{
    struct w2s_control_destroy_params params = { ctl->handle };

    if (ctl->handle) w2s_call( unix_w2s_control_destroy, &params );
    if (ctl->host.surface) w2s_release_host( ctl->hwnd, &ctl->host );
    RemovePropW( ctl->hwnd, prop_name );
    if (ctl->last) HeapFree( GetProcessHeap(), 0, ctl->last );
    HeapFree( GetProcessHeap(), 0, ctl );
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

    switch (msg)
    {
    case WM_NCCALCSIZE:
        /* no borders or scroll bars: the native control draws its own */
        return 0;
    case WM_NCPAINT:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        /* wine's drawing would show around the native control's rounded edges;
         * paint what the parent has behind us instead */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        if (hdc) DrawThemeParentBackground( hwnd, hdc, &ps.rcPaint );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_PRINTCLIENT:
        DrawThemeParentBackground( hwnd, (HDC)wparam, NULL );
        return 0;
    case WM_NCDESTROY:
    {
        WNDPROC orig = ctl->orig;
        detach( ctl );
        return CallWindowProcW( orig, hwnd, msg, wparam, lparam );
    }
    }

    ret = CallWindowProcW( ctl->orig, hwnd, msg, wparam, lparam );

    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
    {
        struct w2s_control_focus_params params = { ctl->handle, msg == WM_SETFOCUS };
        w2s_call( unix_w2s_control_focus, &params );
    }
    if (!ctl->applying && !ctl->snapshotting && is_state_message( ctl, msg )) w2s_push( ctl, FALSE );
    return ret;
}

/***********************************************************************
 *      W2SWindowCreated  (win32swiftui.@)
 */
void WINAPI W2SWindowCreated( HWND hwnd )
{
    const struct w2s_kind *kind;
    const struct w2s_map_entry *entry;
    struct w2s_control *ctl;
    struct w2s_control_create_params params;
    char *snap;

    if (!unix_ready || !(kind = w2s_select_kind( hwnd ))) return;
    if (!(entry = map_entry( kind->entry )))
    {
        TRACE( "%s is not in the map\n", kind->entry );
        return;
    }

    TRACE( "attach %p as %s\n", hwnd, kind->entry );
    ctl = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ctl) );
    ctl->hwnd = hwnd;
    ctl->kind = kind;
    ctl->state_in = entry->state_in;
    ctl->state_in_count = entry->state_in_count;
    SetPropW( hwnd, prop_name, ctl );
    ctl->orig = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)subclass_proc );
    TRACE( "  subclassed\n" );
    SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    TRACE( "  frame changed\n" );

    if (!w2s_get_host( hwnd, &ctl->host ))
    {
        TRACE( "no host for %p\n", hwnd );
        goto fail;
    }

    TRACE( "  host view %#llx window %#llx\n", (unsigned long long)ctl->host.view, (unsigned long long)ctl->host.window );
    snap = build_snapshot( ctl );
    TRACE( "  snapshot %s\n", snap );
    params.host_view = ctl->host.view;
    params.window = ctl->host.window;
    params.post_wake = ctl->host.post_wake;
    params.hwnd = (UINT_PTR)hwnd;
    params.entry = kind->entry;
    params.json = snap;
    params.json_len = strlen( snap );
    params.handle = 0;
    w2s_call( unix_w2s_control_create, &params );
    ctl->handle = params.handle;
    ctl->last = snap;
    if (!ctl->handle) goto fail;
    TRACE( "attached %p as %s: %s\n", hwnd, kind->entry, snap );
    InvalidateRect( hwnd, NULL, TRUE );
    return;

fail:
    SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)ctl->orig );
    if (ctl->host.surface) w2s_release_host( hwnd, &ctl->host );
    RemovePropW( hwnd, prop_name );
    if (ctl->last) HeapFree( GetProcessHeap(), 0, ctl->last );
    HeapFree( GetProcessHeap(), 0, ctl );
    SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
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
    return debug_call( hwnd, "{\"op\":\"query\"}" );
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
        struct w2s_init_params params = { W2S_PROTOCOL_VERSION, 0 };

        DisableThreadLibraryCalls( instance );
        if (GetEnvironmentVariableA( "W2S_DEBUG", value, sizeof(value) )) w2s_debug = atoi( value );
        if (__wine_init_unix_call())
        {
            TRACE( "no unix side\n" );
            return TRUE;
        }
        w2s_wake_message = RegisterWindowMessageW( L"Win32ToSwiftUI.Wake" );
        unix_ready = !w2s_call( unix_w2s_init, &params ) && params.ok;
        TRACE( "ready %d, wake message %#x\n", unix_ready, w2s_wake_message );
    }
    return TRUE;
}
