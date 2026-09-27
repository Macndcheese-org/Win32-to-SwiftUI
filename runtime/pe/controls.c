/*
 * win32swiftui.dll: the controls the runtime translates (milestone 1), one
 * kind per map entry: which windows it takes, what it tells the native view
 * (snapshot), and how native events become Win32 actions (apply).
 */
#include "w2s_pe.h"

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

static void button_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    json_int( j, "checked", SendMessageW( ctl->hwnd, BM_GETCHECK, 0, 0 ) );
    json_bool( j, "isDefault", (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON );
    json_bool( j, "noPrefix", FALSE );
    json_bool( j, "leftText", (style & BS_LEFTTEXT) != 0 );
    json_bool( j, "isCancel", GetDlgCtrlID( ctl->hwnd ) == IDCANCEL );
}

static void button_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    if (!strcmp( ev->type, "click" )) SendMessageW( ctl->hwnd, BM_CLICK, 0, 0 );
}

static void groupbox_snapshot( struct w2s_control *ctl, struct json *j )
{
}

static void nothing_apply( struct w2s_control *ctl, const struct w2s_event *ev )
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
static void json_base64( struct json *j, const char *key, const BYTE *data, size_t n )
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
static BYTE *image_bgra( HICON icon, HBITMAP bitmap, int w, int h )
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
    if ((style & SS_TYPEMASK) == SS_ICON) bits = image_bgra( image, NULL, w, h );
    else bits = image_bgra( NULL, image, w, h );
    if (!bits) return;
    json_int( j, "imageWidth", w );
    json_int( j, "imageHeight", h );
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

static const struct w2s_kind kind_edit_single = { "edit.single", edit_snapshot, edit_apply };
static const struct w2s_kind kind_edit_password = { "edit.password", edit_snapshot, edit_apply };
static const struct w2s_kind kind_edit_number = { "edit.number", edit_snapshot, edit_apply };
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
        LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM };
        text[0] = 0;
        col.pszText = text;
        col.cchTextMax = ARRAYSIZE(text);
        col.iSubItem = order[c];
        if (!SendMessageW( hwnd, LVM_GETCOLUMNW, order[c], (LPARAM)&col )) col.iSubItem = order[c];
        subitems[c] = col.iSubItem;
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
    BYTE sent[MAX_LV_IMAGES / 8];
};

static void listview_icons( struct w2s_control *ctl, struct json *j, BOOL small, int count )
{
    struct lv_data *data = ctl->data;
    HIMAGELIST himl = (HIMAGELIST)SendMessageW( ctl->hwnd, LVM_GETIMAGELIST, small ? LVSIL_SMALL : LVSIL_NORMAL, 0 );
    int cx = 0, cy = 0, i, fresh = 0;
    char key[16];

    if (!data) data = ctl->data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) );
    if (himl) ImageList_GetIconSize( himl, &cx, &cy );
    if (himl != data->himl || cx != data->cx || cy != data->cy)
    {
        data->himl = himl;
        data->cx = cx;
        data->cy = cy;
        data->gen++;
        memset( data->sent, 0, sizeof(data->sent) );
    }
    json_bool( j, "small", small );
    json_int( j, "imageGen", data->gen );
    json_arr_begin( j, "imageSize" );
    json_int( j, NULL, cx );
    json_int( j, NULL, cy );
    json_arr_end( j );
    json_arr_begin( j, "icons" );
    for (i = 0; i < count && i < 2000; i++)
    {
        LVITEMW item = { LVIF_IMAGE };
        item.iItem = i;
        item.iImage = -1;
        SendMessageW( ctl->hwnd, LVM_GETITEMW, 0, (LPARAM)&item );
        json_int( j, NULL, himl ? item.iImage : -1 );
    }
    json_arr_end( j );
    if (!himl || cx <= 0 || cy <= 0 || cx > 256 || cy > 256) return;

    json_key_obj_begin( j, "images" );
    for (i = 0; i < count && i < 2000 && fresh < 64; i++)
    {
        LVITEMW item = { LVIF_IMAGE };
        HICON icon;
        BYTE *bits;

        item.iItem = i;
        item.iImage = -1;
        SendMessageW( ctl->hwnd, LVM_GETITEMW, 0, (LPARAM)&item );
        if (item.iImage < 0 || item.iImage >= MAX_LV_IMAGES || (data->sent[item.iImage / 8] & (1 << (item.iImage % 8))))
            continue;
        data->sent[item.iImage / 8] |= 1 << (item.iImage % 8);
        if (!(icon = ImageList_GetIcon( himl, item.iImage, ILD_NORMAL ))) continue;
        if ((bits = image_bgra( icon, NULL, cx, cy )))
        {
            snprintf( key, sizeof(key), "%d", item.iImage );
            json_base64( j, key, bits, (size_t)cx * cy * 4 );
            HeapFree( GetProcessHeap(), 0, bits );
        }
        DestroyIcon( icon );
        if (++fresh == 64)
        {
            /* enough for one snapshot: the rest come with the next one */
            PostMessageW( ctl->hwnd, w2s_wake_message, W2S_WAKE_REFRESH, 0 );
            break;
        }
    }
    json_obj_end( j );
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

static void tree_nodes( HWND hwnd, HTREEITEM item, struct json *j, int depth, int *budget )
{
    for (; item && *budget > 0; item = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)item ))
    {
        TVITEMW it = { TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN | TVIF_HANDLE };
        WCHAR text[260];
        HTREEITEM child;
        BOOL open;

        text[0] = 0;
        it.hItem = item;
        it.pszText = text;
        it.cchTextMax = ARRAYSIZE(text);
        it.stateMask = TVIS_EXPANDED;
        SendMessageW( hwnd, TVM_GETITEMW, 0, (LPARAM)&it );
        if (it.pszText != text) lstrcpynW( text, it.pszText && it.pszText != LPSTR_TEXTCALLBACKW ? it.pszText : L"", ARRAYSIZE(text) );
        (*budget)--;
        child = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)item );
        open = (it.state & TVIS_EXPANDED) && child && depth < MAX_TREE_DEPTH;

        json_obj_begin( j );
        json_int( j, "id", (INT_PTR)item );
        json_str( j, "text", text );
        json_bool( j, "kids", child || it.cChildren > 0 );
        if (open)
        {
            json_bool( j, "open", TRUE );
            json_arr_begin( j, "children" );
            tree_nodes( hwnd, child, j, depth + 1, budget );
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

    json_arr_begin( j, "nodes" );
    tree_nodes( ctl->hwnd, (HTREEITEM)SendMessageW( ctl->hwnd, TVM_GETNEXTITEM, TVGN_ROOT, 0 ), j, 0, &budget );
    json_arr_end( j );
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
}

static void trackbar_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    HWND parent = GetParent( ctl->hwnd );
    int pos = (int)ev->value;

    if (!ev->has_value) return;
    if (!strcmp( ev->type, "value" ))
    {
        SendMessageW( ctl->hwnd, TBM_SETPOS, TRUE, pos );
        SendMessageW( parent, WM_HSCROLL, MAKEWPARAM( TB_THUMBTRACK, pos ), (LPARAM)ctl->hwnd );
    }
    else if (!strcmp( ev->type, "valueEnd" ))
    {
        SendMessageW( ctl->hwnd, TBM_SETPOS, TRUE, pos );
        SendMessageW( parent, WM_HSCROLL, MAKEWPARAM( TB_THUMBPOSITION, pos ), (LPARAM)ctl->hwnd );
        SendMessageW( parent, WM_HSCROLL, MAKEWPARAM( TB_ENDTRACK, 0 ), (LPARAM)ctl->hwnd );
    }
}

static const struct w2s_kind kind_trackbar = { "trackbar", trackbar_snapshot, trackbar_apply };

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

static void tab_snapshot( struct w2s_control *ctl, struct json *j )
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

static void tab_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    NMHDR nm = { 0 };

    if (strcmp( ev->type, "select" ) || !ev->has_value) return;
    if ((int)SendMessageW( ctl->hwnd, TCM_GETCURSEL, 0, 0 ) == (int)ev->value) return;
    if (w2s_notify_parent( ctl->hwnd, TCN_SELCHANGING, &nm )) return; /* vetoed */
    SendMessageW( ctl->hwnd, TCM_SETCURSEL, (int)ev->value, 0 );
    w2s_notify_parent( ctl->hwnd, TCN_SELCHANGE, &nm );
}

static const struct w2s_kind kind_tab = { "tab", tab_snapshot, tab_apply };

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

const struct w2s_kind *w2s_select_kind( HWND hwnd )
{
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );
    HWND parent = GetParent( hwnd );
    WCHAR name[128], parent_name[128];

    if (!(style & WS_CHILD) || !parent) return NULL;
    class_name( hwnd, name, ARRAYSIZE(name) );
    class_name( parent, parent_name, ARRAYSIZE(parent_name) );

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
    if (is_class( name, TRACKBAR_CLASSW )) return (style & TBS_VERT) ? NULL : &kind_trackbar;
    if (is_class( name, WC_TABCONTROLW ))
    {
        if (style & (TCS_OWNERDRAWFIXED | TCS_BUTTONS | TCS_VERTICAL | TCS_MULTILINE)) return NULL;
        return &kind_tab;
    }
    return NULL;
}
