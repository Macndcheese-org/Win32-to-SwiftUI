/*
 * win32swiftui.dll: menus (map: menu.bar, menu.popup).
 *
 * A top-level window with a menu shows it in the Mac menu bar while it is
 * active. Its own menu bar goes: wine's win32u sees __wine_native_menu_bar and
 * gives the window no menu bar height, so the space goes to the client area.
 * A resizable window keeps its frame and gets WM_SIZE (the app lays itself out
 * into the freed strip); a fixed-size one gets shorter by the menu height, so
 * no gap is left.
 *
 * The Win32 menu stays authoritative. Before a submenu opens natively, the app
 * gets WM_INITMENU/WM_INITMENUPOPUP, as on Windows, so it can check and enable
 * items; the native side waits briefly for the refreshed submenu. A choice
 * comes back as WM_COMMAND with the item's id.
 *
 * TrackPopupMenuEx becomes an NSMenu popped up at the same place.
 */
#include "w2s_pe.h"

#define MAX_DEPTH 8
#define MAX_ITEMS 500

static const WCHAR frame_prop[] = L"Win32ToSwiftUI.Frame";
static const WCHAR native_bar_prop[] = L"__wine_native_menu_bar";   /* read by win32u */

struct w2s_frame
{
    HWND hwnd;
    WNDPROC orig;
    struct w2s_host host;
    UINT64 handle;          /* the native menu bar (a "menubar" control on the unix side) */
    HMENU menu;             /* the menu shown */
    char *last;             /* its last snapshot */
    BOOL native;            /* __wine_native_menu_bar is set */
    BOOL active;
    int shrink;             /* menu bar height to take off a fixed-size window, pending */
    BOOL alert_posted;      /* a dialog that is an alert, to be shown as one (alertdlg.c) */
    BOOL show_dialog;       /* ... but let it show as the dialog it is */
};

/* ---------- snapshot ---------- */

static void json_menu( struct json *j, HMENU menu, int depth );

static void json_menu_item( struct json *j, HMENU menu, int pos, int depth )
{
    MENUITEMINFOW info = { sizeof(info) };
    WCHAR text[256];

    info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP;
    info.dwTypeData = text;
    info.cch = ARRAYSIZE(text);
    text[0] = 0;
    if (!GetMenuItemInfoW( menu, pos, TRUE, &info )) return;

    json_obj_begin( j );
    if (info.fType & MFT_SEPARATOR) json_bool( j, "sep", TRUE );
    else
    {
        json_int( j, "id", info.wID );
        json_str( j, "text", text );
        if (info.fState & MFS_CHECKED) json_bool( j, "checked", TRUE );
        if (info.fType & MFT_RADIOCHECK) json_bool( j, "radio", TRUE );
        if (info.fState & (MFS_DISABLED | MFS_GRAYED)) json_bool( j, "disabled", TRUE );
        if (info.fType & MFT_OWNERDRAW) json_bool( j, "ownerDraw", TRUE );
        if (info.fType & MFT_RIGHTJUSTIFY) json_bool( j, "right", TRUE );
        if (info.hSubMenu && depth < MAX_DEPTH)
        {
            json_key_obj_begin( j, "sub" );
            json_int( j, "menu", (INT64)(UINT_PTR)info.hSubMenu );
            json_int( j, "pos", pos );
            json_menu( j, info.hSubMenu, depth + 1 );
            json_obj_end( j );
        }
    }
    json_obj_end( j );
}

static void json_menu( struct json *j, HMENU menu, int depth )
{
    int i, count = GetMenuItemCount( menu );

    json_arr_begin( j, "items" );
    for (i = 0; i < count && i < MAX_ITEMS; i++) json_menu_item( j, menu, i, depth );
    json_arr_end( j );
}

static char *menu_snapshot( HMENU menu, const char *entry )
{
    struct json j;
    char *copy;

    json_init( &j );
    json_obj_begin( &j );
    WCHAR module[MAX_PATH], *base, *dot;

    json_str_a( &j, "entry", entry );
    json_int( &j, "menu", (INT64)(UINT_PTR)menu );
    /* the program's name (its image's file name without .exe): About <program> */
    if (GetModuleFileNameW( NULL, module, MAX_PATH ))
    {
        base = wcsrchr( module, '\\' ) ? wcsrchr( module, '\\' ) + 1 : module;
        if ((dot = wcsrchr( base, '.' ))) *dot = 0;
        json_str( &j, "program", base );
    }
    json_menu( &j, menu, 0 );
    json_obj_end( &j );
    copy = HeapAlloc( GetProcessHeap(), 0, j.len + 1 );
    memcpy( copy, j.buf, j.len + 1 );
    json_free( &j );
    return copy;
}

/* ---------- the menu bar ---------- */

static void send_menu( struct w2s_frame *frame, BOOL force )
{
    struct w2s_control_update_params params;
    char *snap;

    if (!frame->handle || !frame->menu) return;
    snap = menu_snapshot( frame->menu, "menubar" );
    if (!force && frame->last && !strcmp( frame->last, snap ))
    {
        HeapFree( GetProcessHeap(), 0, snap );
        return;
    }
    params.handle = frame->handle;
    params.json = W2S_PTR( snap );
    params.json_len = strlen( snap );
    w2s_call( unix_w2s_control_update, &params );
    if (frame->last) HeapFree( GetProcessHeap(), 0, frame->last );
    frame->last = snap;
}

static void show_menu( struct w2s_frame *frame, BOOL show )
{
    struct w2s_control_focus_params params = { frame->handle, show };
    if (frame->handle) w2s_call( unix_w2s_control_focus, &params );
}

/* The window's own menu bar goes; see the top of the file for the space.
 * in_frame_change: we're in the WM_NCCALCSIZE of a frame change already under
 * way (SetMenu, DrawMenuBar): the property only has to be there before wine
 * computes it; a SetWindowPos of our own would be overwritten by it. */
static void set_native( struct w2s_frame *frame, BOOL native, BOOL in_frame_change )
{
    HWND hwnd = frame->hwnd;
    MENUBARINFO info = { sizeof(info) };
    int height = 0;

    if (frame->native == native) return;
    if (native && GetMenuBarInfo( hwnd, OBJID_MENU, 0, &info )) height = info.rcBar.bottom - info.rcBar.top;
    if (native) SetPropW( hwnd, native_bar_prop, (HANDLE)1 );
    else RemovePropW( hwnd, native_bar_prop );
    frame->native = native;
    /* a fixed-size window gets shorter instead, so no gap is left */
    if (native && height > 0 && !(GetWindowLongW( hwnd, GWL_STYLE ) & WS_THICKFRAME) && !IsZoomed( hwnd ))
    {
        frame->shrink = height;
        PostMessageW( hwnd, w2s_wake_message, W2S_WAKE_SHRINK, 0 );
    }
    if (!in_frame_change)
        SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    TRACE( "%p: menu bar %s (%d px)\n", hwnd, native ? "native" : "wine's", height );
}

static void shrink_frame( struct w2s_frame *frame )
{
    RECT win;
    int height = frame->shrink;

    frame->shrink = 0;
    if (!height || !frame->native || !GetWindowRect( frame->hwnd, &win )) return;
    SetWindowPos( frame->hwnd, 0, 0, 0, win.right - win.left, win.bottom - win.top - height,
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
}

static BOOL create_menubar( struct w2s_frame *frame )
{
    struct w2s_control_create_params params;
    char *snap;

    if (!frame->host.view && !w2s_get_host( frame->hwnd, &frame->host )) return FALSE;
    snap = menu_snapshot( frame->menu, "menubar" );
    params.host_view = frame->host.view;
    params.window = frame->host.window;
    params.post_wake = frame->host.post_wake;
    params.hwnd = (UINT_PTR)frame->hwnd;
    params.entry = W2S_PTR( "menubar" );
    params.json = W2S_PTR( snap );
    params.json_len = strlen( snap );
    params.handle = 0;
    w2s_call( unix_w2s_control_create, &params );
    frame->handle = params.handle;
    frame->last = snap;
    return frame->handle != 0;
}

static void destroy_menubar( struct w2s_frame *frame )
{
    struct w2s_control_destroy_params params = { frame->handle };
    if (frame->handle) w2s_call( unix_w2s_control_destroy, &params );
    frame->handle = 0;
    if (frame->last) HeapFree( GetProcessHeap(), 0, frame->last );
    frame->last = NULL;
}

/* the window's menu may have changed (SetMenu, DrawMenuBar, first show) */
static void sync_menu( struct w2s_frame *frame, BOOL in_frame_change )
{
    HMENU menu = GetMenu( frame->hwnd );

    if (!IsMenu( menu )) menu = NULL;
    if (menu != frame->menu)
    {
        destroy_menubar( frame );
        frame->menu = menu;
        if (menu && create_menubar( frame ))
        {
            set_native( frame, TRUE, in_frame_change );
            if (frame->active) show_menu( frame, TRUE );
        }
        else set_native( frame, FALSE, in_frame_change );
        return;
    }
    send_menu( frame, FALSE );
}

/* the popup that holds command id, and its place in the menu above it */
static HMENU popup_of( HMENU menu, UINT id, int *pos )
{
    int i, j, count = GetMenuItemCount( menu );
    HMENU sub, found;

    for (i = 0; i < count; i++)
    {
        if (!(sub = GetSubMenu( menu, i ))) continue;
        for (j = GetMenuItemCount( sub ) - 1; j >= 0; j--)
            if (!GetSubMenu( sub, j ) && GetMenuItemID( sub, j ) == id) break;
        if (j >= 0)
        {
            *pos = i;
            return sub;
        }
        if ((found = popup_of( sub, id, pos ))) return found;
    }
    return NULL;
}

/* a shortcut the menu bar matched, as TranslateAccelerator takes one: the app is
 * told the item's menus open (it may enable or check it there), and a disabled
 * or grayed item does nothing */
static void menu_shortcut( struct w2s_frame *frame, UINT id )
{
    UINT state = ~0u;
    HMENU popup;
    int pos = 0;

    if (!IsWindowEnabled( frame->hwnd )) return;
    if (frame->menu && IsMenu( frame->menu ))
    {
        popup = popup_of( frame->menu, id, &pos );
        SendMessageW( frame->hwnd, WM_INITMENU, (WPARAM)frame->menu, 0 );
        if (popup) SendMessageW( frame->hwnd, WM_INITMENUPOPUP, (WPARAM)popup, MAKELPARAM( pos, FALSE ) );
        state = GetMenuState( frame->menu, id, MF_BYCOMMAND );
    }
    if (state == ~0u || !(state & (MF_DISABLED | MF_GRAYED)))
        PostMessageW( frame->hwnd, WM_COMMAND, MAKEWPARAM( id, 1 ), 0 );
    send_menu( frame, FALSE );      /* the states the app set there */
}


BOOL WINAPI W2SShellAbout( HWND owner, const WCHAR *app, const WCHAR *other, HICON icon, BOOL *ret );

/* The application menu's About <program> for an app that has no About of its own: the
 * standard panel, with the program's name, version and copyright */
static void menu_about( HWND hwnd )
{
    WCHAR module[MAX_PATH], name[MAX_PATH], *base, *dot;
    BOOL ret;

    if (!GetModuleFileNameW( NULL, module, MAX_PATH )) return;
    base = wcsrchr( module, '\\' );
    lstrcpynW( name, base ? base + 1 : module, MAX_PATH );
    if ((dot = wcsrchr( name, '.' ))) *dot = 0;
    W2SShellAbout( hwnd, name, NULL, NULL, &ret );
}

static void apply_menu_events( struct w2s_frame *frame )
{
    struct w2s_pop_events_params params;
    struct w2s_event *events;
    char small[1024], *buf = small;
    int i, count;

    params.handle = frame->handle;
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
    if (!params.len) return;
    count = json_parse_events( buf, &events );
    for (i = 0; i < count; i++)
    {
        const struct w2s_event *ev = &events[i];

        if (!strcmp( ev->type, "menu" ) && ev->has_value)
        {
            /* as if chosen in wine's menu bar */
            PostMessageW( frame->hwnd, WM_COMMAND, MAKEWPARAM( (UINT)ev->value, 0 ), 0 );
        }
        else if (!strcmp( ev->type, "menuKey" ) && ev->has_value) menu_shortcut( frame, (UINT)ev->value );
        else if (!strcmp( ev->type, "about" )) menu_about( frame->hwnd );
        else if (!strcmp( ev->type, "menuOpen" ) && ev->array_count >= 2)
        {
            /* a submenu is about to open: let the app update it, then send it
             * again (the native side is waiting for it). a[0]: menu, a[1]: pos,
             * a[2]: 1 for a top-level menu */
            HMENU sub = (HMENU)(UINT_PTR)(unsigned int)ev->array[0];
            if (ev->array_count > 2 && ev->array[2]) SendMessageW( frame->hwnd, WM_INITMENU, (WPARAM)frame->menu, 0 );
            if (IsMenu( sub )) SendMessageW( frame->hwnd, WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM( ev->array[1], FALSE ) );
            send_menu( frame, TRUE );
        }
    }
    json_free_events( events, count );
    if (buf != small) HeapFree( GetProcessHeap(), 0, buf );
}

static LRESULT CALLBACK frame_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct w2s_frame *frame = GetPropW( hwnd, frame_prop );
    LRESULT ret;

    if (!frame) return DefWindowProcW( hwnd, msg, wparam, lparam );

    if (msg == w2s_wake_message && w2s_wake_message)
    {
        if (wparam == W2S_WAKE_ALERT)
        {
            frame->alert_posted = FALSE;
            if (!w2s_dialog_run_alert( hwnd ))
            {
                frame->show_dialog = TRUE;
                ShowWindow( hwnd, SW_SHOW );
            }
        }
        else if (wparam == W2S_WAKE_SHRINK) shrink_frame( frame );
        else if (wparam == W2S_WAKE_LOOK) w2s_sync_look();
        else if (frame->handle) apply_menu_events( frame );
        return 0;
    }
    /* a dialog that is only an icon, text and buttons is shown as the alert of the system */
    if (msg == WM_WINDOWPOSCHANGING && (((WINDOWPOS *)lparam)->flags & SWP_SHOWWINDOW) && !frame->show_dialog &&
        (frame->alert_posted || (!(GetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE) && w2s_dialog_is_alert( hwnd ))))
    {
        ((WINDOWPOS *)lparam)->flags &= ~SWP_SHOWWINDOW;
        if (!frame->alert_posted)
        {
            frame->alert_posted = TRUE;
            PostMessageW( hwnd, w2s_wake_message, W2S_WAKE_ALERT, 0 );
        }
    }
    /* SetMenu and DrawMenuBar recalculate the frame: be ready before wine does */
    if (msg == WM_NCCALCSIZE) sync_menu( frame, TRUE );
    if (msg == WM_NCDESTROY)
    {
        WNDPROC orig = frame->orig;
        if (frame->active) show_menu( frame, FALSE );
        destroy_menubar( frame );
        if (frame->host.surface) w2s_release_host( hwnd, &frame->host );
        RemovePropW( hwnd, frame_prop );
        RemovePropW( hwnd, native_bar_prop );
        HeapFree( GetProcessHeap(), 0, frame );
        return CallWindowProcW( orig, hwnd, msg, wparam, lparam );
    }

    ret = CallWindowProcW( frame->orig, hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_ACTIVATE:
        frame->active = LOWORD( wparam ) != WA_INACTIVE;
        sync_menu( frame, FALSE );
        show_menu( frame, frame->active );
        break;
    case WM_SHOWWINDOW:
        sync_menu( frame, FALSE );
        break;
    }
    return ret;
}

/* a top-level window: its menu, if it has or gets one, goes to the menu bar */
void w2s_frame_created( HWND hwnd )
{
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );
    struct w2s_frame *frame;

    TRACE( "frame %p: style %08lx\n", hwnd, style );
    if ((style & WS_CHILD) || (style & WS_CAPTION) != WS_CAPTION) return;
    if (GetPropW( hwnd, frame_prop )) return;
    frame = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*frame) );
    frame->hwnd = hwnd;
    SetPropW( hwnd, frame_prop, frame );
    frame->orig = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)frame_proc );
    frame->active = GetActiveWindow() == hwnd;
    sync_menu( frame, FALSE );
    /* made already shown (Windows Installer's dialogs): shown as the alert it is, not as this */
    if ((style & WS_VISIBLE) && w2s_dialog_is_alert( hwnd ))
    {
        TRACE( "frame %p: an alert\n", hwnd );
        frame->alert_posted = TRUE;
        ShowWindow( hwnd, SW_HIDE );
        PostMessageW( hwnd, w2s_wake_message, W2S_WAKE_ALERT, 0 );
    }
}

/* for tests/gallery: the menu the native side shows for this window */
UINT64 w2s_frame_handle( HWND hwnd )
{
    struct w2s_frame *frame = GetPropW( hwnd, frame_prop );
    return frame ? frame->handle : 0;
}

/***********************************************************************
 *      W2STrackPopupMenu  (win32swiftui.@)
 */
/* The item (by position) the next popup menu is answered with without showing it: a native
 * control that already shows the choices (a pop-up button) has made the choice the app's own
 * menu asks for. -1: none. */
int w2s_popup_choice = -1;

BOOL WINAPI W2STrackPopupMenu( HMENU menu, UINT flags, INT x, INT y, HWND hwnd, TPMPARAMS *params, INT *ret )
{
    HWND top = hwnd ? GetAncestor( hwnd, GA_ROOT ) : NULL;
    struct w2s_host host;
    struct json j;
    char *result, *snap;
    POINT pt = { x, y };
    RECT client;
    double id = 0;

    if (w2s_popup_choice >= 0 && IsMenu( menu ))
    {
        UINT cmd = GetMenuItemID( menu, w2s_popup_choice );

        w2s_popup_choice = -1;
        if (cmd != (UINT)-1)
        {
            if (flags & TPM_RETURNCMD) *ret = (INT)cmd;
            else
            {
                if (cmd && !(flags & TPM_NONOTIFY)) PostMessageW( hwnd, WM_COMMAND, MAKEWPARAM( cmd, 0 ), 0 );
                *ret = TRUE;
            }
            return TRUE;
        }
    }
    if (!IsMenu( menu ) || !top || !IsWindowVisible( top )) return FALSE;
    if (!w2s_get_host( top, &host )) return FALSE;

    /* what Windows sends before a popup menu */
    SendMessageW( hwnd, WM_ENTERMENULOOP, TRUE, 0 );
    SendMessageW( hwnd, WM_INITMENUPOPUP, (WPARAM)menu, 0 );

    ScreenToClient( top, &pt );
    GetClientRect( top, &client );
    snap = menu_snapshot( menu, "popup" );
    json_init( &j );
    json_obj_begin( &j );
    json_int( &j, "view", (INT64)host.view );
    json_int( &j, "x", pt.x );
    json_int( &j, "y", pt.y );
    json_int( &j, "widthPx", client.right );
    json_int( &j, "heightPx", client.bottom );
    json_raw( &j, "menu", snap );
    json_obj_end( &j );
    HeapFree( GetProcessHeap(), 0, snap );

    *ret = 0;
    if (w2s_run_request( "popup", top, j.buf, &result ))
    {
        json_get_num( result, "id", &id );
        HeapFree( GetProcessHeap(), 0, result );
    }
    json_free( &j );
    w2s_release_host( top, &host );
    SendMessageW( hwnd, WM_EXITMENULOOP, TRUE, 0 );

    if (flags & TPM_RETURNCMD) *ret = (INT)id;
    else
    {
        if (id && !(flags & TPM_NONOTIFY)) PostMessageW( hwnd, WM_COMMAND, MAKEWPARAM( (UINT)id, 0 ), 0 );
        *ret = TRUE;
    }
    TRACE( "popup menu %p -> %d\n", menu, (int)id );
    return TRUE;
}
