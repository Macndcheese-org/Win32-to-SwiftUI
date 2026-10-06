/*
 * win32swiftui.dll: a dialog that is an alert (map: messagebox).
 *
 * A message box is an NSAlert, but many apps (and Windows Installer's packages: WiX's
 * cancel dialog) build the same thing by hand: a small dialog of an icon, some text and
 * a few push buttons. The system has that dialog, so it shows it: as the alert, a sheet
 * on the window it belongs to, and the button the person chooses is clicked in the
 * dialog, which goes on as if it had been chosen there. The dialog itself is never
 * shown. Anything else in the dialog (an edit, a checkbox, a list, a picture) and it is
 * the app's own dialog, which stays one.
 */
#include "w2s_pe.h"

#define ALERT_MAX_CHILDREN 12
#define ALERT_MAX_TEXTS 4

struct alert_text { RECT rc; WCHAR *text; };
struct alert_button { RECT rc; HWND hwnd; WCHAR *label; };

struct alert_dialog
{
    struct alert_text texts[ALERT_MAX_TEXTS];
    struct alert_button buttons[3];
    int text_count, button_count;
    const char *style;
};

static const WCHAR *class_base( const WCHAR *name )
{
    const WCHAR *bang = wcsrchr( name, '!' );
    return bang ? bang + 1 : name;
}

/* the text of a control without its '&' access keys (macOS has none) */
static WCHAR *plain_text( HWND hwnd )
{
    WCHAR buf[512], *s, *d;

    buf[0] = 0;
    GetWindowTextW( hwnd, buf, ARRAYSIZE(buf) );
    for (s = d = buf; *s; s++)
    {
        if (*s == '&' && s[1] != '&') continue;
        *d++ = *s;
        if (*s == '&') s++;
    }
    *d = 0;
    return strdupW( buf );
}

static void alert_free( struct alert_dialog *a )
{
    int i;

    for (i = 0; i < a->text_count; i++) HeapFree( GetProcessHeap(), 0, a->texts[i].text );
    for (i = 0; i < a->button_count; i++) HeapFree( GetProcessHeap(), 0, a->buttons[i].label );
    memset( a, 0, sizeof(*a) );
}

static BOOL before( const RECT *a, const RECT *b )
{
    return a->top != b->top ? a->top < b->top : a->left < b->left;
}

/* Whether the dialog is only an icon, text and one to three push buttons; what they are. */
static BOOL alert_collect( HWND dlg, struct alert_dialog *a )
{
    HWND child;
    int total = 0, i, j;

    memset( a, 0, sizeof(*a) );
    a->style = "informational";
    if (GetMenu( dlg ) || (GetWindowLongW( dlg, GWL_STYLE ) & WS_THICKFRAME)) return FALSE;

    for (child = GetWindow( dlg, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
    {
        DWORD style = GetWindowLongW( child, GWL_STYLE );
        WCHAR cls[64];
        const WCHAR *name;
        RECT rc;

        /* its own style: the dialog is not shown yet, and IsWindowVisible says no for all of it */
        if (!(style & WS_VISIBLE)) continue;
        if (++total > ALERT_MAX_CHILDREN || !GetClassNameW( child, cls, ARRAYSIZE(cls) )) goto fail;
        name = class_base( cls );
        GetWindowRect( child, &rc );
        MapWindowPoints( NULL, dlg, (POINT *)&rc, 2 );

        if (!_wcsicmp( name, L"Static" ))
        {
            switch (style & SS_TYPEMASK)
            {
            case SS_ICON:
            {
                HICON icon = (HICON)SendMessageW( child, STM_GETICON, 0, 0 );
                const char *spec = icon ? w2s_stock_icon( icon, NULL, 0, 0, NULL ) : NULL;

                if (spec && strstr( spec, "xmark.octagon" )) a->style = "critical";
                else if (spec && strstr( spec, "NSCaution" )) a->style = "warning";
                break;
            }
            case SS_LEFT: case SS_CENTER: case SS_RIGHT: case SS_SIMPLE: case SS_LEFTNOWORDWRAP:
            {
                WCHAR *text = plain_text( child );

                if (!text[0]) { HeapFree( GetProcessHeap(), 0, text ); break; }
                if (a->text_count == ALERT_MAX_TEXTS) { HeapFree( GetProcessHeap(), 0, text ); goto fail; }
                a->texts[a->text_count].rc = rc;
                a->texts[a->text_count++].text = text;
                break;
            }
            case SS_ETCHEDHORZ: case SS_ETCHEDVERT: case SS_ETCHEDFRAME: break;    /* a rule */
            default: goto fail;     /* a picture, a frame to draw in, an owner-drawn one */
            }
        }
        else if (!_wcsicmp( name, L"Button" ))
        {
            DWORD type = style & BS_TYPEMASK;

            if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) goto fail;
            if (a->button_count == ARRAYSIZE(a->buttons)) goto fail;
            a->buttons[a->button_count].rc = rc;
            a->buttons[a->button_count].hwnd = child;
            a->buttons[a->button_count++].label = plain_text( child );
        }
        else goto fail;
    }
    if (!a->text_count || !a->button_count) goto fail;
    /* a lone button that is not an OK: a progress or wait dialog's Cancel, which changes as it runs */
    if (a->button_count == 1)
    {
        const WCHAR *l = a->buttons[0].label;
        int id = GetDlgCtrlID( a->buttons[0].hwnd );

        if (id != IDOK && id != IDCLOSE && _wcsicmp( l, L"OK" ) && _wcsicmp( l, L"Close" )) goto fail;
    }
    /* as they are laid out: the text from the top, the buttons from the left */
    for (i = 1; i < a->text_count; i++)
        for (j = i; j > 0 && before( &a->texts[j].rc, &a->texts[j - 1].rc ); j--)
        { struct alert_text t = a->texts[j]; a->texts[j] = a->texts[j - 1]; a->texts[j - 1] = t; }
    for (i = 1; i < a->button_count; i++)
        for (j = i; j > 0 && before( &a->buttons[j].rc, &a->buttons[j - 1].rc ); j--)
        { struct alert_button t = a->buttons[j]; a->buttons[j] = a->buttons[j - 1]; a->buttons[j - 1] = t; }
    return TRUE;

fail:
    alert_free( a );
    return FALSE;
}

BOOL w2s_dialog_is_alert( HWND hwnd )
{
    struct alert_dialog a;

    if (!alert_collect( hwnd, &a )) return FALSE;
    alert_free( &a );
    return TRUE;
}

static BOOL is_cancel_label( const WCHAR *label )
{
    return !_wcsicmp( label, L"Cancel" ) || !_wcsicmp( label, L"No" ) || !_wcsicmp( label, L"Close" );
}

/* The dialog's alert; the button chosen is clicked in the dialog. FALSE: nothing to show
 * it with (the caller shows the dialog). */
BOOL w2s_dialog_run_alert( HWND hwnd )
{
    struct alert_dialog a;
    struct json j;
    char *result;
    double chosen = 0;
    HWND owner;
    int i, def = 0, cancel = -1, n;
    LRESULT defid = SendMessageW( hwnd, DM_GETDEFID, 0, 0 );
    BOOL ok;

    if (!alert_collect( hwnd, &a )) return FALSE;

    for (i = 0; i < a.button_count; i++)
    {
        int id = GetDlgCtrlID( a.buttons[i].hwnd );

        if ((GetWindowLongW( a.buttons[i].hwnd, GWL_STYLE ) & BS_TYPEMASK) == BS_DEFPUSHBUTTON ||
            (HIWORD( defid ) == DC_HASDEFID && id == (int)LOWORD( defid )))
            def = i;
        if (cancel < 0 && (id == IDCANCEL || is_cancel_label( a.buttons[i].label ))) cancel = i;
    }
    if (a.button_count == 1) cancel = 0;

    json_init( &j );
    json_obj_begin( &j );
    json_str( &j, "messageText", a.texts[0].text );
    {
        WCHAR rest[1024] = { 0 };

        for (i = 1; i < a.text_count; i++)
        {
            if (i > 1) lstrcatW( rest, L"\n\n" );
            lstrcatW( rest, a.texts[i].text );
        }
        json_str( &j, "informativeText", rest );
    }
    json_str_a( &j, "style", a.style );
    json_bool( &j, "floating", FALSE );
    json_arr_begin( &j, "buttons" );
    /* NSAlert: the first button is the default and sits on the right */
    for (n = 0; n < a.button_count; n++)
    {
        i = n == 0 ? def : n <= def ? n - 1 : n;
        json_obj_begin( &j );
        json_str( &j, "title", a.buttons[i].label );
        json_int( &j, "id", i + 1 );
        json_bool( &j, "default", n == 0 );
        json_bool( &j, "cancel", i == cancel );
        json_obj_end( &j );
    }
    json_arr_end( &j );
    json_obj_end( &j );

    owner = GetWindow( hwnd, GW_OWNER );
    if (!owner) owner = GetParent( hwnd );
    ok = w2s_run_request( "alert", owner, j.buf, &result );
    json_free( &j );
    if (ok)
    {
        i = json_get_num( result, "button", &chosen ) ? (int)chosen - 1 : cancel;
        HeapFree( GetProcessHeap(), 0, result );
        if (i >= 0 && i < a.button_count) SendMessageW( a.buttons[i].hwnd, BM_CLICK, 0, 0 );
    }
    alert_free( &a );
    return ok;
}
