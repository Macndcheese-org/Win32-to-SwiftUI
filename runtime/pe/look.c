/*
 * win32swiftui.dll: Light and Dark mode (map: 70-look.yaml).
 *
 * wine draws dialogs, and whatever an app draws with GetSysColor, from its
 * system colour table. The native side resolves that table from NSColor for
 * the current macOS appearance and accent colour, and wakes us when it
 * changes (W2S_WAKE_LOOK); we write it with SetSysColors, which sends
 * WM_SYSCOLORCHANGE and repaints, and send WM_THEMECHANGED to the process's
 * windows, as a theme change does. SetSysColors doesn't touch the saved
 * colours (HKCU\Control Panel\Colors): the change lasts for the session.
 *
 * Each translated control then tells its native view what the app paints
 * behind it (its parent's WM_CTLCOLORSTATIC brush), and the native view
 * takes the appearance that reads on it: dark controls on wine's dark dialog
 * background, light ones on an app's own white page.
 */
#include "w2s_pe.h"

static UINT64 look_version;
static LONG look_busy;

static void refresh_controls( HWND hwnd )
{
    struct w2s_control *ctl = w2s_control_from_hwnd( hwnd );

    SendMessageTimeoutW( hwnd, WM_THEMECHANGED, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL );
    if (!ctl) return;
    /* its thread sends it the fresh snapshot, the backdrop measured again */
    ctl->backdrop_stale = TRUE;
    PostMessageW( hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
}

static BOOL CALLBACK child_proc( HWND hwnd, LPARAM lparam )
{
    refresh_controls( hwnd );
    return TRUE;
}

static BOOL CALLBACK top_proc( HWND hwnd, LPARAM lparam )
{
    DWORD pid;

    GetWindowThreadProcessId( hwnd, &pid );
    if (pid != GetCurrentProcessId()) return TRUE;
    refresh_controls( hwnd );
    EnumChildWindows( hwnd, child_proc, 0 );
    return TRUE;
}

/* the table if it changed since we last wrote it */
static BOOL look_changed( struct w2s_system_colors_params *params )
{
    memset( params, 0, sizeof(*params) );
    params->version = look_version;
    w2s_call( unix_w2s_system_colors, params );
    return params->count != 0;
}

BOOL w2s_look_pending(void)
{
    struct w2s_system_colors_params params;
    return look_changed( &params );
}

/***********************************************************************
 *      w2s_sync_look
 *
 * Writes wine's system colours from the native table, if it changed.
 */
void w2s_sync_look(void)
{
    struct w2s_system_colors_params params;
    INT indices[W2S_SYSTEM_COLORS];
    COLORREF values[W2S_SYSTEM_COLORS];
    UINT i, n = 0;

    if (InterlockedExchange( &look_busy, 1 )) return;
    if (look_changed( &params ))
    {
        look_version = params.version;
        for (i = 0; i < params.count && i < W2S_SYSTEM_COLORS; i++)
        {
            if (params.colors[i] == CLR_INVALID) continue;
            indices[n] = i;
            values[n++] = params.colors[i];
        }
        TRACE( "system colours for a %s appearance: %u of them, COLOR_BTNFACE %06x\n",
               params.dark ? "dark" : "light", n, (unsigned int)params.colors[COLOR_BTNFACE] );
        if (n) SetSysColors( n, indices, values );
        EnumWindows( top_proc, 0 );
    }
    InterlockedExchange( &look_busy, 0 );
}

/***********************************************************************
 *      w2s_backdrop
 *
 * The colour the app paints behind a control: its parent's answer to
 * WM_CTLCOLORSTATIC, what a transparent control draws on. CLR_INVALID when
 * it isn't one colour (a hollow brush over the app's own painting).
 */
COLORREF w2s_backdrop( HWND hwnd )
{
    HWND parent = GetParent( hwnd );
    COLORREF ret = CLR_INVALID;
    LOGBRUSH lb;
    HBRUSH brush;
    HDC hdc;

    if (!parent || !(hdc = CreateCompatibleDC( NULL ))) return CLR_INVALID;
    brush = (HBRUSH)SendMessageW( parent, WM_CTLCOLORSTATIC, (WPARAM)hdc, (LPARAM)hwnd );
    if (brush && GetObjectW( brush, sizeof(lb), &lb ) == sizeof(lb) && lb.lbStyle == BS_SOLID)
        ret = lb.lbColor & 0xffffff;
    DeleteDC( hdc );
    return ret;
}
