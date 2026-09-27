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

static void static_image_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    HANDLE image;
    int w = 0, h = 0;
    BITMAPINFO bmi;
    BYTE *bits;
    HDC hdc;
    HBITMAP dib, old;

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

    memset( &bmi, 0, sizeof(bmi) );
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    hdc = CreateCompatibleDC( 0 );
    dib = CreateDIBSection( hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    if (!dib) { DeleteDC( hdc ); return; }
    old = SelectObject( hdc, dib );
    memset( bits, 0, w * h * 4 );
    if ((style & SS_TYPEMASK) == SS_ICON) DrawIconEx( hdc, 0, 0, image, w, h, 0, 0, DI_NORMAL );
    else
    {
        HDC src = CreateCompatibleDC( 0 );
        HGDIOBJ prev = SelectObject( src, image );
        int i;
        BitBlt( hdc, 0, 0, w, h, src, 0, 0, SRCCOPY );
        SelectObject( src, prev );
        DeleteDC( src );
        for (i = 0; i < w * h; i++) bits[i * 4 + 3] = 0xff; /* bitmaps are opaque */
    }
    GdiFlush();
    json_int( j, "imageWidth", w );
    json_int( j, "imageHeight", h );
    json_base64( j, "imageBGRA", bits, (size_t)w * h * 4 );
    SelectObject( hdc, old );
    DeleteObject( dib );
    DeleteDC( hdc );
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
static void edit_apply( struct w2s_control *ctl, const struct w2s_event *ev )
{
    int old_len, new_len, prefix = 0, suffix = 0;
    WCHAR *old, *inserted;

    if (strcmp( ev->type, "text" ) || !ev->string) return;
    old_len = GetWindowTextLengthW( ctl->hwnd );
    old = HeapAlloc( GetProcessHeap(), 0, (old_len + 1) * sizeof(WCHAR) );
    GetWindowTextW( ctl->hwnd, old, old_len + 1 );
    new_len = wcslen( ev->string );

    while (prefix < old_len && prefix < new_len && old[prefix] == ev->string[prefix]) prefix++;
    while (suffix < old_len - prefix && suffix < new_len - prefix &&
           old[old_len - 1 - suffix] == ev->string[new_len - 1 - suffix]) suffix++;

    inserted = HeapAlloc( GetProcessHeap(), 0, (new_len - prefix - suffix + 1) * sizeof(WCHAR) );
    memcpy( inserted, ev->string + prefix, (new_len - prefix - suffix) * sizeof(WCHAR) );
    inserted[new_len - prefix - suffix] = 0;

    SendMessageW( ctl->hwnd, EM_SETSEL, prefix, old_len - suffix );
    SendMessageW( ctl->hwnd, EM_REPLACESEL, TRUE, (LPARAM)inserted );

    HeapFree( GetProcessHeap(), 0, inserted );
    HeapFree( GetProcessHeap(), 0, old );
}

static const struct w2s_kind kind_edit_single = { "edit.single", edit_snapshot, edit_apply };
static const struct w2s_kind kind_edit_password = { "edit.password", edit_snapshot, edit_apply };
static const struct w2s_kind kind_edit_number = { "edit.number", edit_snapshot, edit_apply };
static const struct w2s_kind kind_edit_readonly = { "edit.readonly", edit_snapshot, nothing_apply };

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

static void listview_snapshot( struct w2s_control *ctl, struct json *j )
{
    DWORD style = GetWindowLongW( ctl->hwnd, GWL_STYLE );
    int subitems[MAX_LV_COLUMNS] = { 0 }, columns = 1;
    int count = (int)SendMessageW( ctl->hwnd, LVM_GETITEMCOUNT, 0, 0 ), i, c;
    WCHAR text[512];

    if ((style & LVS_TYPEMASK) == LVS_REPORT)
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
}

static const struct w2s_kind kind_listview_list = { "listview.list", listview_snapshot, listview_apply };
static const struct w2s_kind kind_listview_report = { "listview.report", listview_snapshot, listview_apply };

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
        if (style & ES_MULTILINE) return NULL; /* NSTextView path: later */
        if (style & ES_PASSWORD) return &kind_edit_password;
        if (style & ES_READONLY) return &kind_edit_readonly;
        if (style & ES_NUMBER) return &kind_edit_number;
        return &kind_edit_single;
    }
    if (is_class( name, L"ComboBox" ))
    {
        if (style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) return NULL;
        if ((style & 3) == CBS_DROPDOWNLIST) return &kind_combobox_dropdownlist;
        return NULL;
    }
    if (is_class( name, L"ListBox" ))
    {
        if (style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE | LBS_NODATA | LBS_MULTICOLUMN | LBS_NOSEL)) return NULL;
        return (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? &kind_listbox_multi : &kind_listbox_single;
    }
    if (is_class( name, WC_LISTVIEWW ))
    {
        if (style & (LVS_OWNERDRAWFIXED | LVS_OWNERDATA)) return NULL;
        if ((style & LVS_TYPEMASK) == LVS_REPORT) return &kind_listview_report;
        if ((style & LVS_TYPEMASK) == LVS_LIST) return &kind_listview_list;
        return NULL;
    }
    if (is_class( name, STATUSCLASSNAMEW )) return &kind_statusbar;
    if (is_class( name, PROGRESS_CLASSW )) return (style & PBS_VERTICAL) ? NULL : &kind_progress;
    if (is_class( name, TRACKBAR_CLASSW )) return (style & TBS_VERT) ? NULL : &kind_trackbar;
    if (is_class( name, WC_TABCONTROLW ))
    {
        if (style & (TCS_OWNERDRAWFIXED | TCS_BUTTONS | TCS_VERTICAL | TCS_MULTILINE)) return NULL;
        return &kind_tab;
    }
    return NULL;
}
