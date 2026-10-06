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
static LONG deferred_count;                         /* controls waiting to show (w2s_attach) */
static DWORD show_hook_tls = TLS_OUT_OF_INDEXES;    /* each GUI thread's show_hook */

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
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );
    RECT rc;

    /* Button, Static, Edit and ComboBox are CS_PARENTDC classes: their DCs clip
     * with the parent's region, which ignores the control's own window region,
     * so wine kept drawing them under the native view. win32u drops parent
     * clipping for WS_CLIPSIBLINGS windows (dce.c), and then the empty region
     * holds for every DC the control gets, in WM_PAINT or not. */
    /* our own style change must not look like the app's (reselect_kind) */
    ctl->reselecting++;
    if (clip && !(style & WS_CLIPSIBLINGS))
    {
        ctl->added_clipsiblings = TRUE;
        SetWindowLongW( hwnd, GWL_STYLE, style | WS_CLIPSIBLINGS );
    }
    else if (!clip && ctl->added_clipsiblings)
    {
        ctl->added_clipsiblings = FALSE;
        SetWindowLongW( hwnd, GWL_STYLE, style & ~WS_CLIPSIBLINGS );
    }
    ctl->reselecting--;
    SetWindowRgn( hwnd, clip ? wine_region( ctl ) : NULL, FALSE );
    if (!parent || !IsWindowVisible( hwnd )) return;
    GetWindowRect( hwnd, &rc );
    MapWindowPoints( NULL, parent, (POINT *)&rc, 2 );
    RedrawWindow( parent, &rc, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
}

void w2s_common_snapshot( struct w2s_control *ctl, struct json *j )
{
    WCHAR text[1024], *help;
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
    if (GetObjectW( font, sizeof(lf), &lf ))
    {
        json_int( j, "fontPx", abs( lf.lfHeight ) ? abs( lf.lfHeight ) : 11 );
        json_bool( j, "bold", lf.lfWeight >= FW_BOLD );
    }
    GetClientRect( ctl->hwnd, &rc );
    json_int( j, "widthPx", rc.right );
    json_int( j, "heightPx", rc.bottom );
    /* light or dark as what is behind it (look.c) */
    if (ctl->backdrop_stale)
    {
        ctl->backdrop_stale = FALSE;
        ctl->backdrop = w2s_backdrop( ctl->hwnd );
    }
    if (ctl->backdrop != CLR_INVALID) json_int( j, "backdrop", ctl->backdrop );
    if ((help = w2s_tool_text( ctl->hwnd )))
    {
        json_str( j, "help", help );
        HeapFree( GetProcessHeap(), 0, help );
    }
}

const UINT *w2s_map_state_in( const char *entry, unsigned int *count )
{
    const struct w2s_map_entry *e = map_entry( entry );
    *count = e ? e->state_in_count : 0;
    return e ? e->state_in : NULL;
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
        params.json = W2S_PTR( snap );
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
    params.buffer = W2S_PTR( ctl->native );
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
        params.buffer = W2S_PTR( ctl->native );
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
    params.buffer = W2S_PTR( small );
    params.size = sizeof(small);
    params.len = 0;
    w2s_call( unix_w2s_pop_events, &params );
    if (params.len > params.size)
    {
        buf = HeapAlloc( GetProcessHeap(), 0, params.len );
        params.buffer = W2S_PTR( buf );
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
    ctl->backdrop_stale = TRUE;
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
    params.entry = W2S_PTR( kind->entry );
    params.json = W2S_PTR( snap );
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
    /* it had the focus before its view came (made while hidden, focused as it showed) */
    {
        HWND focus = GetFocus();
        if (focus && (focus == ctl->hwnd || IsChild( ctl->hwnd, focus )))
        {
            struct w2s_control_focus_params focus_params = { ctl->handle, TRUE };
            w2s_call( unix_w2s_control_focus, &focus_params );
        }
    }
    /* the native side has resolved wine's colours since we last wrote them:
     * write them once the control's creation is over */
    if (w2s_look_pending()) PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_LOOK, 0 );
    return TRUE;
}

/* a style change may make the control another entry, or one we don't translate
 * (a list view switched to LVS_OWNERDATA, LVS_EX_CHECKBOXES added) */
static void reselect_kind( struct w2s_control *ctl )
{
    const struct w2s_kind *kind;

    /* switching sends style changes of its own; they must not switch again
     * (that activated twice: a leaked native view, and a second one that
     * never got the list view's icons) */
    if (ctl->reselecting) return;
    kind = w2s_select_kind( ctl->hwnd );
    if (kind == ctl->kind && ctl->active) return;
    if (kind && ctl->active && !strcmp( family( kind ), family( ctl->kind ) ))
    {
        if (set_kind( ctl, kind )) w2s_push( ctl, FALSE );
        return;
    }
    TRACE( "%p: %s -> %s\n", ctl->hwnd, ctl->active ? ctl->kind->entry : "(wine)", kind ? kind->entry : "(wine)" );
    ctl->reselecting++;
    deactivate( ctl, FALSE );
    if (kind) activate( ctl, kind );
    ctl->reselecting--;
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

/* a control whose native view isn't wanted any more (a tab whose only page is the window's sidebar) */
void w2s_retire_control( HWND hwnd )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );

    if (!ctl || !ctl->active) return;
    ctl->reselecting++;
    deactivate( ctl, FALSE );
    ctl->reselecting--;
}

struct w2s_control *w2s_control_from_hwnd( HWND hwnd )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    return ctl && ctl->active ? ctl : NULL;
}

/***********************************************************************
 *      w2s_control_of
 *
 * The translated control a window is (its native view up), or NULL.
 */
struct w2s_control *w2s_control_of( HWND hwnd )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    return ctl && ctl->active && ctl->handle ? ctl : NULL;
}

static LRESULT CALLBACK subclass_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    LRESULT ret;

    if (!ctl) return DefWindowProcW( hwnd, msg, wparam, lparam );
    if (w2s_debug > 1) TRACE( "  msg %p %#x\n", hwnd, msg );

    if (msg == w2s_wake_message && w2s_wake_message)
    {
        if (wparam == W2S_WAKE_REFRESH)
        {
            ctl->push_posted = FALSE;
            w2s_push( ctl, FALSE );
        }
        else if (wparam == W2S_WAKE_LOOK) w2s_sync_look();
        else apply_events( ctl );
        return 0;
    }

    if (msg == WM_NCDESTROY)
    {
        WNDPROC orig = ctl->orig;
        if (ctl->deferred) InterlockedDecrement( &deferred_count );
        deactivate( ctl, TRUE );
        RemovePropW( hwnd, prop_name );
        HeapFree( GetProcessHeap(), 0, ctl );
        return CallWindowProcW( orig, hwnd, msg, wparam, lparam );
    }
    if (!ctl->active)
    {
        ret = CallWindowProcW( ctl->orig, hwnd, msg, wparam, lparam );
        if (ctl->deferred) return ret;      /* until it shows (show_hook) */
        if (is_reselect_message( msg ) && unix_ready) reselect_kind( ctl );
        return ret;
    }

    /* The empty window region already hides whatever wine paints, in and out
     * of WM_PAINT, and lets the parent paint behind the control, so painting
     * needs nothing from us. Two things still do: */
    switch (ctl->kind->flags & W2S_KEEP_FRAME ? 0 : msg)
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

    /* focus going to one of the control's own children (a combo box's edit
     * part) stays within the native view */
    if (msg == WM_SETFOCUS || (msg == WM_KILLFOCUS && !IsChild( hwnd, (HWND)wparam )))
    {
        struct w2s_control_focus_params params = { ctl->handle, msg == WM_SETFOCUS };
        w2s_call( unix_w2s_control_focus, &params );
    }
    /* a control shown or hidden on a settings form's page: the form lays out again */
    if (msg == WM_WINDOWPOSCHANGED && (((WINDOWPOS *)lparam)->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)))
        w2s_form_child_changed( hwnd );
    if (ctl->kind->observe) ctl->kind->observe( ctl, msg, wparam, lparam );
    if (is_reselect_message( msg ) && !ctl->applying) reselect_kind( ctl );
    /* one snapshot for a burst of changes, once the app is back in its message loop:
     * a combo box filled item by item (Notepad++'s Preferences) sent one per item,
     * each with every item so far */
    if (ctl->active && !ctl->applying && !ctl->snapshotting && is_state_message( ctl, msg ) && !ctl->push_posted)
    {
        ctl->push_posted = TRUE;
        PostMessageW( hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    }
    return ret;
}

/* Controls made where nothing shows them yet: a dialog's controls before it shows,
 * and a settings window's pages made up front (Notepad++'s Preferences has 25, all
 * but one hidden: 475 controls, 40 seconds to open). Their native view comes when
 * they show; meanwhile they are wine's, which draws nothing for them. */
static void activate_deferred( struct w2s_control *ctl )
{
    const struct w2s_kind *kind;

    if (!ctl->deferred || !IsWindowVisible( ctl->hwnd )) return;
    ctl->deferred = NULL;
    InterlockedDecrement( &deferred_count );
    /* what it is now: its styles may have changed meanwhile (LVS_EX_CHECKBOXES) */
    if ((kind = w2s_select_kind( ctl->hwnd )) && !w2s_is_drawing_surface( ctl->hwnd, kind )) activate( ctl, kind );
}

static BOOL CALLBACK activate_shown_child( HWND hwnd, LPARAM lparam )
{
    struct w2s_control *ctl = GetPropW( hwnd, prop_name );
    if (ctl && ctl->deferred) activate_deferred( ctl );
    return deferred_count > 0;
}

/* a window just shown: the waiting controls in it that now show */
static LRESULT CALLBACK show_hook( int code, WPARAM wparam, LPARAM lparam )
{
    const CWPRETSTRUCT *cwp = (const CWPRETSTRUCT *)lparam;

    if (code == HC_ACTION && cwp->message == WM_WINDOWPOSCHANGED)
    {
        UINT flags = ((const WINDOWPOS *)cwp->lParam)->flags;

        if ((flags & SWP_SHOWWINDOW) && deferred_count > 0)
        {
            activate_shown_child( cwp->hwnd, 0 );
            EnumChildWindows( cwp->hwnd, activate_shown_child, 0 );
        }
        /* a settings window's page shown or hidden: its form lays out again */
        if (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) w2s_form_child_changed( cwp->hwnd );
    }
    return CallNextHookEx( NULL, code, wparam, lparam );
}

/* what changes the window around it (a toolbar in the frame, a sidebar, a settings
 * form, scroll bars) comes at once: the window would change size as it shows */
static BOOL can_defer( HWND hwnd, const struct w2s_kind *kind )
{
    static const char *const at_once[] = { "toolbar", "rebar", "treeview", "tab", "scrollbar" };
    unsigned int i;

    if (IsWindowVisible( hwnd ) || show_hook_tls == TLS_OUT_OF_INDEXES) return FALSE;
    for (i = 0; i < ARRAYSIZE(at_once); i++)
        if (!strncmp( kind->entry, at_once[i], strlen( at_once[i] ) )) return FALSE;
    if (!TlsGetValue( show_hook_tls ))
    {
        HHOOK hook = SetWindowsHookExW( WH_CALLWNDPROCRET, show_hook, NULL, GetCurrentThreadId() );
        if (!hook) return FALSE;
        TlsSetValue( show_hook_tls, hook );
    }
    return TRUE;
}

/***********************************************************************
 *      w2s_attach
 *
 * Subclasses a window and puts the native view of kind over it (once it shows).
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
    if (can_defer( hwnd, kind ))
    {
        ctl->deferred = kind;
        InterlockedIncrement( &deferred_count );
        return TRUE;
    }
    if (activate( ctl, kind )) return TRUE;

    SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)ctl->orig );
    RemovePropW( hwnd, prop_name );
    HeapFree( GetProcessHeap(), 0, ctl );
    return FALSE;
}

/* A window whose creation just ended turns out to be a combo box of a class we
 * don't translate (Delphi's TComboBox, a superclass of comctl32's): it made its
 * edit and list in its WM_CREATE, before it could say they are its parts, so they
 * may have come out as controls of their own. They go back to it. */
static void release_combo_parts( HWND hwnd )
{
    COMBOBOXINFO cbi = { sizeof(cbi) };
    struct w2s_control *ctl;
    HWND parts[2];
    int i;

    if (!GetWindow( hwnd, GW_CHILD ) || !GetComboBoxInfo( hwnd, &cbi )) return;
    parts[0] = cbi.hwndItem;
    parts[1] = cbi.hwndList;
    for (i = 0; i < 2; i++)
    {
        if (!parts[i] || !(ctl = GetPropW( parts[i], prop_name ))) continue;
        if (ctl->deferred)
        {
            ctl->deferred = NULL;
            InterlockedDecrement( &deferred_count );
            continue;
        }
        if (!ctl->active) continue;
        TRACE( "%p is combo box %p's part\n", parts[i], hwnd );
        ctl->reselecting++;
        deactivate( ctl, FALSE );
        ctl->reselecting--;
    }
}

/* A column header of a program's own (winefile's) stays wine's: its theme paints it light whatever
 * the system colours are, with the text in the system's, which in a dark appearance is light too.
 * Without the theme it is drawn from the system colours. */
static void classic_header_in_dark( HWND hwnd )
{
    DWORD face = GetSysColor( COLOR_BTNFACE );
    HRESULT (WINAPI *set_theme)( HWND, LPCWSTR, LPCWSTR );
    HMODULE uxtheme;

    if ((GetRValue( face ) * 30 + GetGValue( face ) * 59 + GetBValue( face ) * 11) / 100 >= 128) return;
    if (!(uxtheme = LoadLibraryW( L"uxtheme.dll" ))) return;
    set_theme = (void *)GetProcAddress( uxtheme, "SetWindowTheme" );
    if (set_theme) set_theme( hwnd, L" ", L" " );      /* wine refuses empty names */
    /* comctl32 keeps the system colours it drew with when it loaded, until a window hears of a change */
    SendMessageW( hwnd, WM_SYSCOLORCHANGE, 0, 0 );
    InvalidateRect( hwnd, NULL, TRUE );
}

/***********************************************************************
 *      W2SWindowCreated  (win32swiftui.@)
 */
void WINAPI W2SWindowCreated( HWND hwnd )
{
    const struct w2s_kind *kind;
    WCHAR name[64];

    if (!unix_ready) return;
    if (GetClassNameW( hwnd, name, ARRAYSIZE(name) ) && wcsstr( name, TOOLTIPS_CLASSW ))
    {
        w2s_observe_tooltip( hwnd );
        return;
    }
    if (!(GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD))
    {
        w2s_announce_app_name();        /* system.c: the Dock and the menu bar name the program, not wine */
        w2s_sync_look();                /* look.c: wine's colours follow macOS */
        w2s_frame_created( hwnd );      /* menus.c: its menu goes to the Mac menu bar */
        return;
    }
    if (!(kind = w2s_select_kind( hwnd )))
    {
        if (wcsstr( name, L"SysHeader32" )) classic_header_in_dark( hwnd );
        release_combo_parts( hwnd );
        return;
    }
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

    /* no window: the open alert or panel; a top-level window: its menu bar */
    params.handle = 0;
    if (hwnd && ctl && ctl->handle) params.handle = ctl->handle;
    else if (hwnd && !(params.handle = w2s_frame_handle( hwnd ))) return NULL;
    params.json = W2S_PTR( json );
    params.json_len = strlen( json );
    params.size = 65536;
    buf = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, params.size );
    params.buffer = W2S_PTR( buf );
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
        show_hook_tls = TlsAlloc();
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
