/*
 * win32swiftui.dll: the controls the runtime translates (milestone 1), one
 * kind per map entry: which windows it takes, what it tells the native view
 * (snapshot), and how native events become Win32 actions (apply).
 */
#include "w2s_pe.h"
#include "w2s_map_tables.h"  /* w2s_toolbar_images */

/* ---------- helpers ---------- */

static void class_name( HWND hwnd, WCHAR *name, int size )
{
    WCHAR *bang;
    name[0] = 0;
    GetClassNameW( hwnd, name, size );
    /* comctl32 v6 classes come back versioned ("6.0.2600.2982!Button") */
    if ((bang = wcschr( name, '!' ))) memmove( name, bang + 1, (wcslen( bang + 1 ) + 1) * sizeof(WCHAR) );
}

static BOOL is_class( const WCHAR *name, const WCHAR *want )
{
    return !_wcsicmp( name, want );
}

static void json_text_list( struct json *j, const char *key, HWND hwnd, UINT count_msg, UINT len_msg, UINT text_msg, int limit )
{
    int i, count = (int)SendMessageW( hwnd, count_msg, 0, 0 );
    WCHAR small[256];

    json_arr_begin( j, key );
    for (i = 0; i < count && i < limit; i++)
    {
        int len = (int)SendMessageW( hwnd, len_msg, i, 0 );
        WCHAR *text = small;
        if (len < 0) len = 0;
        if (len >= ARRAYSIZE(small)) text = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) );
        text[0] = 0;
        SendMessageW( hwnd, text_msg, i, (LPARAM)text );
        json_str( j, NULL, text );
        if (text != small) HeapFree( GetProcessHeap(), 0, text );
    }
    json_arr_end( j );
}

/* ---------- Button ---------- */

static BOOL is_native_wizard_button( HWND hwnd );

/* a native wizard's Back and Next in the macOS assistant style: wine's
 * "< Back" and "Next >" lose their arrows, and read Go Back and Continue in
 * English (other languages keep wine's word) */
static void wizard_button_display( struct w2s_control *ctl, struct json *j )
{
    WCHAR text[64], plain[64], *p = text, *end;
    int i, n = 0;

    if (!is_native_wizard_button( ctl->hwnd )) return;
    GetWindowTextW( ctl->hwnd, text, ARRAYSIZE(text) );
    while (*p == '<' || *p == ' ') p++;
    end = p + wcslen( p );
    while (end > p && (end[-1] == '>' || end[-1] == ' ')) *--end = 0;
    for (i = 0; p[i]; i++) if (p[i] != '&') plain[n++] = p[i];
    plain[n] = 0;
    if (!_wcsicmp( plain, L"Back" )) p = (WCHAR *)L"Go Back";
    else if (!_wcsicmp( plain, L"Next" )) p = (WCHAR *)L"Continue";
    json_str( j, "display", p );
}

static void button_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    json_int( j, "checked", SendMessageW( ctl->hwnd, BM_GETCHECK, 0, 0 ) );
    json_bool( j, "isDefault", (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON );
    json_bool( j, "noPrefix", FALSE );
    json_bool( j, "leftText", (style & BS_LEFTTEXT) != 0 );
    json_bool( j, "isCancel", GetDlgCtrlID( ctl->hwnd ) == IDCANCEL );
    wizard_button_display( ctl, j );
}

static void button_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (!strcmp( ev->type, "click" )) SendMessageW( ctl->hwnd, BM_CLICK, 0, 0 );
}

/* A group box's title goes above the box, where macOS puts it (HIG: Boxes),
 * when nothing of the app's sits in that band: the box then takes the Win32
 * rectangle, whose caption band becomes the box's padding. Otherwise the title
 * goes inside the box's top (NSBox's belowTop). */
#define GROUPBOX_TITLE_BAND 18

static BOOL groupbox_room_above( HWND hwnd )
{
    HWND parent = GetParent( hwnd ), child;
    HFONT font = (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 );
    RECT rc, band, sib, tmp;
    WCHAR text[256];
    SIZE size = { 0 };
    HGDIOBJ old;
    HDC hdc;

    if (!parent) return FALSE;
    GetWindowRect( hwnd, &rc );
    MapWindowPoints( NULL, parent, (POINT *)&rc, 2 );
    if (rc.top < GROUPBOX_TITLE_BAND) return FALSE;
    /* the title's width, in the Mac's larger font */
    text[0] = 0;
    GetWindowTextW( hwnd, text, ARRAYSIZE(text) );
    if ((hdc = GetDC( hwnd )))
    {
        old = SelectObject( hdc, font ? font : GetStockObject( DEFAULT_GUI_FONT ) );
        GetTextExtentPoint32W( hdc, text, wcslen( text ), &size );
        SelectObject( hdc, old );
        ReleaseDC( hwnd, hdc );
    }
    SetRect( &band, rc.left, rc.top - GROUPBOX_TITLE_BAND, min( rc.right, rc.left + size.cx * 4 / 3 + 16 ), rc.top );
    for (child = GetWindow( parent, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
    {
        if (child == hwnd || !IsWindowVisible( child )) continue;
        GetWindowRect( child, &sib );
        MapWindowPoints( NULL, parent, (POINT *)&sib, 2 );
        /* a box around this one isn't in the way */
        if (sib.left <= band.left && sib.top <= band.top && sib.right >= band.right && sib.bottom >= rc.bottom)
            continue;
        if (IntersectRect( &tmp, &sib, &band )) return FALSE;
    }
    return TRUE;
}

static void groupbox_snapshot( struct w2s_control *ctl, struct json *j )
{
    /* made with the dialog, before the controls around it: look again once
     * they're all there (a posted refresh comes after the dialog is built) */
    if (!ctl->data)
    {
        ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(int) );
        PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    }
    json_bool( j, "titleAbove", groupbox_room_above( ctl->hwnd ) );
}

static void nothing_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
}

static void nothing_snapshot( struct w2s_control *ctl, struct json *j )
{
}

/* BS_SPLITBUTTON: the arrow half raises BCN_DROPDOWN; the app answers with its
 * own TrackPopupMenu (a native menu too, see menu.popup) */
static void split_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    BUTTON_SPLITINFO info = { BCSIF_STYLE };

    button_snapshot( ctl, j );
    json_bool( j, "isDefault", (style & BS_TYPEMASK) == BS_DEFSPLITBUTTON );
    if (!SendMessageW( ctl->hwnd, BCM_GETSPLITINFO, 0, (LPARAM)&info )) info.uSplitStyle = 0;
    json_bool( j, "noSplit", (info.uSplitStyle & BCSS_NOSPLIT) != 0 );
}

static void split_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (!strcmp( ev->type, "click" )) SendMessageW( ctl->hwnd, BM_CLICK, 0, 0 );
    else if (!strcmp( ev->type, "dropdown" ))
    {
        NMBCDROPDOWN nm = { { 0 } };
        GetClientRect( ctl->hwnd, &nm.rcButton );
        w2s_notify_parent( ctl->hwnd, BCN_DROPDOWN, &nm.hdr );
    }
}

/* BS_COMMANDLINK: the title and the note under it (BCM_SETNOTE) */
static void commandlink_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    DWORD len = SendMessageW( ctl->hwnd, BCM_GETNOTELENGTH, 0, 0 );
    WCHAR small[256], *note = small;

    button_snapshot( ctl, j );
    json_bool( j, "isDefault", (style & BS_TYPEMASK) == BS_DEFCOMMANDLINK );
    if (len >= ARRAYSIZE(small)) note = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) );
    note[0] = 0;
    if (len)
    {
        DWORD size = len + 1;
        SendMessageW( ctl->hwnd, BCM_GETNOTE, (WPARAM)&size, (LPARAM)note );
    }
    json_str( j, "note", note );
    if (note != small) HeapFree( GetProcessHeap(), 0, note );
}

/* push and default buttons share a view: the default moves between buttons
 * with the focus (DM_SETDEFID, BM_SETSTYLE) and must not rebuild the view */
static const struct w2s_kind kind_button_push = { "button.push", button_snapshot, button_apply, NULL, "button.push" };
static const struct w2s_kind kind_button_default = { "button.default", button_snapshot, button_apply, NULL, "button.push" };
static const struct w2s_kind kind_button_checkbox = { "button.checkbox", button_snapshot, button_apply };
static const struct w2s_kind kind_button_3state = { "button.3state", button_snapshot, button_apply };
static const struct w2s_kind kind_button_pushlike = { "button.pushlike", button_snapshot, button_apply };
static const struct w2s_kind kind_button_radio = { "button.radio", button_snapshot, button_apply };
static const struct w2s_kind kind_button_groupbox = { "button.groupbox", groupbox_snapshot, nothing_apply };
static const struct w2s_kind kind_button_split = { "button.split", split_snapshot, split_apply };
static const struct w2s_kind kind_button_commandlink = { "button.commandlink", commandlink_snapshot, button_apply };

/* ---------- Static ---------- */

static void static_text_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    DWORD type = style & SS_TYPEMASK;
    const char *align = type == SS_CENTER ? "center" : type == SS_RIGHT ? "trailing" : "leading";
    json_str_a( j, "align", align );
    json_bool( j, "wrap", type != SS_LEFTNOWORDWRAP && type != SS_SIMPLE );
    json_bool( j, "noPrefix", (style & SS_NOPREFIX) != 0 );
    json_bool( j, "centerVertically", (style & SS_CENTERIMAGE) != 0 );
}

static void static_separator_snapshot( struct w2s_control *ctl, struct json *j )
{
    json_bool( j, "vertical", (GetWindowLongW( ctl->hwnd, GWL_STYLE ) & SS_TYPEMASK) == SS_ETCHEDVERT );
}

/* icon/bitmap -> 32bpp BGRA, base64 */
void json_base64( struct json *j, const char *key, const BYTE *data, size_t n )
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = HeapAlloc( GetProcessHeap(), 0, (n + 2) / 3 * 4 + 1 ), *o = out;
    size_t i;
    for (i = 0; i + 2 < n; i += 3)
    {
        *o++ = tab[data[i] >> 2];
        *o++ = tab[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
        *o++ = tab[((data[i + 1] & 15) << 2) | (data[i + 2] >> 6)];
        *o++ = tab[data[i + 2] & 63];
    }
    if (i < n)
    {
        *o++ = tab[data[i] >> 2];
        if (i + 1 < n)
        {
            *o++ = tab[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
            *o++ = tab[(data[i + 1] & 15) << 2];
        }
        else
        {
            *o++ = tab[(data[i] & 3) << 4];
            *o++ = '=';
        }
        *o++ = '=';
    }
    *o = 0;
    json_str_a( j, key, out );
    HeapFree( GetProcessHeap(), 0, out );
}

/* An icon or bitmap as 32bpp premultiplied BGRA, top-down (what CGImage gets).
 * An icon without an alpha channel takes its opacity from its mask. */
BYTE *w2s_image_bgra( HICON icon, HBITMAP bitmap, int w, int h )
{
    BITMAPINFO bmi;
    BYTE *bits, *out;
    HDC hdc;
    HBITMAP dib, old;
    int i, n = w * h;
    BOOL alpha = FALSE;

    memset( &bmi, 0, sizeof(bmi) );
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    hdc = CreateCompatibleDC( 0 );
    if (!(dib = CreateDIBSection( hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( hdc );
        return NULL;
    }
    old = SelectObject( hdc, dib );
    out = HeapAlloc( GetProcessHeap(), 0, (size_t)n * 4 );
    memset( bits, 0, (size_t)n * 4 );
    if (icon)
    {
        DrawIconEx( hdc, 0, 0, icon, w, h, 0, 0, DI_NORMAL );
        GdiFlush();
        memcpy( out, bits, (size_t)n * 4 );
        for (i = 0; i < n && !alpha; i++) alpha = out[i * 4 + 3] != 0;
        if (!alpha)
        {
            /* the AND mask over white: black where the icon is opaque */
            memset( bits, 0xff, (size_t)n * 4 );
            DrawIconEx( hdc, 0, 0, icon, w, h, 0, 0, DI_MASK );
            GdiFlush();
            for (i = 0; i < n; i++)
            {
                if (!bits[i * 4] && !bits[i * 4 + 1] && !bits[i * 4 + 2]) out[i * 4 + 3] = 0xff;
                else memset( out + i * 4, 0, 4 );
            }
        }
    }
    else
    {
        HDC src = CreateCompatibleDC( 0 );
        HGDIOBJ prev = SelectObject( src, bitmap );
        BitBlt( hdc, 0, 0, w, h, src, 0, 0, SRCCOPY );
        SelectObject( src, prev );
        DeleteDC( src );
        GdiFlush();
        memcpy( out, bits, (size_t)n * 4 );
        for (i = 0; i < n; i++) out[i * 4 + 3] = 0xff; /* bitmaps are opaque */
    }
    SelectObject( hdc, old );
    DeleteObject( dib );
    DeleteDC( hdc );
    return out;
}

static void static_image_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    const char *spec;
    HANDLE image;
    int w = 0, h = 0;
    BYTE *bits;

    if ((style & SS_TYPEMASK) == SS_ICON)
    {
        ICONINFO info;
        BITMAP bm;
        image = (HANDLE)SendMessageW( ctl->hwnd, STM_GETICON, 0, 0 );
        if (!image || !GetIconInfo( image, &info )) return;
        if (GetObjectW( info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bm), &bm ))
        {
            w = bm.bmWidth;
            h = info.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
        }
        if (info.hbmColor) DeleteObject( info.hbmColor );
        if (info.hbmMask) DeleteObject( info.hbmMask );
    }
    else
    {
        BITMAP bm;
        image = (HANDLE)SendMessageW( ctl->hwnd, STM_GETIMAGE, IMAGE_BITMAP, 0 );
        if (!image || !GetObjectW( image, sizeof(bm), &bm )) return;
        w = bm.bmWidth;
        h = abs( bm.bmHeight );
    }
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    if ((style & SS_TYPEMASK) == SS_ICON) bits = w2s_image_bgra( image, NULL, w, h );
    else bits = w2s_image_bgra( NULL, image, w, h );
    if (!bits) return;
    json_int( j, "imageWidth", w );
    json_int( j, "imageHeight", h );
    /* wine's stock icons show the macOS image with the same meaning (icons.c) */
    if ((style & SS_TYPEMASK) == SS_ICON && (spec = w2s_stock_icon( image, bits, w, h, NULL )))
        json_str_a( j, "imageSymbol", spec );
    else
        json_base64( j, "imageBGRA", bits, (size_t)w * h * 4 );
    HeapFree( GetProcessHeap(), 0, bits );
}

/* SS_*FRAME: an outline; SS_*RECT (the same family): a filled rectangle */
static void static_frame_snapshot( struct w2s_control *ctl, struct json *j )
{
    switch (GetWindowLongW( ctl->hwnd, GWL_STYLE ) & SS_TYPEMASK)
    {
    case SS_BLACKRECT: json_str_a( j, "fill", "label" ); break;
    case SS_GRAYRECT: json_str_a( j, "fill", "separator" ); break;
    case SS_WHITERECT: json_str_a( j, "fill", "window" ); break;
    default: json_str_a( j, "fill", "none" ); break;
    }
}

static const struct w2s_kind kind_static_frame = { "static.frame", static_frame_snapshot, nothing_apply };
static const struct w2s_kind kind_static_text = { "static.text", static_text_snapshot, nothing_apply };
static const struct w2s_kind kind_static_separator = { "static.separator", static_separator_snapshot, nothing_apply };
static const struct w2s_kind kind_static_image = { "static.image", static_image_snapshot, nothing_apply };

/* ---------- Edit ---------- */

static void edit_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    WCHAR cue[256] = {0};
    const char *align = (style & ES_CENTER) ? "center" : (style & ES_RIGHT) ? "trailing" : "leading";

    SendMessageW( ctl->hwnd, EM_GETCUEBANNER, (WPARAM)cue, ARRAYSIZE(cue) );
    json_str( j, "cue", cue );
    json_bool( j, "readonly", (style & ES_READONLY) != 0 );
    json_int( j, "limit", SendMessageW( ctl->hwnd, EM_GETLIMITTEXT, 0, 0 ) );
    json_str_a( j, "align", align );
}

/* replace only the part that changed, the way typing would, so the edit's
 * own filtering and EN_UPDATE/EN_CHANGE happen as usual */
static void replace_text( HWND edit, const WCHAR *text )
{
    int old_len, new_len, prefix = 0, suffix = 0;
    WCHAR *old, *inserted;

    old_len = GetWindowTextLengthW( edit );
    old = HeapAlloc( GetProcessHeap(), 0, (old_len + 1) * sizeof(WCHAR) );
    old[0] = 0;
    old_len = GetWindowTextW( edit, old, old_len + 1 );
    new_len = wcslen( text );

    while (prefix < old_len && prefix < new_len && old[prefix] == text[prefix]) prefix++;
    while (suffix < old_len - prefix && suffix < new_len - prefix &&
           old[old_len - 1 - suffix] == text[new_len - 1 - suffix]) suffix++;

    inserted = HeapAlloc( GetProcessHeap(), 0, (new_len - prefix - suffix + 1) * sizeof(WCHAR) );
    memcpy( inserted, text + prefix, (new_len - prefix - suffix) * sizeof(WCHAR) );
    inserted[new_len - prefix - suffix] = 0;

    SendMessageW( edit, EM_SETSEL, prefix, old_len - suffix );
    SendMessageW( edit, EM_REPLACESEL, TRUE, (LPARAM)inserted );

    HeapFree( GetProcessHeap(), 0, inserted );
    HeapFree( GetProcessHeap(), 0, old );
}

static void edit_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (!strcmp( ev->type, "text" ) && ev->string) replace_text( ctl->hwnd, ev->string );
}

/* EM_SHOWBALLOONTIP (map: edit.balloon): wine's edit has none; the native
 * field shows it as a popover. ctl->data holds it while it shows. */
struct edit_balloon
{
    WCHAR title[100];
    WCHAR text[1024];
    INT icon;
    UINT serial;            /* each EM_SHOWBALLOONTIP shows it again */
};

static BOOL edit_answer( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *ret )
{
    static UINT serial;
    const EDITBALLOONTIP *tip = (const EDITBALLOONTIP *)lparam;
    struct edit_balloon *b;

    if (msg == EM_HIDEBALLOONTIP)
    {
        HeapFree( GetProcessHeap(), 0, ctl->data );
        ctl->data = NULL;
        w2s_push( ctl, TRUE );
        *ret = TRUE;
        return TRUE;
    }
    if (msg != EM_SHOWBALLOONTIP) return FALSE;
    *ret = FALSE;
    if (!tip || tip->cbStruct < sizeof(*tip)) return TRUE;
    if (!ctl->data) ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b) );
    if (!(b = ctl->data)) return TRUE;
    lstrcpynW( b->title, tip->pszTitle ? tip->pszTitle : L"", ARRAYSIZE(b->title) );
    lstrcpynW( b->text, tip->pszText ? tip->pszText : L"", ARRAYSIZE(b->text) );
    b->icon = tip->ttiIcon;
    b->serial = ++serial;
    w2s_push( ctl, TRUE );
    *ret = TRUE;
    return TRUE;
}

static void edit_field_snapshot( struct w2s_control *ctl, struct json *j )
{
    struct edit_balloon *b = ctl->data;

    edit_snapshot( ctl, j );
    if (!b) return;
    json_key_obj_begin( j, "balloon" );
    json_str( j, "title", b->title );
    json_str( j, "text", b->text );
    /* TTI_INFO, TTI_WARNING, TTI_ERROR and their _LARGE forms */
    json_str_a( j, "icon", b->icon == TTI_INFO || b->icon == TTI_INFO_LARGE ? "info"
                         : b->icon == TTI_WARNING || b->icon == TTI_WARNING_LARGE ? "warning"
                         : b->icon == TTI_ERROR || b->icon == TTI_ERROR_LARGE ? "error" : "none" );
    json_int( j, "serial", b->serial );
    json_obj_end( j );
}

static const struct w2s_kind kind_edit_single = { "edit.single", edit_field_snapshot, edit_apply, edit_answer };
static const struct w2s_kind kind_edit_password = { "edit.password", edit_field_snapshot, edit_apply, edit_answer };
static const struct w2s_kind kind_edit_number = { "edit.number", edit_field_snapshot, edit_apply, edit_answer };
static const struct w2s_kind kind_edit_readonly = { "edit.readonly", edit_snapshot, nothing_apply };

/* ---------- multi-line Edit ---------- */

/* The native text view keeps every line break as "\n", the Win32 edit as
 * "\r\n": offsets and text are converted here. A lone "\r" or "\n" in the
 * Win32 text is one character on both sides. */

static WCHAR *window_text( HWND hwnd, int *len )
{
    int n = GetWindowTextLengthW( hwnd );
    WCHAR *text = HeapAlloc( GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR) );
    text[0] = 0;
    *len = GetWindowTextW( hwnd, text, n + 1 );
    return text;
}

static inline BOOL is_crlf( const WCHAR *text, int len, int i )
{
    return text[i] == '\r' && i + 1 < len && text[i + 1] == '\n';
}

/* Win32 offset -> native offset (inside a CRLF: before the break) */
static int native_offset( const WCHAR *text, int len, int pos )
{
    int i, n = pos;
    pos = min( max( pos, 0 ), len );
    for (i = 0; i < pos; i++) if (is_crlf( text, len, i )) n--;
    return max( n, 0 );
}

/* native offset -> Win32 offset */
static int win_offset( const WCHAR *text, int len, int npos )
{
    int i = 0, n = 0;
    while (i < len && n < npos)
    {
        i += is_crlf( text, len, i ) ? 2 : 1;
        n++;
    }
    return i;
}

/* the native form of the text, and FNV-1a over its UTF-16 units: the native
 * view publishes the same hash, so both sides can tell they hold one text */
static WCHAR *native_text( const WCHAR *text, int len, int *nlen, UINT32 *hash )
{
    WCHAR *out = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) );
    UINT32 h = 2166136261u;
    int i, n = 0;

    for (i = 0; i < len; i++)
    {
        WCHAR c = text[i];
        if (is_crlf( text, len, i )) continue;
        out[n++] = c;
        h ^= c & 0xff;
        h *= 16777619u;
        h ^= c >> 8;
        h *= 16777619u;
    }
    out[n] = 0;
    *nlen = n;
    if (hash) *hash = h;
    return out;
}

/* "\n" -> "\r\n" for the Win32 edit */
static WCHAR *crlf_text( const WCHAR *text )
{
    int n = 0, i;
    WCHAR *out, *o;

    for (i = 0; text[i]; i++) n += text[i] == '\n' ? 2 : 1;
    out = o = HeapAlloc( GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR) );
    for (i = 0; text[i]; i++)
    {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) *o++ = '\r';
        *o++ = text[i];
    }
    *o = 0;
    return out;
}

/* what the native view published: its selection and line layout */
struct ml_native
{
    UINT64 version;         /* of ctl->native this was read from */
    int len;                /* native length */
    UINT32 hash;
    int sel[2];
    int *lines, nlines;     /* native start offset of each visual line */
    int *tops, ntops;       /* each line's top, in pixels from the top of the visible area */
    int first;              /* first visible line */
    int caret[2], caret_char;
    int avg, left;          /* average character width, left edge of the text, pixels */
};

struct ml_data
{
    int caret_gen, scroll_gen, scroll_line;
    struct ml_native nat;
};

static struct ml_data *ml_data( struct w2s_control *ctl )
{
    if (!ctl->data) ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct ml_data) );
    return ctl->data;
}

static void ml_release( struct w2s_control *ctl )
{
    struct ml_data *data = ctl->data;
    if (data->nat.lines) HeapFree( GetProcessHeap(), 0, data->nat.lines );
    if (data->nat.tops) HeapFree( GetProcessHeap(), 0, data->nat.tops );
    HeapFree( GetProcessHeap(), 0, data );
}

static int json_int_or( const char *text, const char *key, int def )
{
    double v;
    return json_get_num( text, key, &v ) ? (int)v : def;
}

static struct ml_native *ml_native( struct w2s_control *ctl )
{
    struct ml_data *data = ml_data( ctl );
    struct ml_native *nat = &data->nat;
    const char *state = w2s_native_state( ctl, NULL );
    int *pair, n;
    double hash;

    if (!state) return NULL;
    if (nat->version == ctl->native_version && nat->lines) return nat;

    if (nat->lines) HeapFree( GetProcessHeap(), 0, nat->lines );
    if (nat->tops) HeapFree( GetProcessHeap(), 0, nat->tops );
    memset( nat, 0, sizeof(*nat) );
    nat->version = ctl->native_version;
    nat->len = json_int_or( state, "len", -1 );
    nat->hash = json_get_num( state, "hash", &hash ) ? (UINT32)hash : 0;
    if ((n = json_get_int_array( state, "sel", &pair )) == 2) memcpy( nat->sel, pair, sizeof(nat->sel) );
    if (pair) HeapFree( GetProcessHeap(), 0, pair );
    if ((n = json_get_int_array( state, "caret", &pair )) == 2) memcpy( nat->caret, pair, sizeof(nat->caret) );
    if (pair) HeapFree( GetProcessHeap(), 0, pair );
    nat->nlines = json_get_int_array( state, "lines", &nat->lines );
    nat->ntops = json_get_int_array( state, "tops", &nat->tops );
    nat->first = json_int_or( state, "first", 0 );
    nat->caret_char = json_int_or( state, "caretChar", nat->sel[1] );
    nat->avg = max( 1, json_int_or( state, "avg", 7 ) );
    nat->left = json_int_or( state, "left", 0 );
    return nat;
}

/* the visual line holding native offset n */
static int ml_line_of( const struct ml_native *nat, int n )
{
    int lo = 0, hi = nat->nlines - 1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        if (nat->lines[mid] <= n) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* a visual line's Win32 range, without its line break */
static void ml_line_range( const struct ml_native *nat, const WCHAR *text, int len, int line, int *start, int *end )
{
    *start = win_offset( text, len, nat->lines[line] );
    *end = line + 1 < nat->nlines ? win_offset( text, len, nat->lines[line + 1] ) : len;
    if (*end > *start && text[*end - 1] == '\n') (*end)--;
    if (*end > *start && text[*end - 1] == '\r') (*end)--;
}

static BOOL ml_answer( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *ret )
{
    struct ml_native *nat = ml_native( ctl );
    WCHAR *text, *ntext;
    int len, nlen, line, start, end, n;
    UINT32 hash;

    if (!nat || nat->nlines <= 0 || nat->ntops != nat->nlines) return FALSE;
    text = window_text( ctl->hwnd, &len );
    ntext = native_text( text, len, &nlen, &hash );
    HeapFree( GetProcessHeap(), 0, ntext );
    if (nlen != nat->len || hash != nat->hash)
    {
        /* the native view hasn't caught up with the Win32 text yet */
        HeapFree( GetProcessHeap(), 0, text );
        return FALSE;
    }

    switch (msg)
    {
    case EM_GETSEL:
        start = win_offset( text, len, nat->sel[0] );
        end = win_offset( text, len, nat->sel[1] );
        if (wparam) *(DWORD *)wparam = start;
        if (lparam) *(DWORD *)lparam = end;
        *ret = (start > 0xffff || end > 0xffff) ? -1 : MAKELONG( start, end );
        break;
    case EM_LINEFROMCHAR:
        n = (INT)wparam == -1 ? nat->sel[0] : native_offset( text, len, (INT)wparam );
        *ret = ml_line_of( nat, n );
        break;
    case EM_LINEINDEX:
        line = (INT)wparam == -1 ? ml_line_of( nat, nat->caret_char ) : (INT)wparam;
        *ret = line >= 0 && line < nat->nlines ? win_offset( text, len, nat->lines[line] ) : -1;
        break;
    case EM_GETLINECOUNT:
        *ret = nat->nlines;
        break;
    case EM_LINELENGTH:
        if ((INT)wparam == -1)
        {
            /* the unselected characters on the lines the selection touches */
            int s0, e0, s1, e1, sel0 = win_offset( text, len, nat->sel[0] ), sel1 = win_offset( text, len, nat->sel[1] );
            ml_line_range( nat, text, len, ml_line_of( nat, nat->sel[0] ), &s0, &e0 );
            ml_line_range( nat, text, len, ml_line_of( nat, nat->sel[1] ), &s1, &e1 );
            *ret = max( 0, sel0 - s0 ) + max( 0, e1 - sel1 );
        }
        else
        {
            ml_line_range( nat, text, len, ml_line_of( nat, native_offset( text, len, (INT)wparam ) ), &start, &end );
            *ret = end - start;
        }
        break;
    case EM_GETFIRSTVISIBLELINE:
        *ret = nat->first;
        break;
    case EM_GETLINE:
        line = (INT)wparam;
        if (line < 0 || line >= nat->nlines || !lparam) *ret = 0;
        else
        {
            WORD size = *(WORD *)lparam;
            ml_line_range( nat, text, len, line, &start, &end );
            n = min( end - start, (int)size );
            memcpy( (WCHAR *)lparam, text + start, n * sizeof(WCHAR) );
            *ret = n;
        }
        break;
    case EM_POSFROMCHAR:
    {
        int x, y;
        if ((INT)wparam < 0 || (INT)wparam > len) { *ret = -1; break; }
        n = native_offset( text, len, (INT)wparam );
        line = ml_line_of( nat, n );
        y = nat->tops[line];
        /* exact at the caret and at line starts; elsewhere the average advance */
        if (n == nat->caret_char) { x = nat->caret[0]; y = nat->caret[1]; }
        else x = nat->left + (n - nat->lines[line]) * nat->avg;
        *ret = MAKELONG( (SHORT)x, (SHORT)y );
        break;
    }
    case EM_CHARFROMPOS:
    {
        int x = (SHORT)LOWORD( lparam ), y = (SHORT)HIWORD( lparam ), col;
        for (line = 0; line + 1 < nat->nlines && nat->tops[line + 1] <= y; line++) ;
        ml_line_range( nat, text, len, line, &start, &end );
        col = max( 0, (x - nat->left + nat->avg / 2) / nat->avg );
        start = min( start + col, end );
        *ret = MAKELONG( start, line );
        break;
    }
    default:
        HeapFree( GetProcessHeap(), 0, text );
        return FALSE;
    }
    HeapFree( GetProcessHeap(), 0, text );
    return TRUE;
}

static void ml_snapshot( struct w2s_control *ctl, struct json *j )
{
    struct ml_data *data = ml_data( ctl );
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE ), sel_start = 0, sel_end = 0;
    WCHAR *text, *ntext, parent_class[64] = { 0 };
    int len, nlen;
    const char *align = (style & ES_CENTER) ? "center" : (style & ES_RIGHT) ? "trailing" : "leading";

    text = window_text( ctl->hwnd, &len );
    ntext = native_text( text, len, &nlen, NULL );
    json_str( j, "text", ntext );
    SendMessageW( ctl->hwnd, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end );   /* the Win32 edit's own */
    json_arr_begin( j, "sel" );
    json_int( j, NULL, native_offset( text, len, sel_start ) );
    json_int( j, NULL, native_offset( text, len, sel_end ) );
    json_arr_end( j );
    HeapFree( GetProcessHeap(), 0, ntext );
    HeapFree( GetProcessHeap(), 0, text );

    GetClassNameW( GetParent( ctl->hwnd ), parent_class, ARRAYSIZE(parent_class) );
    json_bool( j, "readonly", (style & ES_READONLY) != 0 );
    /* Return only goes to the default button in a dialog, without ES_WANTRETURN */
    json_bool( j, "wantReturn", (style & ES_WANTRETURN) || wcscmp( parent_class, L"#32770" ) );
    json_bool( j, "wrap", !(style & (ES_AUTOHSCROLL | WS_HSCROLL)) );
    json_int( j, "limit", (UINT)SendMessageW( ctl->hwnd, EM_GETLIMITTEXT, 0, 0 ) );
    json_str_a( j, "align", align );
    json_int( j, "caretGen", data->caret_gen );
    json_int( j, "scrollGen", data->scroll_gen );
    json_int( j, "scrollLine", data->scroll_line );
}

static void ml_observe( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct ml_data *data;

    if (msg != EM_SCROLLCARET && msg != EM_LINESCROLL) return;
    data = ml_data( ctl );
    if (msg == EM_SCROLLCARET) data->caret_gen++;
    else
    {
        struct ml_native *nat = ml_native( ctl );
        data->scroll_line = max( 0, (nat ? nat->first : 0) + (int)lparam );
        data->scroll_gen++;
    }
}

/* the native view reports each edit as a replacement of a range, as typing does */
static void ml_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    WCHAR *text;
    int len;

    if (!strcmp( ev->type, "return" ))
    {
        HWND dlg = GetParent( ctl->hwnd ), button;
        DWORD def = (DWORD)SendMessageW( dlg, DM_GETDEFID, 0, 0 );

        /* what IsDialogMessage does with Return */
        if (HIWORD( def ) == DC_HASDEFID && (button = GetDlgItem( dlg, LOWORD( def ) )) && IsWindowEnabled( button ))
            SendMessageW( dlg, WM_COMMAND, MAKEWPARAM( LOWORD( def ), BN_CLICKED ), (LPARAM)button );
        else
            SendMessageW( dlg, WM_COMMAND, IDOK, (LPARAM)GetDlgItem( dlg, IDOK ) );
        return;
    }
    if (ev->array_count != 2) return;
    text = window_text( ctl->hwnd, &len );
    if (!strcmp( ev->type, "replace" ) && ev->string)
    {
        WCHAR *insert = crlf_text( ev->string );
        int start = win_offset( text, len, ev->array[0] );
        int end = win_offset( text, len, ev->array[0] + ev->array[1] );
        SendMessageW( ctl->hwnd, EM_SETSEL, start, end );
        SendMessageW( ctl->hwnd, EM_REPLACESEL, TRUE, (LPARAM)insert );
        HeapFree( GetProcessHeap(), 0, insert );
    }
    else if (!strcmp( ev->type, "sel" ))
        SendMessageW( ctl->hwnd, EM_SETSEL, win_offset( text, len, ev->array[0] ), win_offset( text, len, ev->array[1] ) );
    HeapFree( GetProcessHeap(), 0, text );
}

static const struct w2s_kind kind_edit_multiline =
    { "edit.multiline", ml_snapshot, ml_apply, ml_answer, NULL, NULL, ml_observe, ml_release, W2S_OWN_TEXT };

/* ---------- ComboBox ---------- */

static void combo_snapshot( struct w2s_control *ctl, struct json *j )
{
    json_text_list( j, "items", ctl->hwnd, CB_GETCOUNT, CB_GETLBTEXTLEN, CB_GETLBTEXT, 2000 );
    json_int( j, "selection", SendMessageW( ctl->hwnd, CB_GETCURSEL, 0, 0 ) );
}

static void combo_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (strcmp( ev->type, "select" ) || !ev->has_value) return;
    if ((int)SendMessageW( ctl->hwnd, CB_GETCURSEL, 0, 0 ) == (int)ev->value) return;
    SendMessageW( ctl->hwnd, CB_SETCURSEL, (int)ev->value, 0 );
    w2s_notify_parent_command( ctl->hwnd, CBN_SELCHANGE );
    w2s_notify_parent_command( ctl->hwnd, CBN_SELENDOK );
}

static const struct w2s_kind kind_combobox_dropdownlist = { "combobox.dropdownlist", combo_snapshot, combo_apply };

/* CBS_DROPDOWN: typing goes into the combo box's own edit part, so the combo
 * box raises CBN_EDITUPDATE/CBN_EDITCHANGE and matches its list as usual */
static HWND combo_edit( HWND combo )
{
    COMBOBOXINFO info = { sizeof(info) };
    return GetComboBoxInfo( combo, &info ) ? info.hwndItem : NULL;
}

static void editable_combo_snapshot( struct w2s_control *ctl, struct json *j )
{
    HWND edit = combo_edit( ctl->hwnd );

    combo_snapshot( ctl, j );
    json_int( j, "limit", edit ? (UINT)SendMessageW( edit, EM_GETLIMITTEXT, 0, 0 ) : 0 );
}

static void editable_combo_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    HWND edit;

    if (!strcmp( ev->type, "text" ) && ev->string)
    {
        if ((edit = combo_edit( ctl->hwnd ))) replace_text( edit, ev->string );
    }
    else if (!strcmp( ev->type, "open" )) w2s_notify_parent_command( ctl->hwnd, CBN_DROPDOWN );
    else if (!strcmp( ev->type, "close" )) w2s_notify_parent_command( ctl->hwnd, CBN_CLOSEUP );
    else combo_apply( ctl, ev );
}

static const struct w2s_kind kind_combobox_editable = { "combobox.editable", editable_combo_snapshot, editable_combo_apply };

/* ---------- ComboBoxEx (map: comboboxex) ---------- */

/* A ComboBoxEx is a combo box and an edit it owns: the native pop-up (or
 * editable combo) stands in for both, and native choices go through its own
 * combo box part, as a pick in the list does, so the ComboBoxEx updates its edit
 * and tells the app (CBN_SELENDOK). Typing goes into its edit, and Return
 * there (wine keeps dialog keys) ends the edit with CBEN_ENDEDIT. */
static void comboex_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    HWND edit = (HWND)SendMessageW( ctl->hwnd, CBEM_GETEDITCONTROL, 0, 0 );
    int count = min( (int)SendMessageW( ctl->hwnd, CB_GETCOUNT, 0, 0 ), 2000 ), i, sel, len;
    WCHAR text[256], *edit_text;

    json_bool( j, "editable", (style & 3) != CBS_DROPDOWNLIST );
    json_arr_begin( j, "items" );
    for (i = 0; i < count; i++)
    {
        COMBOBOXEXITEMW item;
        memset( &item, 0, sizeof(item) );
        item.mask = CBEIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        item.cchTextMax = ARRAYSIZE(text);
        text[0] = 0;
        SendMessageW( ctl->hwnd, CBEM_GETITEMW, 0, (LPARAM)&item );
        json_str( j, NULL, text );
    }
    json_arr_end( j );
    sel = (int)SendMessageW( ctl->hwnd, CB_GETCURSEL, 0, 0 );
    json_int( j, "selection", sel );
    if (edit && (edit_text = window_text( edit, &len )))
    {
        json_str( j, "text", edit_text );
        HeapFree( GetProcessHeap(), 0, edit_text );
        json_int( j, "limit", (UINT)SendMessageW( edit, EM_GETLIMITTEXT, 0, 0 ) );
    }
    else json_str( j, "text", L"" );
}

static void comboex_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    HWND combo = (HWND)SendMessageW( ctl->hwnd, CBEM_GETCOMBOCONTROL, 0, 0 );
    HWND edit = (HWND)SendMessageW( ctl->hwnd, CBEM_GETEDITCONTROL, 0, 0 );
    int id = combo ? GetDlgCtrlID( combo ) : 0;

    if (!strcmp( ev->type, "select" ) && ev->has_value && combo)
    {
        if ((int)SendMessageW( ctl->hwnd, CB_GETCURSEL, 0, 0 ) == (int)ev->value) return;
        SendMessageW( combo, CB_SETCURSEL, (int)ev->value, 0 );
        SendMessageW( ctl->hwnd, WM_COMMAND, MAKEWPARAM( id, CBN_SELCHANGE ), (LPARAM)combo );
        SendMessageW( ctl->hwnd, WM_COMMAND, MAKEWPARAM( id, CBN_SELENDOK ), (LPARAM)combo );
    }
    else if (!strcmp( ev->type, "text" ) && ev->string && edit) replace_text( edit, ev->string );
    else if (!strcmp( ev->type, "open" ) && combo)
        SendMessageW( ctl->hwnd, WM_COMMAND, MAKEWPARAM( id, CBN_DROPDOWN ), (LPARAM)combo );
    else if (!strcmp( ev->type, "close" ) && combo)
        SendMessageW( ctl->hwnd, WM_COMMAND, MAKEWPARAM( id, CBN_CLOSEUP ), (LPARAM)combo );
}

static const struct w2s_kind kind_comboboxex =
    { "comboboxex", comboex_snapshot, comboex_apply, NULL, NULL, NULL, NULL, NULL, W2S_OWN_TEXT };

/* ---------- SysLink (map: syslink) ---------- */

/* The text keeps its <a href="..." id="...">markup</a> (WM_GETTEXT is the
 * window text): the native view makes links of it. A native click on link n
 * tells the parent what comctl32 tells it (NM_CLICK with the link's id and
 * URL), so the app opens the link itself, as on Windows. */
static void syslink_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    NMLINK nml;
    LITEM item;

    if (strcmp( ev->type, "link" ) || !ev->has_value) return;
    memset( &item, 0, sizeof(item) );
    item.mask = LIF_ITEMINDEX | LIF_ITEMID | LIF_URL;
    item.iLink = (int)ev->value;
    if (!SendMessageW( ctl->hwnd, LM_GETITEM, 0, (LPARAM)&item )) return;
    memset( &nml, 0, sizeof(nml) );
    nml.hdr.hwndFrom = ctl->hwnd;
    nml.hdr.idFrom = GetWindowLongPtrW( ctl->hwnd, GWLP_ID );
    nml.hdr.code = NM_CLICK;
    nml.item.iLink = item.iLink;
    lstrcpyW( nml.item.szID, item.szID );
    lstrcpyW( nml.item.szUrl, item.szUrl );
    SendMessageW( GetParent( ctl->hwnd ), WM_NOTIFY, nml.hdr.idFrom, (LPARAM)&nml );
}

static const struct w2s_kind kind_syslink = { "syslink", nothing_snapshot, syslink_apply };

/* ---------- ListBox ---------- */

static void listbox_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );

    json_text_list( j, "items", ctl->hwnd, LB_GETCOUNT, LB_GETTEXTLEN, LB_GETTEXT, 5000 );
    if (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL))
    {
        int count = (int)SendMessageW( ctl->hwnd, LB_GETSELCOUNT, 0, 0 ), i;
        int *sel = count > 0 ? HeapAlloc( GetProcessHeap(), 0, count * sizeof(int) ) : NULL;
        if (sel) count = (int)SendMessageW( ctl->hwnd, LB_GETSELITEMS, count, (LPARAM)sel );
        json_arr_begin( j, "selections" );
        for (i = 0; sel && i < count; i++) json_int( j, NULL, sel[i] );
        json_arr_end( j );
        if (sel) HeapFree( GetProcessHeap(), 0, sel );
    }
    else json_int( j, "selection", SendMessageW( ctl->hwnd, LB_GETCURSEL, 0, 0 ) );
}

static void listbox_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    BOOL notify = (style & LBS_NOTIFY) != 0;

    if (!strcmp( ev->type, "select" ) && ev->has_value)
    {
        SendMessageW( ctl->hwnd, LB_SETCURSEL, (int)ev->value, 0 );
        if (notify) w2s_notify_parent_command( ctl->hwnd, LBN_SELCHANGE );
    }
    else if (!strcmp( ev->type, "selectMany" ))
    {
        int i;
        SendMessageW( ctl->hwnd, LB_SETSEL, FALSE, -1 );
        for (i = 0; i < ev->array_count; i++) SendMessageW( ctl->hwnd, LB_SETSEL, TRUE, ev->array[i] );
        if (ev->array_count) SendMessageW( ctl->hwnd, LB_SETCARETINDEX, ev->array[ev->array_count - 1], FALSE );
        if (notify) w2s_notify_parent_command( ctl->hwnd, LBN_SELCHANGE );
    }
    else if (!strcmp( ev->type, "activate" ) && ev->has_value)
    {
        SendMessageW( ctl->hwnd, LB_SETCURSEL, (int)ev->value, 0 );
        if (notify) w2s_notify_parent_command( ctl->hwnd, LBN_DBLCLK );
    }
}

static const struct w2s_kind kind_listbox_single = { "listbox.single", listbox_snapshot, listbox_apply };
static const struct w2s_kind kind_listbox_multi = { "listbox.multi", listbox_snapshot, listbox_apply };

/* ---------- ListView (list and report modes) ---------- */

#define MAX_LV_COLUMNS 64

/* the report columns in display order, with the subitem each one shows; none
 * until the app inserts the first one */
static int listview_columns( HWND hwnd, int *subitems, struct json *j )
{
    HWND header = (HWND)SendMessageW( hwnd, LVM_GETHEADER, 0, 0 );
    int n = header ? (int)SendMessageW( header, HDM_GETITEMCOUNT, 0, 0 ) : 0, order[MAX_LV_COLUMNS], c;
    WCHAR text[512];

    n = max( 0, min( n, MAX_LV_COLUMNS ) );
    if (!n || !SendMessageW( hwnd, LVM_GETCOLUMNORDERARRAY, n, (LPARAM)order ))
        for (c = 0; c < n; c++) order[c] = c;
    if (j) json_arr_begin( j, "columns" );
    for (c = 0; c < n; c++)
    {
        LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
        text[0] = 0;
        col.pszText = text;
        col.cchTextMax = ARRAYSIZE(text);
        SendMessageW( hwnd, LVM_GETCOLUMNW, order[c], (LPARAM)&col );
        /* column k shows subitem k, as the list view draws it; LVCOLUMN.iSubItem
         * is only what the app stored (0 when it never set LVCF_SUBITEM) */
        subitems[c] = order[c];
        if (!j) continue;
        json_obj_begin( j );
        json_str( j, "title", text );
        json_int( j, "width", col.cx );
        json_int( j, "index", order[c] );
        json_str_a( j, "align", (col.fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_RIGHT ? "trailing" :
                                (col.fmt & LVCFMT_JUSTIFYMASK) == LVCFMT_CENTER ? "center" : "leading" );
        json_obj_end( j );
    }
    if (j) json_arr_end( j );
    return n;
}

/* Icons go once per image list: the native view keeps those it was sent
 * until imageGen changes (a new image list or icon size). */
#define MAX_LV_IMAGES 4096

struct lv_data
{
    HIMAGELIST himl;
    int cx, cy, gen;
    BOOL more;                      /* images left for the next snapshot */
    BYTE sent[MAX_LV_IMAGES / 8];
};

/* An image list's images, each sent once: the native view keeps them until
 * imageGen changes (another image list or icon size). Pixels, or for wine's
 * stock icons the macOS image with the same meaning (icons.c). indices: the
 * images the view shows now. */
static void image_list_json( struct lv_data *data, HIMAGELIST himl, const int *indices, int count, struct json *j )
{
    int cx = 0, cy = 0, i, fresh = 0, spec_index[64];
    const char *specs[64];
    char key[16];

    if (himl) ImageList_GetIconSize( himl, &cx, &cy );
    if (himl != data->himl || cx != data->cx || cy != data->cy)
    {
        data->himl = himl;
        data->cx = cx;
        data->cy = cy;
        data->gen++;
        memset( data->sent, 0, sizeof(data->sent) );
    }
    json_int( j, "imageGen", data->gen );
    json_arr_begin( j, "imageSize" );
    json_int( j, NULL, cx );
    json_int( j, NULL, cy );
    json_arr_end( j );
    if (!himl || cx <= 0 || cy <= 0 || cx > 256 || cy > 256) return;

    json_key_obj_begin( j, "images" );
    for (i = 0; i < count && fresh < 64; i++)
    {
        int index = indices[i];
        HICON icon;
        BYTE *bits;

        if (index < 0 || index >= MAX_LV_IMAGES || (data->sent[index / 8] & (1 << (index % 8)))) continue;
        data->sent[index / 8] |= 1 << (index % 8);
        if (!(icon = ImageList_GetIcon( himl, index, ILD_NORMAL ))) continue;
        specs[fresh] = NULL;
        if ((bits = w2s_image_bgra( icon, NULL, cx, cy )))
        {
            if ((specs[fresh] = w2s_stock_icon( NULL, bits, cx, cy, himl )))
                spec_index[fresh] = index;
            else
            {
                snprintf( key, sizeof(key), "%d", index );
                json_base64( j, key, bits, (size_t)cx * cy * 4 );
            }
            HeapFree( GetProcessHeap(), 0, bits );
        }
        DestroyIcon( icon );
        if (++fresh == 64)
        {
            /* enough for one snapshot: the rest come with the next one (the view's
             * control gets the refresh; data->sent keeps what already went) */
            break;
        }
    }
    json_obj_end( j );
    json_key_obj_begin( j, "symbols" );
    for (i = 0; i < fresh; i++)
    {
        if (!specs[i]) continue;
        snprintf( key, sizeof(key), "%d", spec_index[i] );
        json_str_a( j, key, specs[i] );
    }
    json_obj_end( j );
    data->more = (fresh == 64);
}

static void listview_icons( struct w2s_control *ctl, struct json *j, BOOL small, int count )
{
    struct lv_data *data = ctl->data;
    HIMAGELIST himl = (HIMAGELIST)SendMessageW( ctl->hwnd, LVM_GETIMAGELIST, small ? LVSIL_SMALL : LVSIL_NORMAL, 0 );
    int i, *indices;

    if (!data) data = ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) );
    json_bool( j, "small", small );
    count = min( count, 2000 );
    indices = HeapAlloc( GetProcessHeap(), 0, max( count, 1 ) * sizeof(int) );
    json_arr_begin( j, "icons" );
    for (i = 0; i < count; i++)
    {
        LVITEMW item = { LVIF_IMAGE };
        item.iItem = i;
        item.iImage = -1;
        SendMessageW( ctl->hwnd, LVM_GETITEMW, 0, (LPARAM)&item );
        indices[i] = himl ? item.iImage : -1;
        json_int( j, NULL, indices[i] );
    }
    json_arr_end( j );
    image_list_json( data, himl, indices, count, j );
    HeapFree( GetProcessHeap(), 0, indices );
    if (data->more) PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
}

static void listview_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD view = (DWORD)SendMessageW( ctl->hwnd, LVM_GETVIEW, 0, 0 );
    DWORD ex = (DWORD)SendMessageW( ctl->hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0 );
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    int subitems[MAX_LV_COLUMNS] = { 0 }, columns = 1;
    int count = (int)SendMessageW( ctl->hwnd, LVM_GETITEMCOUNT, 0, 0 ), i, c;
    WCHAR text[512];

    json_bool( j, "report", view == LV_VIEW_DETAILS );
    if (view == LV_VIEW_DETAILS)
        columns = max( 1, listview_columns( ctl->hwnd, subitems, j ) );
    json_arr_begin( j, "rows" );
    for (i = 0; i < count && i < 2000; i++)
    {
        json_arr_begin( j, NULL );
        for (c = 0; c < columns; c++)
        {
            LVITEMW item = { 0 };
            text[0] = 0;
            item.iSubItem = subitems[c];
            item.pszText = text;
            item.cchTextMax = ARRAYSIZE(text);
            SendMessageW( ctl->hwnd, LVM_GETITEMTEXTW, i, (LPARAM)&item );
            json_str( j, NULL, text );
        }
        json_arr_end( j );
    }
    json_arr_end( j );
    json_arr_begin( j, "selections" );
    for (i = -1; (i = (int)SendMessageW( ctl->hwnd, LVM_GETNEXTITEM, i, LVNI_SELECTED )) != -1;)
        json_int( j, NULL, i );
    json_arr_end( j );
    json_bool( j, "single", (style & LVS_SINGLESEL) != 0 );
    json_bool( j, "noHeader", (style & LVS_NOCOLUMNHEADER) != 0 );
    json_bool( j, "sortHeader", !(style & LVS_NOSORTHEADER) );
    if (ex & LVS_EX_CHECKBOXES)
    {
        json_arr_begin( j, "checks" );
        for (i = 0; i < count && i < 2000; i++)
            json_bool( j, NULL, (SendMessageW( ctl->hwnd, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK ) >> 12) == 2 );
        json_arr_end( j );
    }
    if (view == LV_VIEW_ICON || view == LV_VIEW_SMALLICON)
        listview_icons( ctl, j, view == LV_VIEW_SMALLICON, count );
}

static void listview_select( HWND hwnd, const int *items, int count )
{
    LVITEMW item = { 0 };
    int i;

    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = 0;
    SendMessageW( hwnd, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&item );
    for (i = 0; i < count; i++)
    {
        item.state = LVIS_SELECTED | (i == count - 1 ? LVIS_FOCUSED : 0);
        SendMessageW( hwnd, LVM_SETITEMSTATE, items[i], (LPARAM)&item );
    }
}

static void listview_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (!strcmp( ev->type, "selectMany" )) listview_select( ctl->hwnd, ev->array, ev->array_count );
    else if (!strcmp( ev->type, "select" ) && ev->has_value)
    {
        int i = (int)ev->value;
        listview_select( ctl->hwnd, &i, i >= 0 ? 1 : 0 );
    }
    else if (!strcmp( ev->type, "activate" ) && ev->has_value)
    {
        NMITEMACTIVATE nm = { { 0 } };
        int i = (int)ev->value;
        listview_select( ctl->hwnd, &i, 1 );
        nm.iItem = i;
        w2s_notify_parent( ctl->hwnd, NM_DBLCLK, &nm.hdr );
        w2s_notify_parent( ctl->hwnd, LVN_ITEMACTIVATE, &nm.hdr );
    }
    else if (!strcmp( ev->type, "column" ) && ev->has_value)
    {
        NMLISTVIEW nm = { { 0 } };
        nm.iItem = -1;
        nm.iSubItem = (int)ev->value;
        w2s_notify_parent( ctl->hwnd, LVN_COLUMNCLICK, &nm.hdr );
    }
    else if (!strcmp( ev->type, "check" ) && ev->array_count == 2)
    {
        /* the state image, as a click on the box sets it: LVN_ITEMCHANGING/CHANGED follow */
        LVITEMW item = { 0 };
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK( ev->array[1] ? 2 : 1 );
        SendMessageW( ctl->hwnd, LVM_SETITEMSTATE, ev->array[0], (LPARAM)&item );
    }
}

static const struct w2s_kind kind_listview_list = { "listview.list", listview_snapshot, listview_apply };
static const struct w2s_kind kind_listview_report = { "listview.report", listview_snapshot, listview_apply };
static const struct w2s_kind kind_listview_checkboxes = { "listview.checkboxes", listview_snapshot, listview_apply };
static const struct w2s_kind kind_listview_icon = { "listview.icon", listview_snapshot, listview_apply };

/* ---------- TreeView ---------- */

/* Each node carries its HTREEITEM as id (stable while it exists; the tree
 * control checks handles it is given). Children are listed only under open
 * nodes, the others just say whether they have any: apps fill a node's
 * children on TVN_ITEMEXPANDING, so the native view asks for an expansion
 * before the children exist. */
#define MAX_TREE_NODES 5000
#define MAX_TREE_DEPTH 50

static void tree_nodes( HWND hwnd, HTREEITEM item, struct json *j, int depth, int *budget,
                        int *images, int *images_count )
{
    for (; item && *budget > 0; item = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)item ))
    {
        TVITEMW it = { TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN | TVIF_HANDLE | TVIF_IMAGE };
        WCHAR text[260];
        HTREEITEM child;
        BOOL open;

        text[0] = 0;
        it.hItem = item;
        it.pszText = text;
        it.cchTextMax = ARRAYSIZE(text);
        it.stateMask = TVIS_EXPANDED;
        it.iImage = -1;
        SendMessageW( hwnd, TVM_GETITEMW, 0, (LPARAM)&it );
        if (it.pszText != text) lstrcpynW( text, it.pszText && it.pszText != LPSTR_TEXTCALLBACKW ? it.pszText : L"", ARRAYSIZE(text) );
        (*budget)--;
        child = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)item );
        open = (it.state & TVIS_EXPANDED) && child && depth < MAX_TREE_DEPTH;

        json_obj_begin( j );
        json_int( j, "id", (INT_PTR)item );
        json_str( j, "text", text );
        json_bool( j, "kids", child || it.cChildren > 0 );
        if (it.iImage >= 0 && it.iImage != I_IMAGECALLBACK && images && *images_count < MAX_TREE_NODES)
        {
            json_int( j, "img", it.iImage );
            images[(*images_count)++] = it.iImage;
        }
        if (open)
        {
            json_bool( j, "open", TRUE );
            json_arr_begin( j, "children" );
            tree_nodes( hwnd, child, j, depth + 1, budget, images, images_count );
            json_arr_end( j );
        }
        json_obj_end( j );
    }
}

static void tree_snapshot( struct w2s_control *ctl, struct json *j )
{
    HWND parent = GetParent( ctl->hwnd );
    WCHAR parent_class[64] = { 0 };
    int budget = MAX_TREE_NODES;
    RECT rc;

    HIMAGELIST himl = (HIMAGELIST)SendMessageW( ctl->hwnd, TVM_GETIMAGELIST, TVSIL_NORMAL, 0 );
    struct lv_data *data = ctl->data;
    int *images = NULL, images_count = 0;

    if (!data) data = ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) );
    if (himl) images = HeapAlloc( GetProcessHeap(), 0, MAX_TREE_NODES * sizeof(int) );
    json_arr_begin( j, "nodes" );
    tree_nodes( ctl->hwnd, (HTREEITEM)SendMessageW( ctl->hwnd, TVM_GETNEXTITEM, TVGN_ROOT, 0 ), j, 0, &budget,
                images, &images_count );
    json_arr_end( j );
    /* the nodes' icons: wine's folders and drives come as the Finder's (icons.c) */
    image_list_json( data, himl, images, images_count, j );
    HeapFree( GetProcessHeap(), 0, images );
    if (data->more) PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    json_int( j, "selection", (INT_PTR)SendMessageW( ctl->hwnd, TVM_GETNEXTITEM, TVGN_CARET, 0 ) );
    /* a tree along the left edge of a window is a sidebar (Finder's); in a dialog, a bordered list */
    GetClassNameW( parent, parent_class, ARRAYSIZE(parent_class) );
    GetWindowRect( ctl->hwnd, &rc );
    MapWindowPoints( NULL, parent, (POINT *)&rc, 2 );
    json_bool( j, "sidebar", wcscmp( parent_class, L"#32770" ) && rc.left <= 1 );
}

/* what a click on the node's button does: notify, let the app veto or fill
 * the children, expand, notify */
static void tree_expand( HWND hwnd, HTREEITEM item, BOOL expand )
{
    BOOL unicode = (BOOL)SendMessageW( hwnd, TVM_GETUNICODEFORMAT, 0, 0 );
    TVITEMW it = { TVIF_HANDLE | TVIF_STATE | TVIF_CHILDREN | TVIF_PARAM };
    NMTREEVIEWW nm = { { 0 } };

    it.hItem = item;
    it.stateMask = TVIS_EXPANDED | TVIS_EXPANDEDONCE;
    if (!SendMessageW( hwnd, TVM_GETITEMW, 0, (LPARAM)&it )) return;
    if (!!(it.state & TVIS_EXPANDED) == !!expand) return;
    if (expand && it.cChildren && !(it.state & TVIS_EXPANDEDONCE))
    {
        /* TVM_EXPAND notifies on a first expansion by itself, as a click does */
        SendMessageW( hwnd, TVM_EXPAND, TVE_EXPAND, (LPARAM)item );
        return;
    }
    nm.action = expand ? TVE_EXPAND : TVE_COLLAPSE;
    nm.itemNew.mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM;
    nm.itemNew.hItem = item;
    nm.itemNew.state = it.state;
    nm.itemNew.stateMask = it.stateMask;
    nm.itemNew.lParam = it.lParam;
    if (w2s_notify_parent( hwnd, unicode ? TVN_ITEMEXPANDINGW : TVN_ITEMEXPANDINGA, &nm.hdr )) return; /* vetoed */
    SendMessageW( hwnd, TVM_EXPAND, expand ? TVE_EXPAND : TVE_COLLAPSE, (LPARAM)item );
    nm.itemNew.state ^= TVIS_EXPANDED;
    w2s_notify_parent( hwnd, unicode ? TVN_ITEMEXPANDEDW : TVN_ITEMEXPANDEDA, &nm.hdr );
}

static void tree_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    HTREEITEM item = (HTREEITEM)(INT_PTR)(INT64)ev->value;

    if (!ev->has_value) return;
    if (!strcmp( ev->type, "select" ))
    {
        if (item != (HTREEITEM)SendMessageW( ctl->hwnd, TVM_GETNEXTITEM, TVGN_CARET, 0 ))
            SendMessageW( ctl->hwnd, TVM_SELECTITEM, TVGN_CARET, (LPARAM)item );
    }
    else if (!strcmp( ev->type, "expand" )) tree_expand( ctl->hwnd, item, TRUE );
    else if (!strcmp( ev->type, "collapse" )) tree_expand( ctl->hwnd, item, FALSE );
    else if (!strcmp( ev->type, "activate" ))
    {
        NMHDR nm = { 0 };
        TVITEMW it = { TVIF_HANDLE | TVIF_STATE | TVIF_CHILDREN };

        SendMessageW( ctl->hwnd, TVM_SELECTITEM, TVGN_CARET, (LPARAM)item );
        /* unless the app handles the double click, it toggles the node */
        if (w2s_notify_parent( ctl->hwnd, NM_DBLCLK, &nm )) return;
        it.hItem = item;
        it.stateMask = TVIS_EXPANDED;
        if (SendMessageW( ctl->hwnd, TVM_GETITEMW, 0, (LPARAM)&it ) && it.cChildren)
            tree_expand( ctl->hwnd, item, !(it.state & TVIS_EXPANDED) );
    }
}

static const struct w2s_kind kind_treeview = { "treeview", tree_snapshot, tree_apply };

/* ---------- Progress, trackbar ---------- */

static void progress_snapshot( struct w2s_control *ctl, struct json *j )
{
    PBRANGE range;
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );

    SendMessageW( ctl->hwnd, PBM_GETRANGE, FALSE, (LPARAM)&range );
    json_int( j, "value", SendMessageW( ctl->hwnd, PBM_GETPOS, 0, 0 ) );
    json_int( j, "min", range.iLow );
    json_int( j, "max", range.iHigh );
    json_bool( j, "marquee", (style & PBS_MARQUEE) != 0 );
    json_int( j, "state", SendMessageW( ctl->hwnd, PBM_GETSTATE, 0, 0 ) );
}

static const struct w2s_kind kind_progress = { "progress", progress_snapshot, nothing_apply };

static void trackbar_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    json_int( j, "value", SendMessageW( ctl->hwnd, TBM_GETPOS, 0, 0 ) );
    json_int( j, "min", SendMessageW( ctl->hwnd, TBM_GETRANGEMIN, 0, 0 ) );
    json_int( j, "max", SendMessageW( ctl->hwnd, TBM_GETRANGEMAX, 0, 0 ) );
    json_int( j, "ticks", (style & TBS_NOTICKS) ? 0 : SendMessageW( ctl->hwnd, TBM_GETNUMTICS, 0, 0 ) );
    /* a vertical one's ticks: on the right unless TBS_LEFT (TBS_BOTH: the right, one side on macOS) */
    if (style & TBS_VERT) json_str_a( j, "tickSide", (style & (TBS_LEFT | TBS_BOTH)) == TBS_LEFT ? "leading" : "trailing" );
}

/* a native drag: TB_THUMBTRACK as it moves; at the end TB_THUMBPOSITION and
 * TB_ENDTRACK. WM_VSCROLL for a vertical trackbar. */
static void trackbar_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    HWND parent = GetParent( ctl->hwnd );
    UINT msg = (GetWindowLongW( ctl->hwnd, GWL_STYLE ) & TBS_VERT) ? WM_VSCROLL : WM_HSCROLL;
    int pos = (int)ev->value;

    if (!ev->has_value) return;
    if (!strcmp( ev->type, "value" ))
    {
        SendMessageW( ctl->hwnd, TBM_SETPOS, TRUE, pos );
        SendMessageW( parent, msg, MAKEWPARAM( TB_THUMBTRACK, pos ), (LPARAM)ctl->hwnd );
    }
    else if (!strcmp( ev->type, "valueEnd" ))
    {
        SendMessageW( ctl->hwnd, TBM_SETPOS, TRUE, pos );
        SendMessageW( parent, msg, MAKEWPARAM( TB_THUMBPOSITION, pos ), (LPARAM)ctl->hwnd );
        SendMessageW( parent, msg, MAKEWPARAM( TB_ENDTRACK, 0 ), (LPARAM)ctl->hwnd );
    }
}

static const struct w2s_kind kind_trackbar = { "trackbar", trackbar_snapshot, trackbar_apply };
static const struct w2s_kind kind_trackbar_vertical = { "trackbar.vertical", trackbar_snapshot, trackbar_apply };

/* ---------- Up-down, date and time ---------- */

static void updown_snapshot( struct w2s_control *ctl, struct json *j )
{
    int lo = 0, hi = 0;
    BOOL error = FALSE;

    json_int( j, "value", SendMessageW( ctl->hwnd, UDM_GETPOS32, 0, (LPARAM)&error ) );
    SendMessageW( ctl->hwnd, UDM_GETRANGE32, (WPARAM)&lo, (LPARAM)&hi );
    json_int( j, "min", lo );
    json_int( j, "max", hi );
    json_bool( j, "buddy", SendMessageW( ctl->hwnd, UDM_GETBUDDY, 0, 0 ) != 0 );
}

/* A click on that arrow, through the control's own mouse handling: the
 * acceleration, wrapping, reversed ranges, the UDN_DELTAPOS veto, the buddy's
 * text and WM_VSCROLL all stay the up-down control's. */
static void updown_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    BOOL up = ev->value > 0;
    RECT rc;
    LPARAM pt;

    if (strcmp( ev->type, "step" ) || !ev->has_value) return;
    GetClientRect( ctl->hwnd, &rc );
    if (style & UDS_HORZ) pt = MAKELPARAM( up ? rc.right * 3 / 4 : rc.right / 4, rc.bottom / 2 );
    else pt = MAKELPARAM( rc.right / 2, up ? rc.bottom / 4 : rc.bottom * 3 / 4 );
    SendMessageW( ctl->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt );
    SendMessageW( ctl->hwnd, WM_LBUTTONUP, 0, pt );
}

static const struct w2s_kind kind_updown = { "updown", updown_snapshot, updown_apply };

/* dates travel as [year, month, day, hour, minute, second], local and Gregorian like SYSTEMTIME */
static void json_systemtime( struct json *j, const char *key, const SYSTEMTIME *st )
{
    json_arr_begin( j, key );
    json_int( j, NULL, st->wYear );
    json_int( j, NULL, st->wMonth );
    json_int( j, NULL, st->wDay );
    json_int( j, NULL, st->wHour );
    json_int( j, NULL, st->wMinute );
    json_int( j, NULL, st->wSecond );
    json_arr_end( j );
}

static void json_date_range( struct json *j, DWORD which, const SYSTEMTIME *range )
{
    if (which & GDTR_MIN) json_systemtime( j, "dateMin", &range[0] );
    if (which & GDTR_MAX) json_systemtime( j, "dateMax", &range[1] );
}

static void datetime_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    SYSTEMTIME st, range[2];
    BOOL valid = SendMessageW( ctl->hwnd, DTM_GETSYSTEMTIME, 0, (LPARAM)&st ) == GDT_VALID;

    if (!valid) GetLocalTime( &st );    /* DTS_SHOWNONE unchecked: the picker still shows a date */
    json_systemtime( j, "date", &st );
    json_bool( j, "dateValid", valid );
    memset( range, 0, sizeof(range) );
    json_date_range( j, (DWORD)SendMessageW( ctl->hwnd, DTM_GETRANGE, 0, (LPARAM)range ), range );
    json_bool( j, "showNone", (style & DTS_SHOWNONE) != 0 );
    json_bool( j, "timeOnly", (style & DTS_TIMEFORMAT) == DTS_TIMEFORMAT );
    json_bool( j, "upDown", (style & DTS_UPDOWN) != 0 );
}

/* DTM_SETSYSTEMTIME doesn't notify; a user's change does */
static void datetime_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    NMDATETIMECHANGE nm = { { 0 } };
    SYSTEMTIME st;

    if (SendMessageW( ctl->hwnd, DTM_GETSYSTEMTIME, 0, (LPARAM)&st ) != GDT_VALID) GetLocalTime( &st );
    if (!strcmp( ev->type, "none" ))
    {
        if (!(style & DTS_SHOWNONE)) return;
        SendMessageW( ctl->hwnd, DTM_SETSYSTEMTIME, GDT_NONE, (LPARAM)&st );
        nm.dwFlags = GDT_NONE;
    }
    else if (!strcmp( ev->type, "date" ) && ev->array_count >= 3)
    {
        /* the picker shows either the date or the time: the rest stays */
        if ((style & DTS_TIMEFORMAT) == DTS_TIMEFORMAT)
        {
            if (ev->array_count < 6) return;
            st.wHour = ev->array[3];
            st.wMinute = ev->array[4];
            st.wSecond = ev->array[5];
        }
        else
        {
            st.wYear = ev->array[0];
            st.wMonth = ev->array[1];
            st.wDay = ev->array[2];
        }
        st.wMilliseconds = 0;
        if (!SendMessageW( ctl->hwnd, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st )) return;
        SendMessageW( ctl->hwnd, DTM_GETSYSTEMTIME, 0, (LPARAM)&st );     /* with its day of the week */
        nm.dwFlags = GDT_VALID;
    }
    else return;
    nm.st = st;
    w2s_notify_parent( ctl->hwnd, DTN_DATETIMECHANGE, &nm.nmhdr );
}

static const struct w2s_kind kind_datetime = { "datetime", datetime_snapshot, datetime_apply };

static void monthcal_snapshot( struct w2s_control *ctl, struct json *j )
{
    SYSTEMTIME st, range[2];

    if (!SendMessageW( ctl->hwnd, MCM_GETCURSEL, 0, (LPARAM)&st )) GetLocalTime( &st );
    st.wHour = st.wMinute = st.wSecond = 0;
    json_systemtime( j, "date", &st );
    memset( range, 0, sizeof(range) );
    json_date_range( j, (DWORD)SendMessageW( ctl->hwnd, MCM_GETRANGE, 0, (LPARAM)range ), range );
}

/* a click on a day: the selection changes, then it is chosen */
static void monthcal_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    NMSELCHANGE nm = { { 0 } };
    SYSTEMTIME st = { 0 };

    if (strcmp( ev->type, "date" ) || ev->array_count < 3) return;
    st.wYear = ev->array[0];
    st.wMonth = ev->array[1];
    st.wDay = ev->array[2];
    if (!SendMessageW( ctl->hwnd, MCM_SETCURSEL, 0, (LPARAM)&st )) return;
    SendMessageW( ctl->hwnd, MCM_GETCURSEL, 0, (LPARAM)&st );
    nm.stSelStart = nm.stSelEnd = st;
    w2s_notify_parent( ctl->hwnd, MCN_SELCHANGE, &nm.nmhdr );
    w2s_notify_parent( ctl->hwnd, MCN_SELECT, &nm.nmhdr );
}

static const struct w2s_kind kind_monthcal = { "monthcal", monthcal_snapshot, monthcal_apply };

/* ---------- Tab ---------- */

/* A top-level property sheet with more than 5 pages gets a real window sidebar
 * from macOS 13 (the map's propsheet entry): an NSSplitViewController whose
 * sidebar lists the pages, outside wine's content. The sheet's tab control
 * carries it: in this mode TCM_ADJUSTRECT is answered with no change (the
 * page area is the tab control's whole rectangle: the tabs are in the
 * window's sidebar, outside wine's content). The tab control's own in-window
 * view draws nothing and passes clicks through. */
#define IDC_PROPSHEET_TAB 12320     /* comctl32's IDC_TABCONTROL */

struct tab_data
{
    BOOL decided;
    BOOL window;                      /* a real window sidebar, not a strip */
    int width;                        /* sidebar width, points */
};

/* decided at the first layout query (TCM_ADJUSTRECT), when the sheet has all
 * its pages: the tabs arrive one by one before that */
static struct tab_data *tab_layout( struct w2s_control *ctl, BOOL decide )
{
    struct tab_data *data = ctl->data;
    int i, count = (int)SendMessageW( ctl->hwnd, TCM_GETITEMCOUNT, 0, 0 );
    WCHAR parent_class[16] = {0};
    HWND parent = GetParent( ctl->hwnd );
    DWORD style;

    if (!data) data = ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) );
    if (!decide || data->decided || count <= 0) return data;

    data->decided = TRUE;
    GetClassNameW( parent, parent_class, ARRAYSIZE(parent_class) );
    style = GetWindowLongW( parent, GWL_STYLE );
    data->window = count > 5 && w2s_os_major >= 13 && !wcscmp( parent_class, L"#32770" ) &&
                   GetDlgCtrlID( ctl->hwnd ) == IDC_PROPSHEET_TAB &&
                   (style & WS_CAPTION) && !(style & WS_CHILD);
    if (data->window)
    {
        HFONT font = (HFONT)SendMessageW( ctl->hwnd, WM_GETFONT, 0, 0 );
        HDC hdc = GetDC( ctl->hwnd );
        HGDIOBJ old = SelectObject( hdc, font ? font : GetStockObject( DEFAULT_GUI_FONT ) );
        int widest = 0;
        WCHAR text[256];
        SIZE size;

        for (i = 0; i < count; i++)
        {
            TCITEMW item = { TCIF_TEXT };
            text[0] = 0;
            item.pszText = text;
            item.cchTextMax = ARRAYSIZE(text);
            SendMessageW( ctl->hwnd, TCM_GETITEMW, i, (LPARAM)&item );
            if (GetTextExtentPoint32W( hdc, text, wcslen( text ), &size )) widest = max( widest, size.cx );
        }
        SelectObject( hdc, old );
        ReleaseDC( ctl->hwnd, hdc );
        /* plain rows, no icons: the widest title larger than the dialog font
         * (13 pt vs 11 px), with room for the list's insets */
        data->width = min( max( widest * 4 / 3 + 60, 180 ), 320 );
        TRACE( "%p: property sheet with %d pages gets a %d pt window sidebar\n", ctl->hwnd, count, data->width );
    }
    return data;
}

static BOOL tab_answer( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *ret )
{
    struct tab_data *data;
    RECT *rc = (RECT *)lparam;

    if (msg != TCM_ADJUSTRECT || !rc) return FALSE;
    if (!(data = ctl->data) || !data->decided)
    {
        data = tab_layout( ctl, TRUE );
        if (data->window) w2s_push( ctl, FALSE );
    }
    if (!data->window)
    {
        /* SwiftUI's TabView (an NSTabView): the page goes in its box, below (or
         * above, TCS_BOTTOM) the tabs straddling its edge. The box fills the
         * control; these are its content's insets as SwiftUI lays it out. Being a
         * few points off only moves the page under the translucent box. */
        static const RECT insets = { 3, 23, 3, 7 };
        BOOL bottom = (GetWindowLongW( ctl->hwnd, GWL_STYLE ) & TCS_BOTTOM) != 0;
        int top = bottom ? insets.bottom : insets.top, low = bottom ? insets.top : insets.bottom;

        if (wparam)
        {
            rc->left -= insets.left;
            rc->top -= top;
            rc->right += insets.right;
            rc->bottom += low;
        }
        else
        {
            rc->left += insets.left;
            rc->top += top;
            rc->right -= insets.right;
            rc->bottom -= low;
        }
        *ret = 0;
        return TRUE;
    }
    /* a window sidebar: the page area is the tab control's whole rectangle */
    *ret = 0;
    return TRUE;
}

static void tab_items( struct w2s_control *ctl, struct json *j )
{
    int i, count = (int)SendMessageW( ctl->hwnd, TCM_GETITEMCOUNT, 0, 0 );
    WCHAR text[256];

    json_arr_begin( j, "items" );
    for (i = 0; i < count; i++)
    {
        TCITEMW item = { TCIF_TEXT };
        text[0] = 0;
        item.pszText = text;
        item.cchTextMax = ARRAYSIZE(text);
        SendMessageW( ctl->hwnd, TCM_GETITEMW, i, (LPARAM)&item );
        json_str( j, NULL, text );
    }
    json_arr_end( j );
    json_int( j, "selection", SendMessageW( ctl->hwnd, TCM_GETCURSEL, 0, 0 ) );
}

static void tab_snapshot( struct w2s_control *ctl, struct json *j )
{
    struct tab_data *data = tab_layout( ctl, FALSE );

    json_str_a( j, "mode", data->window ? "window" : "strip" );
    json_bool( j, "bottom", (GetWindowLongW( ctl->hwnd, GWL_STYLE ) & TCS_BOTTOM) != 0 );
    if (data->window) json_int( j, "sidebarPx", data->width );
    tab_items( ctl, j );
}

static void tab_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    NMHDR nm = { 0 };

    if (strcmp( ev->type, "select" ) || !ev->has_value) return;
    if ((int)SendMessageW( ctl->hwnd, TCM_GETCURSEL, 0, 0 ) == (int)ev->value) return;
    if (w2s_notify_parent( ctl->hwnd, TCN_SELCHANGING, &nm )) return; /* vetoed */
    SendMessageW( ctl->hwnd, TCM_SETCURSEL, (int)ev->value, 0 );
    w2s_notify_parent( ctl->hwnd, TCN_SELCHANGE, &nm );
}

static void tab_release( struct w2s_control *ctl )
{
    HeapFree( GetProcessHeap(), 0, ctl->data );
}

static const struct w2s_kind kind_tab = { "tab", tab_snapshot, tab_apply, tab_answer,
                                                  NULL, NULL, NULL, tab_release };

/* ---------- Wizard (map: propsheet.wizard) ---------- */

/* A wizard's tab control, which wine hides otherwise, carries the macOS
 * Installer layout: the page titles as steps in a sidebar, and the active
 * page's header, which wine hands over as the tab control's text
 * ("title\nsubtitle"). Marked with __wine_native_wizard, the tab control stays
 * shown over the page area and wine lays the wizard out around its
 * TCM_ADJUSTRECT (comctl32 propsheet.c), as for the property sheet sidebar:
 * the steps take the left of it, the header goes above the page. */
#define IDC_WIZARD_BACK 12323       /* comctl32's IDC_BACK_BUTTON */
#define IDC_WIZARD_NEXT 12324       /* comctl32's IDC_NEXT_BUTTON */

static const WCHAR native_wizard_prop[] = L"__wine_native_wizard";

struct wizard_data
{
    BOOL decided;
    int width;                      /* the steps, pixels; 0 when the titles don't tell the pages apart */
};

static BOOL is_wizard_tab( HWND hwnd )
{
    HWND parent = GetParent( hwnd );
    WCHAR name[16] = {0};

    GetClassNameW( parent, name, ARRAYSIZE(name) );
    return !wcscmp( name, L"#32770" ) && GetDlgCtrlID( hwnd ) == IDC_PROPSHEET_TAB &&
           GetDlgItem( parent, IDC_WIZARD_BACK ) && GetDlgItem( parent, IDC_WIZARD_NEXT );
}

static BOOL is_native_wizard_button( HWND hwnd )
{
    int id = GetDlgCtrlID( hwnd );
    return (id == IDC_WIZARD_BACK || id == IDC_WIZARD_NEXT) &&
           GetPropW( GetDlgItem( GetParent( hwnd ), IDC_PROPSHEET_TAB ), native_wizard_prop );
}

static struct wizard_data *wizard_data( struct w2s_control *ctl )
{
    HWND parent = GetParent( ctl->hwnd );
    struct w2s_control *button;
    int id;

    if (ctl->data) return ctl->data;
    ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct wizard_data) );
    /* from here on wine keeps the tab control shown and asks it for the layout */
    SetPropW( ctl->hwnd, native_wizard_prop, (HANDLE)1 );
    /* Back and Next were translated before us: they read Go Back / Continue now */
    for (id = IDC_WIZARD_BACK; id <= IDC_WIZARD_NEXT; id++)
        if ((button = w2s_control_from_hwnd( GetDlgItem( parent, id ) ))) w2s_push( button, FALSE );
    return ctl->data;
}

static void wizard_release( struct w2s_control *ctl )
{
    RemovePropW( ctl->hwnd, native_wizard_prop );
    HeapFree( GetProcessHeap(), 0, ctl->data );
}

/* decided at the first layout query, when wine has put all the pages in: the
 * steps only help when the titles tell the pages apart (many wizards give
 * every page the wizard's own title) */
static void wizard_decide( struct w2s_control *ctl, struct wizard_data *data )
{
    int i, k, count = (int)SendMessageW( ctl->hwnd, TCM_GETITEMCOUNT, 0, 0 ), distinct = 0, widest = 0;
    HFONT font = (HFONT)SendMessageW( ctl->hwnd, WM_GETFONT, 0, 0 );
    WCHAR (*titles)[128];
    HDC hdc;
    HGDIOBJ old;
    SIZE size;

    data->decided = TRUE;
    if (count < 2 || !(titles = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*titles) ))) return;
    hdc = GetDC( ctl->hwnd );
    old = SelectObject( hdc, font ? font : GetStockObject( DEFAULT_GUI_FONT ) );
    for (i = 0; i < count; i++)
    {
        TCITEMW item = { TCIF_TEXT };
        item.pszText = titles[i];
        item.cchTextMax = ARRAYSIZE(titles[i]);
        SendMessageW( ctl->hwnd, TCM_GETITEMW, i, (LPARAM)&item );
        if (!titles[i][0]) continue;
        for (k = 0; k < i; k++) if (!wcscmp( titles[k], titles[i] )) break;
        if (k == i) distinct++;
        if (GetTextExtentPoint32W( hdc, titles[i], wcslen( titles[i] ), &size )) widest = max( widest, size.cx );
    }
    SelectObject( hdc, old );
    ReleaseDC( ctl->hwnd, hdc );
    HeapFree( GetProcessHeap(), 0, titles );
    /* a bullet and the margins beside the widest title */
    if (distinct >= 2) data->width = min( max( widest * 5 / 4 + 56, 150 ), 260 );
    TRACE( "%p: wizard with %d pages, %d titles: %d px of steps\n", ctl->hwnd, count, distinct, data->width );
}

static BOOL wizard_answer( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *ret )
{
    struct wizard_data *data = wizard_data( ctl );
    RECT *rc = (RECT *)lparam;

    if (msg != TCM_ADJUSTRECT || !rc) return FALSE;
    if (!data->decided)
    {
        wizard_decide( ctl, data );
        w2s_push( ctl, FALSE );
    }
    /* the steps take the left; wine's own padding and header band stay its own */
    if (wparam) rc->left -= data->width;
    else rc->left += data->width;
    *ret = 0;
    return TRUE;
}

static void wizard_snapshot( struct w2s_control *ctl, struct json *j )
{
    struct wizard_data *data = wizard_data( ctl );
    HWND page = (HWND)SendMessageW( GetParent( ctl->hwnd ), PSM_GETCURRENTPAGEHWND, 0, 0 );
    WCHAR text[1024], *subtitle;
    RECT rc;

    json_str_a( j, "mode", "wizard" );
    json_int( j, "sidebarPx", data->width );
    tab_items( ctl, j );
    GetWindowTextW( ctl->hwnd, text, ARRAYSIZE(text) );
    if ((subtitle = wcschr( text, '\n' ))) *subtitle++ = 0;
    json_str( j, "heading", text );
    json_str( j, "subheading", subtitle ? subtitle : L"" );
    /* the header goes above the active page, from its left edge */
    if (page && IsWindowVisible( page ))
    {
        GetWindowRect( page, &rc );
        MapWindowPoints( NULL, ctl->hwnd, (POINT *)&rc, 2 );
        json_arr_begin( j, "headerPx" );
        json_int( j, NULL, rc.left );
        json_int( j, NULL, rc.top );
        json_arr_end( j );
    }
}

/* the steps can't be clicked: an Installer moves on with Continue only */
static const struct w2s_kind kind_wizard =
{
    "tab", wizard_snapshot, nothing_apply, wizard_answer, NULL, NULL, NULL, wizard_release, W2S_OWN_TEXT
};

/* ---------- Status bar ---------- */

#define MAX_SB_PARTS 64

static int status_parts( HWND hwnd, int *right )
{
    int n = (int)SendMessageW( hwnd, SB_GETPARTS, MAX_SB_PARTS, (LPARAM)right );
    return max( 0, min( n, MAX_SB_PARTS ) );
}

static void status_snapshot( struct w2s_control *ctl, struct json *j )
{
    int right[MAX_SB_PARTS], n, i;
    BOOL simple = (BOOL)SendMessageW( ctl->hwnd, SB_ISSIMPLE, 0, 0 );
    WCHAR text[1024];

    n = simple ? 1 : status_parts( ctl->hwnd, right );
    if (simple) right[0] = -1;
    json_bool( j, "simple", simple );
    json_arr_begin( j, "panes" );
    for (i = 0; i < n; i++)
    {
        DWORD info = SendMessageW( ctl->hwnd, SB_GETTEXTLENGTHW, i, 0 );
        BOOL owner_draw = (HIWORD( info ) & SBT_OWNERDRAW) != 0;

        json_obj_begin( j );
        text[0] = 0;
        if (!owner_draw && LOWORD( info ) < ARRAYSIZE(text)) SendMessageW( ctl->hwnd, SB_GETTEXTW, i, (LPARAM)text );
        json_str( j, "text", text );
        json_int( j, "right", right[i] );
        if (owner_draw) json_bool( j, "ownerDraw", TRUE );
        text[0] = 0;
        if (!simple) SendMessageW( ctl->hwnd, SB_GETTIPTEXTW, MAKEWPARAM( i, ARRAYSIZE(text) ), (LPARAM)text );
        if (text[0]) json_str( j, "tip", text );
        json_obj_end( j );
    }
    json_arr_end( j );
}

/* an owner-drawn pane (SBT_OWNERDRAW) stays the app's: wine keeps drawing it */
static HRGN status_region( struct w2s_control *ctl )
{
    int right[MAX_SB_PARTS], n, i;
    HRGN rgn = NULL;

    if (SendMessageW( ctl->hwnd, SB_ISSIMPLE, 0, 0 )) return NULL;
    n = status_parts( ctl->hwnd, right );
    for (i = 0; i < n; i++)
    {
        RECT rc;
        if (!(HIWORD( SendMessageW( ctl->hwnd, SB_GETTEXTLENGTHW, i, 0 ) ) & SBT_OWNERDRAW)) continue;
        if (!SendMessageW( ctl->hwnd, SB_GETRECT, i, (LPARAM)&rc )) continue;
        if (!rgn) rgn = CreateRectRgnIndirect( &rc );
        else
        {
            HRGN part = CreateRectRgnIndirect( &rc );
            CombineRgn( rgn, rgn, part, RGN_OR );
            DeleteObject( part );
        }
    }
    return rgn;
}

static void status_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    NMMOUSE nm = { { 0 } };
    RECT rc;

    if (!ev->has_value || (strcmp( ev->type, "click" ) && strcmp( ev->type, "dblclick" ))) return;
    nm.dwItemSpec = (DWORD_PTR)(INT_PTR)(int)ev->value;
    if (SendMessageW( ctl->hwnd, SB_GETRECT, (int)ev->value, (LPARAM)&rc ))
    {
        nm.pt.x = (rc.left + rc.right) / 2;
        nm.pt.y = (rc.top + rc.bottom) / 2;
    }
    w2s_notify_parent( ctl->hwnd, strcmp( ev->type, "click" ) ? NM_DBLCLK : NM_CLICK, &nm.hdr );
}

static const struct w2s_kind kind_statusbar =
    { "statusbar", status_snapshot, status_apply, NULL, NULL, status_region };

/* ---------- Tooltips: .help on translated controls ---------- */

/* A tooltip window isn't a view: wine's tooltip never shows over a native
 * control (its mouse messages stay native), so each translated control that
 * is a tool (TTF_IDISHWND, as apps add tools for dialog controls) gets that
 * tool's text as .help. The tooltip windows are watched here; when their
 * tools change, the tool controls are asked to send a fresh snapshot. */
#define MAX_TOOLTIPS 64

struct tooltip
{
    HWND hwnd;
    WNDPROC orig;
    BOOL active;            /* TTM_ACTIVATE */
    HWND *tools;            /* the tool windows last seen, HeapAlloc'd */
    int tool_count;
};

static const WCHAR tooltip_prop[] = L"Win32ToSwiftUI.Tooltip";
static CRITICAL_SECTION tooltip_section;
static CRITICAL_SECTION_DEBUG tooltip_section_debug =
{
    0, 0, &tooltip_section,
    { &tooltip_section_debug.ProcessLocksList, &tooltip_section_debug.ProcessLocksList },
    0, 0, 0
};
static CRITICAL_SECTION tooltip_section = { &tooltip_section_debug, -1, 0, 0, 0, 0 };
static HWND tooltips[MAX_TOOLTIPS];
static int tooltip_count;

/* the windows an app's tools stand for (not a control's own tooltip, whose
 * tools notify the control itself) */
static int tool_windows( HWND tip, HWND **out )
{
    int count = (int)SendMessageW( tip, TTM_GETTOOLCOUNT, 0, 0 ), i, n = 0;
    HWND *list = HeapAlloc( GetProcessHeap(), 0, max( count, 1 ) * sizeof(HWND) );

    for (i = 0; i < count; i++)
    {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.lpszText = NULL;     /* no copy: TTM_GETTEXT resolves the text */
        if (!SendMessageW( tip, TTM_ENUMTOOLSW, i, (LPARAM)&ti )) continue;
        if ((ti.uFlags & TTF_IDISHWND) && (HWND)ti.uId != ti.hwnd) list[n++] = (HWND)ti.uId;
    }
    *out = list;
    return n;
}

static void refresh_tools( struct tooltip *tip )
{
    HWND *now;
    int n = tool_windows( tip->hwnd, &now ), i;

    /* the ones that stopped being tools lose their help */
    for (i = 0; i < tip->tool_count; i++)
        if (w2s_control_from_hwnd( tip->tools[i] )) PostMessageW( tip->tools[i], w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    for (i = 0; i < n; i++)
        if (w2s_control_from_hwnd( now[i] )) PostMessageW( now[i], w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    if (tip->tools) HeapFree( GetProcessHeap(), 0, tip->tools );
    tip->tools = now;
    tip->tool_count = n;
}

static BOOL is_tooltip_change( UINT msg )
{
    unsigned int count, i;
    const UINT *list = w2s_map_state_in( "tooltip", &count );

    if (msg == TTM_SETTOOLINFOA || msg == TTM_SETTOOLINFOW) return TRUE;
    for (i = 0; i < count; i++) if (list[i] == msg) return TRUE;
    return FALSE;
}

static LRESULT CALLBACK tooltip_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct tooltip *tip = GetPropW( hwnd, tooltip_prop );
    LRESULT ret;
    int i;

    if (!tip) return DefWindowProcW( hwnd, msg, wparam, lparam );
    if (msg == WM_NCDESTROY)
    {
        WNDPROC orig = tip->orig;

        EnterCriticalSection( &tooltip_section );
        for (i = 0; i < tooltip_count; i++) if (tooltips[i] == hwnd) tooltips[i] = tooltips[--tooltip_count];
        LeaveCriticalSection( &tooltip_section );
        tip->active = FALSE;
        for (i = 0; i < tip->tool_count; i++)
            if (w2s_control_from_hwnd( tip->tools[i] )) PostMessageW( tip->tools[i], w2s_wake_message, W2S_WAKE_REFRESH, 0 );
        RemovePropW( hwnd, tooltip_prop );
        if (tip->tools) HeapFree( GetProcessHeap(), 0, tip->tools );
        HeapFree( GetProcessHeap(), 0, tip );
        return CallWindowProcW( orig, hwnd, msg, wparam, lparam );
    }
    ret = CallWindowProcW( tip->orig, hwnd, msg, wparam, lparam );
    if (msg == TTM_ACTIVATE) tip->active = wparam != 0;
    if (is_tooltip_change( msg )) refresh_tools( tip );
    return ret;
}

void w2s_observe_tooltip( HWND hwnd )
{
    struct tooltip *tip;

    if (GetPropW( hwnd, tooltip_prop )) return;
    EnterCriticalSection( &tooltip_section );
    if (tooltip_count < MAX_TOOLTIPS) tooltips[tooltip_count++] = hwnd;
    else hwnd = NULL;
    LeaveCriticalSection( &tooltip_section );
    if (!hwnd) return;
    tip = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*tip) );
    tip->hwnd = hwnd;
    tip->active = TRUE;
    SetPropW( hwnd, tooltip_prop, tip );
    tip->orig = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)tooltip_proc );
}

WCHAR *w2s_tool_text( HWND hwnd )
{
    HWND list[MAX_TOOLTIPS];
    int count, i, t;

    EnterCriticalSection( &tooltip_section );
    count = tooltip_count;
    memcpy( list, tooltips, count * sizeof(HWND) );
    LeaveCriticalSection( &tooltip_section );

    for (t = 0; t < count; t++)
    {
        struct tooltip *tip = GetPropW( list[t], tooltip_prop );
        int tools;

        if (!tip || !tip->active || !IsWindow( list[t] )) continue;
        tools = (int)SendMessageW( list[t], TTM_GETTOOLCOUNT, 0, 0 );
        for (i = 0; i < tools; i++)
        {
            TTTOOLINFOW ti = { sizeof(ti) };
            WCHAR *text;

            ti.lpszText = NULL;
            if (!SendMessageW( list[t], TTM_ENUMTOOLSW, i, (LPARAM)&ti )) continue;
            if (!(ti.uFlags & TTF_IDISHWND) || (HWND)ti.uId != hwnd || ti.hwnd == hwnd) continue;
            /* resolves LPSTR_TEXTCALLBACK (TTN_GETDISPINFO) and resource strings */
            text = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, INFOTIPSIZE * sizeof(WCHAR) );
            ti.lpszText = text;
            SendMessageW( list[t], TTM_GETTEXTW, INFOTIPSIZE, (LPARAM)&ti );
            if (text[0]) return text;
            HeapFree( GetProcessHeap(), 0, text );
        }
    }
    return NULL;
}

/* ---------- selection ---------- */

/* ---------- Toolbar (map: toolbar) and rebar (map: rebar) ---------- */

/* A toolbar's buttons as native controls, each where comctl32 lays it out, so
 * the windows an app puts in a toolbar or its rebar (a font list) keep their
 * place. Those windows are the app's: wine keeps drawing them (the region).
 * A native click is the toolbar's own mouse click, so checks, groups,
 * drop-downs and WM_COMMAND happen as comctl32 does them. */
#define MAX_TB_BUTTONS 256
#define MAX_TB_STRIPS 16

struct tb_strip
{
    int first, count;               /* the strip's images in the toolbar's image list */
    WCHAR module[64];               /* comctl32.dll for the standard strips */
    UINT bitmap;
};

struct tb_data
{
    struct lv_data images;          /* the images that go as pixels (image_list_json) */
    int total;                      /* the image list's size when last seen */
    int strips;
    struct tb_strip strip[MAX_TB_STRIPS];
};

static struct tb_data *toolbar_data( struct w2s_control *ctl )
{
    if (!ctl->data) ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct tb_data) );
    return ctl->data;
}

/* comctl32's standard strips: the small one's id (the large holds the same images) and size */
static int standard_strip( UINT *bitmap )
{
    switch (*bitmap)
    {
    case IDB_STD_SMALL_COLOR: case IDB_STD_LARGE_COLOR: *bitmap = IDB_STD_SMALL_COLOR; return STD_PRINT + 1;
    case IDB_VIEW_SMALL_COLOR: case IDB_VIEW_LARGE_COLOR: *bitmap = IDB_VIEW_SMALL_COLOR; return VIEW_VIEWMENU + 1;
    case IDB_HIST_SMALL_COLOR: case IDB_HIST_LARGE_COLOR: case IDB_HIST_NORMAL: case IDB_HIST_HOT:
    case IDB_HIST_DISABLED: case IDB_HIST_PRESSED: *bitmap = IDB_HIST_SMALL_COLOR; return HIST_VIEWTREE + 1;
    }
    return 0;
}

/* where the images a TB_ADDBITMAP or TB_LOADIMAGES just added came from. They
 * start where the image list ended before (wine doesn't always add a standard
 * strip's every image); an image list the app sets starts over. */
static void toolbar_observe( struct w2s_control *ctl, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct tb_data *data;
    struct tb_strip *strip;
    HIMAGELIST himl;
    HINSTANCE inst;
    WCHAR path[MAX_PATH], *name;
    UINT bitmap;
    int total, first;

    if (msg != TB_ADDBITMAP && msg != TB_LOADIMAGES && msg != TB_SETIMAGELIST) return;
    data = toolbar_data( ctl );
    himl = (HIMAGELIST)SendMessageW( ctl->hwnd, TB_GETIMAGELIST, 0, 0 );
    total = himl ? ImageList_GetImageCount( himl ) : 0;
    first = data->total;
    data->total = total;
    if (msg == TB_SETIMAGELIST)
    {
        data->strips = 0;
        return;
    }
    if (data->strips >= MAX_TB_STRIPS || total <= first) return;
    if (msg == TB_LOADIMAGES)
    {
        inst = (HINSTANCE)lparam;
        bitmap = wparam;
    }
    else
    {
        const TBADDBITMAP *ab = (const TBADDBITMAP *)lparam;
        if (!ab) return;
        inst = ab->hInst;
        bitmap = ab->nID;
    }
    strip = &data->strip[data->strips];
    strip->count = total - first;
    if (inst == HINST_COMMCTRL)
    {
        /* wine can complete a standard strip after the call returns: its size is known */
        int count = standard_strip( &bitmap );
        if (!count) return;
        strip->count = max( strip->count, count );
        lstrcpyW( strip->module, L"comctl32.dll" );
    }
    else if (inst && GetModuleFileNameW( inst, path, MAX_PATH ))
    {
        name = wcsrchr( path, '\\' );
        lstrcpynW( strip->module, name ? name + 1 : path, ARRAYSIZE(strip->module) );
    }
    else return;    /* an HBITMAP of the app's */
    strip->first = first;
    strip->bitmap = bitmap;
    data->strips++;
    TRACE( "%p: images %d-%d from %ls/%u\n", ctl->hwnd, first, first + strip->count - 1, strip->module, bitmap );
}

/* the macOS image for an image from a known strip (map: toolbar_images), or NULL */
static const char *toolbar_image_spec( const struct tb_data *data, int image )
{
    unsigned int i, k;

    for (i = data->strips; i-- > 0;)
    {
        const struct tb_strip *strip = &data->strip[i];
        if (image < strip->first || image >= strip->first + strip->count) continue;
        for (k = 0; k < ARRAYSIZE(w2s_toolbar_images); k++)
            if (w2s_toolbar_images[k].bitmap == strip->bitmap && w2s_toolbar_images[k].index == image - strip->first &&
                !_wcsicmp( w2s_toolbar_images[k].module, strip->module ))
                return w2s_toolbar_images[k].spec;
        return NULL;
    }
    return NULL;
}

/* a button's tooltip, which the app gives on TTN_GETDISPINFO: asked the way the
 * toolbar's own tooltip control asks it */
static void toolbar_tip( HWND hwnd, int id, WCHAR *buf, int size )
{
    HWND tips = (HWND)SendMessageW( hwnd, TB_GETTOOLTIPS, 0, 0 );
    NMTTDISPINFOW di;

    buf[0] = 0;
    if (!tips) return;
    memset( &di, 0, sizeof(di) );
    di.hdr.hwndFrom = tips;
    di.hdr.idFrom = id;
    di.hdr.code = TTN_GETDISPINFOW;
    SendMessageW( hwnd, WM_NOTIFY, id, (LPARAM)&di );
    if (di.lpszText && di.lpszText != LPSTR_TEXTCALLBACKW)
    {
        if (!IS_INTRESOURCE( di.lpszText )) lstrcpynW( buf, di.lpszText, size );
        else if (di.hinst) LoadStringW( di.hinst, LOWORD( di.lpszText ), buf, size );
    }
    if (!buf[0] && di.szText[0]) lstrcpynW( buf, di.szText, size );
}

/* the app's own windows in a toolbar or rebar that the native view doesn't
 * stand in for: wine keeps drawing them. Window coordinates. */
static HRGN children_region( HWND hwnd, struct json *j )
{
    HRGN rgn = NULL, part;
    HWND child;
    RECT rc, wr;

    GetWindowRect( hwnd, &wr );
    if (j) json_arr_begin( j, "children" );
    for (child = GetWindow( hwnd, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
    {
        if (!IsWindowVisible( child ) || w2s_control_from_hwnd( child )) continue;
        GetWindowRect( child, &rc );
        if (j)
        {
            /* the native view is the client area */
            RECT cr = rc;
            MapWindowPoints( NULL, hwnd, (POINT *)&cr, 2 );
            json_arr_begin( j, NULL );
            json_int( j, NULL, cr.left );
            json_int( j, NULL, cr.top );
            json_int( j, NULL, cr.right );
            json_int( j, NULL, cr.bottom );
            json_arr_end( j );
        }
        OffsetRect( &rc, -wr.left, -wr.top );
        part = CreateRectRgnIndirect( &rc );
        if (!rgn) rgn = part;
        else
        {
            CombineRgn( rgn, rgn, part, RGN_OR );
            DeleteObject( part );
        }
    }
    if (j) json_arr_end( j );
    return rgn;
}

static HRGN container_region( struct w2s_control *ctl )
{
    return children_region( ctl->hwnd, NULL );
}

static void toolbar_snapshot( struct w2s_control *ctl, struct json *j )
{
    struct tb_data *data = toolbar_data( ctl );
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    DWORD ex = (DWORD)SendMessageW( ctl->hwnd, TB_GETEXTENDEDSTYLE, 0, 0 );
    HIMAGELIST himl = (HIMAGELIST)SendMessageW( ctl->hwnd, TB_GETIMAGELIST, 0, 0 );
    int count = min( (int)SendMessageW( ctl->hwnd, TB_BUTTONCOUNT, 0, 0 ), MAX_TB_BUTTONS ), i, npixels = 0;
    int pixels[MAX_TB_BUTTONS];
    WCHAR text[256];
    HRGN rgn;

    json_bool( j, "list", (style & TBSTYLE_LIST) != 0 );
    json_bool( j, "mixed", (ex & TBSTYLE_EX_MIXEDBUTTONS) != 0 );
    json_arr_begin( j, "buttons" );
    for (i = 0; i < count; i++)
    {
        TBBUTTON b;
        RECT rc = { 0 };
        const char *spec;
        int image, len;

        memset( &b, 0, sizeof(b) );
        if (!SendMessageW( ctl->hwnd, TB_GETBUTTON, i, (LPARAM)&b )) continue;
        SendMessageW( ctl->hwnd, TB_GETITEMRECT, i, (LPARAM)&rc );
        json_obj_begin( j );
        json_int( j, "i", i );
        json_arr_begin( j, "rect" );
        json_int( j, NULL, rc.left );
        json_int( j, NULL, rc.top );
        json_int( j, NULL, rc.right - rc.left );
        json_int( j, NULL, rc.bottom - rc.top );
        json_arr_end( j );
        if (b.fsState & TBSTATE_HIDDEN) json_bool( j, "hidden", TRUE );
        if (b.fsStyle & BTNS_SEP)
        {
            json_bool( j, "sep", TRUE );
            json_obj_end( j );
            continue;
        }
        json_int( j, "id", b.idCommand );
        json_bool( j, "enabled", (b.fsState & TBSTATE_ENABLED) != 0 );
        if (b.fsStyle & BTNS_CHECK) json_bool( j, "check", TRUE );
        if ((b.fsStyle & BTNS_CHECKGROUP) == BTNS_CHECKGROUP) json_bool( j, "group", TRUE );
        if (b.fsState & (TBSTATE_CHECKED | TBSTATE_PRESSED)) json_bool( j, "checked", TRUE );
        if (b.fsStyle & BTNS_WHOLEDROPDOWN) json_int( j, "dropdown", 2 );
        else if (b.fsStyle & BTNS_DROPDOWN) json_int( j, "dropdown", 1 );
        if (b.fsStyle & BTNS_SHOWTEXT) json_bool( j, "showText", TRUE );
        text[0] = 0;
        len = (int)SendMessageW( ctl->hwnd, TB_GETBUTTONTEXTW, b.idCommand, 0 );
        if (len > 0 && len < ARRAYSIZE(text)) SendMessageW( ctl->hwnd, TB_GETBUTTONTEXTW, b.idCommand, (LPARAM)text );
        if (text[0]) json_str( j, "text", text );
        toolbar_tip( ctl->hwnd, b.idCommand, text, ARRAYSIZE(text) );
        if (text[0]) json_str( j, "tip", text );
        /* list 0 of several image lists (MAKELONG(index, list)): the only one there is mostly */
        image = (b.iBitmap >= 0 && !HIWORD( b.iBitmap )) ? b.iBitmap : -1;
        if (image >= 0)
        {
            if ((spec = toolbar_image_spec( data, image ))) json_str_a( j, "sym", spec );
            else
            {
                json_int( j, "img", image );
                pixels[npixels++] = image;
            }
        }
        json_obj_end( j );
    }
    json_arr_end( j );
    image_list_json( &data->images, himl, pixels, npixels, j );
    if (data->images.more) PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
    /* where the app's own windows are, so moving one refreshes the region */
    if ((rgn = children_region( ctl->hwnd, j ))) DeleteObject( rgn );
}

static void toolbar_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    BOOL dropdown = !strcmp( ev->type, "dropdown" );
    TBBUTTON b;
    RECT rc;
    POINT pt;
    int i;

    if (!ev->has_value || (strcmp( ev->type, "click" ) && !dropdown)) return;
    i = (int)ev->value;
    memset( &b, 0, sizeof(b) );
    if (!SendMessageW( ctl->hwnd, TB_GETBUTTON, i, (LPARAM)&b ) ||
        !SendMessageW( ctl->hwnd, TB_GETITEMRECT, i, (LPARAM)&rc )) return;
    pt.x = (rc.left + rc.right) / 2;
    pt.y = (rc.top + rc.bottom) / 2;
    if ((b.fsStyle & BTNS_DROPDOWN) && !(b.fsStyle & BTNS_WHOLEDROPDOWN) &&
        (SendMessageW( ctl->hwnd, TB_GETEXTENDEDSTYLE, 0, 0 ) & TBSTYLE_EX_DRAWDDARROWS))
    {
        /* the arrow part or the button part of a split drop-down */
        pt.x = dropdown ? rc.right - 4 : rc.left + (rc.right - rc.left - 12) / 2;
    }
    /* the toolbar's own click: comctl32 checks, groups, drops down and sends WM_COMMAND */
    SendMessageW( ctl->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM( pt.x, pt.y ) );
    if (IsWindow( ctl->hwnd )) SendMessageW( ctl->hwnd, WM_LBUTTONUP, 0, MAKELPARAM( pt.x, pt.y ) );
}

static const struct w2s_kind kind_toolbar =
{
    "toolbar", toolbar_snapshot, toolbar_apply, NULL, NULL, container_region, toolbar_observe
};

/* A rebar draws nothing of its own on macOS (no grippers, no etched lines): its
 * bands' windows are translated or stay the app's (the region) */
static void rebar_snapshot( struct w2s_control *ctl, struct json *j )
{
    HRGN rgn;
    json_int( j, "bands", (int)SendMessageW( ctl->hwnd, RB_GETBANDCOUNT, 0, 0 ) );
    if ((rgn = children_region( ctl->hwnd, j ))) DeleteObject( rgn );
}

static const struct w2s_kind kind_rebar =
{
    "rebar", rebar_snapshot, nothing_apply, NULL, NULL, container_region
};

const struct w2s_kind *w2s_select_kind( HWND hwnd )
{
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );
    HWND parent = GetParent( hwnd );
    WCHAR name[128], parent_name[128];

    if (!(style & WS_CHILD) || !parent) return NULL;
    class_name( hwnd, name, ARRAYSIZE(name) );
    class_name( parent, parent_name, ARRAYSIZE(parent_name) );

    /* a rebar's toolbars and ComboBoxEx lists are translated as anywhere (map: rebar) */
    if (is_class( parent_name, L"ReBarWindow32" ) && is_class( name, TOOLBARCLASSNAMEW ))
        return (style & CCS_VERT) ? NULL : &kind_toolbar;
    if (is_class( name, WC_COMBOBOXEXW ))
        return (style & 3) == CBS_SIMPLE ? NULL : &kind_comboboxex;

    /* parts of composite controls belong to their parent */
    if (is_class( parent_name, L"ComboBox" ) || is_class( parent_name, L"ComboBoxEx32" ) ||
        is_class( parent_name, L"SysListView32" ) || is_class( parent_name, L"SysTreeView32" ) ||
        is_class( parent_name, L"SysDateTimePick32" ) || is_class( parent_name, L"SysIPAddress32" ) ||
        is_class( parent_name, L"msctls_updown32" ) || is_class( parent_name, L"ToolbarWindow32" ) ||
        is_class( parent_name, L"ReBarWindow32" ) || is_class( parent_name, L"SysTabControl32" ))
        return NULL;

    if (is_class( name, L"Button" ))
    {
        if (style & (BS_BITMAP | BS_ICON)) return NULL;
        switch (style & BS_TYPEMASK)
        {
        case BS_PUSHBUTTON: return &kind_button_push;
        case BS_DEFPUSHBUTTON: return &kind_button_default;
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX: return (style & BS_PUSHLIKE) ? &kind_button_pushlike : &kind_button_checkbox;
        case BS_3STATE:
        case BS_AUTO3STATE: return (style & BS_PUSHLIKE) ? &kind_button_pushlike : &kind_button_3state;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON: return (style & BS_PUSHLIKE) ? &kind_button_pushlike : &kind_button_radio;
        case BS_GROUPBOX: return &kind_button_groupbox;
        case BS_SPLITBUTTON:
        case BS_DEFSPLITBUTTON: return &kind_button_split;
        case BS_COMMANDLINK:
        case BS_DEFCOMMANDLINK: return &kind_button_commandlink;
        default: return NULL; /* owner-draw, user button */
        }
    }
    if (is_class( name, L"Static" ))
    {
        switch (style & SS_TYPEMASK)
        {
        case SS_LEFT:
        case SS_CENTER:
        case SS_RIGHT:
        case SS_SIMPLE:
        case SS_LEFTNOWORDWRAP: return &kind_static_text;
        case SS_ICON:
        case SS_BITMAP: return &kind_static_image;
        case SS_ETCHEDHORZ:
        case SS_ETCHEDVERT: return &kind_static_separator;
        case SS_BLACKFRAME:
        case SS_GRAYFRAME:
        case SS_WHITEFRAME:
        case SS_ETCHEDFRAME:
        case SS_BLACKRECT:
        case SS_GRAYRECT:
        case SS_WHITERECT: return &kind_static_frame;
        default: return NULL;
        }
    }
    if (is_class( name, L"Edit" ))
    {
        if (style & ES_MULTILINE) return (style & ES_PASSWORD) ? NULL : &kind_edit_multiline;
        if (style & ES_PASSWORD) return &kind_edit_password;
        if (style & ES_READONLY) return &kind_edit_readonly;
        if (style & ES_NUMBER) return &kind_edit_number;
        return &kind_edit_single;
    }
    if (is_class( name, L"ComboBox" ))
    {
        if (style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) return NULL;
        if ((style & 3) == CBS_DROPDOWNLIST) return &kind_combobox_dropdownlist;
        if ((style & 3) == CBS_DROPDOWN) return &kind_combobox_editable;
        return NULL;
    }
    if (is_class( name, L"ListBox" ))
    {
        if (style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE | LBS_NODATA | LBS_MULTICOLUMN | LBS_NOSEL)) return NULL;
        return (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? &kind_listbox_multi : &kind_listbox_single;
    }
    if (is_class( name, WC_LISTVIEWW ))
    {
        /* LVM_GETVIEW follows both the style and LVM_SETVIEW */
        DWORD view = (DWORD)SendMessageW( hwnd, LVM_GETVIEW, 0, 0 );
        DWORD ex = (DWORD)SendMessageW( hwnd, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0 );

        if (style & (LVS_OWNERDRAWFIXED | LVS_OWNERDATA)) return NULL;
        switch (view)
        {
        case LV_VIEW_DETAILS: return (ex & LVS_EX_CHECKBOXES) ? &kind_listview_checkboxes : &kind_listview_report;
        case LV_VIEW_LIST: return (ex & LVS_EX_CHECKBOXES) ? &kind_listview_checkboxes : &kind_listview_list;
        case LV_VIEW_ICON:
        case LV_VIEW_SMALLICON: return &kind_listview_icon;
        default: return NULL;   /* tiles */
        }
    }
    if (is_class( name, STATUSCLASSNAMEW )) return &kind_statusbar;
    if (is_class( name, WC_LINK )) return &kind_syslink;
    if (is_class( name, TOOLBARCLASSNAMEW )) return (style & CCS_VERT) ? NULL : &kind_toolbar;
    if (is_class( name, REBARCLASSNAMEW )) return (style & CCS_VERT) ? NULL : &kind_rebar;
    if (is_class( name, UPDOWN_CLASSW )) return &kind_updown;
    if (is_class( name, DATETIMEPICK_CLASSW )) return &kind_datetime;
    if (is_class( name, MONTHCAL_CLASSW ))
    {
        /* a range of days: NSDatePicker's range mode, not translated yet */
        if (style & MCS_MULTISELECT) return NULL;
        return &kind_monthcal;
    }
    if (is_class( name, WC_TREEVIEWW ))
    {
        /* check boxes are state images the native rows don't show yet */
        if (style & TVS_CHECKBOXES) return NULL;
        return &kind_treeview;
    }
    if (is_class( name, PROGRESS_CLASSW )) return (style & PBS_VERTICAL) ? NULL : &kind_progress;
    if (is_class( name, TRACKBAR_CLASSW )) return (style & TBS_VERT) ? &kind_trackbar_vertical : &kind_trackbar;
    if (is_class( name, WC_TABCONTROLW ))
    {
        if (is_wizard_tab( hwnd )) return &kind_wizard;
        /* TCS_MULTILINE (every property sheet) is fine: several rows become a
         * window sidebar from macOS 13, and a strip scrolls before that */
        if (style & (TCS_OWNERDRAWFIXED | TCS_BUTTONS | TCS_VERTICAL)) return NULL;
        return &kind_tab;
    }
    return NULL;
}
