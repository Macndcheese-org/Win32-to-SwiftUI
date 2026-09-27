/*
 * win32swiftui.dll: TaskDialogIndirect as an NSAlert (map: taskdialog).
 *
 * The main instruction is the alert's message, the content its informative
 * text, the verification check box its suppression button, the buttons its
 * buttons; radio buttons, the progress bar, expanded information and the
 * footer are SwiftUI in the alert's accessory view.
 *
 * The app talks to an open task dialog through its callback: TDN_* come from
 * here, and TDM_* go to the HWND the callback is given. That HWND is a hidden
 * window of ours whose messages become updates of the open alert. A click on
 * a native button comes back as an event first, so TDN_BUTTON_CLICKED can
 * keep the dialog open, as it can on Windows.
 */
#include "w2s_pe.h"

struct task_dialog
{
    struct w2s_request_handler handler;     /* first: the handler is the state */
    const TASKDIALOGCONFIG *config;
    HWND hwnd;              /* gets the app's TDM_* messages */
    UINT64 id;              /* the open alert */
    int button, radio;
    BOOL verified, closing, created;
    DWORD timer_start;      /* creation or the callback's last reset (S_FALSE) */
    DWORD last_tick;
    int progress_pos, progress_min, progress_max;
};

static const WCHAR class_name[] = L"Win32ToSwiftUI.TaskDialog";

static HRESULT notify( struct task_dialog *td, UINT code, WPARAM wparam, LPARAM lparam )
{
    if (!td->config->pfCallback) return S_OK;
    return td->config->pfCallback( td->hwnd, code, wparam, lparam, td->config->lpCallbackData );
}

static void update( struct task_dialog *td, const char *json )
{
    if (td->id) w2s_request_update( td->id, json );
}

/* a button chosen, by the user or TDM_CLICK_BUTTON: the app may keep the dialog open */
static void click( struct task_dialog *td, int id )
{
    char json[64];

    if (td->closing) return;
    if (notify( td, TDN_BUTTON_CLICKED, id, 0 ) == S_FALSE) return;
    td->button = id;
    td->closing = TRUE;
    snprintf( json, sizeof(json), "{\"close\":%d}", id );
    update( td, json );
}

static void set_radio( struct task_dialog *td, int id )
{
    char json[64];

    td->radio = id;
    snprintf( json, sizeof(json), "{\"radio\":%d}", id );
    update( td, json );
    notify( td, TDN_RADIO_BUTTON_CLICKED, id, 0 );
}

static void set_element( struct task_dialog *td, UINT element, const WCHAR *text )
{
    struct json j;
    WCHAR *s = w2s_resource_string( td->config->hInstance, text );

    json_init( &j );
    json_obj_begin( &j );
    json_int( &j, "element", element );
    json_str( &j, "text", s ? s : L"" );
    json_obj_end( &j );
    update( td, j.buf );
    json_free( &j );
    if (s) HeapFree( GetProcessHeap(), 0, s );
}

static LRESULT CALLBACK task_dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct task_dialog *td = (struct task_dialog *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    char json[128];
    LRESULT ret;

    if (!td) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case TDM_CLICK_BUTTON:
        click( td, (int)wparam );
        return 0;
    case TDM_ENABLE_BUTTON:
        snprintf( json, sizeof(json), "{\"enable\":[%d,%d]}", (int)wparam, lparam != 0 );
        update( td, json );
        return 0;
    case TDM_SET_ELEMENT_TEXT:
    case TDM_UPDATE_ELEMENT_TEXT:
        set_element( td, (UINT)wparam, (const WCHAR *)lparam );
        return msg == TDM_SET_ELEMENT_TEXT;
    case TDM_SET_PROGRESS_BAR_POS:
        ret = td->progress_pos;
        td->progress_pos = (int)wparam;
        snprintf( json, sizeof(json), "{\"progressPos\":%d}", (int)wparam );
        update( td, json );
        return ret;
    case TDM_SET_PROGRESS_BAR_RANGE:
        ret = MAKELONG( td->progress_min, td->progress_max );
        td->progress_min = LOWORD( lparam );
        td->progress_max = HIWORD( lparam );
        snprintf( json, sizeof(json), "{\"progressMin\":%d,\"progressMax\":%d}", td->progress_min, td->progress_max );
        update( td, json );
        return ret;
    case TDM_SET_PROGRESS_BAR_STATE:
        snprintf( json, sizeof(json), "{\"progressState\":%d}", (int)wparam );
        update( td, json );
        return TRUE;
    case TDM_SET_MARQUEE_PROGRESS_BAR:
        snprintf( json, sizeof(json), "{\"marquee\":%s}", wparam ? "true" : "false" );
        update( td, json );
        return TRUE;
    case TDM_SET_PROGRESS_BAR_MARQUEE:
        return TRUE;        /* the native bar always animates */
    case TDM_CLICK_RADIO_BUTTON:
        set_radio( td, (int)wparam );
        return 0;
    case TDM_ENABLE_RADIO_BUTTON:
        snprintf( json, sizeof(json), "{\"enableRadio\":[%d,%d]}", (int)wparam, lparam != 0 );
        update( td, json );
        return 0;
    case TDM_CLICK_VERIFICATION:
        td->verified = wparam != 0;
        snprintf( json, sizeof(json), "{\"verified\":%s}", wparam ? "true" : "false" );
        update( td, json );
        notify( td, TDN_VERIFICATION_CLICKED, wparam, 0 );
        return 0;
    case TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE:
    case TDM_UPDATE_ICON:
        return 0;           /* no UAC shields or live icons on macOS */
    case TDM_NAVIGATE_PAGE:
        TRACE( "TDM_NAVIGATE_PAGE is not supported by the native task dialog\n" );
        return 0;
    case WM_CLOSE:
        click( td, IDCANCEL );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void on_event( struct w2s_request_handler *handler, UINT64 id, const struct w2s_event *ev )
{
    struct task_dialog *td = (struct task_dialog *)handler;

    td->id = id;
    if (!strcmp( ev->type, "created" ) && !td->created)
    {
        td->created = TRUE;
        td->timer_start = td->last_tick = GetTickCount();
        notify( td, TDN_DIALOG_CONSTRUCTED, 0, 0 );
        notify( td, TDN_CREATED, 0, 0 );
    }
    else if (!strcmp( ev->type, "button" ) && ev->has_value) click( td, (int)ev->value );
    else if (!strcmp( ev->type, "radio" ) && ev->has_value)
    {
        td->radio = (int)ev->value;
        notify( td, TDN_RADIO_BUTTON_CLICKED, td->radio, 0 );
    }
    else if (!strcmp( ev->type, "verify" ) && ev->has_value)
    {
        td->verified = ev->value != 0;
        notify( td, TDN_VERIFICATION_CLICKED, td->verified, 0 );
    }
    else if (!strcmp( ev->type, "expando" ) && ev->has_value)
        notify( td, TDN_EXPANDO_BUTTON_CLICKED, ev->value != 0, 0 );
    else if (!strcmp( ev->type, "link" ) && ev->string)
        notify( td, TDN_HYPERLINK_CLICKED, 0, (LPARAM)ev->string );
}

static void on_idle( struct w2s_request_handler *handler, UINT64 id )
{
    struct task_dialog *td = (struct task_dialog *)handler;
    DWORD now = GetTickCount();

    td->id = id;
    if (!td->created || td->closing || !(td->config->dwFlags & TDF_CALLBACK_TIMER)) return;
    /* about every 200 ms, with the time since creation or the last reset */
    if (now - td->last_tick < 200) return;
    td->last_tick = now;
    if (notify( td, TDN_TIMER, now - td->timer_start, 0 ) == S_FALSE) td->timer_start = now;
}

static void json_text( struct json *j, const char *key, HINSTANCE inst, const WCHAR *text )
{
    WCHAR *s = w2s_resource_string( inst, text );
    json_str( j, key, s ? s : L"" );
    if (s) HeapFree( GetProcessHeap(), 0, s );
}

/* custom buttons: "Title\nNote" for command links; the alert shows the title */
static void json_button( struct json *j, const WCHAR *title, int id, BOOL is_default, BOOL is_cancel )
{
    WCHAR *copy = strdupW( title ? title : L"" ), *nl = wcschr( copy, '\n' );

    if (nl) *nl = 0;
    json_obj_begin( j );
    json_str( j, "title", copy );
    json_int( j, "id", id );
    json_bool( j, "default", is_default );
    json_bool( j, "cancel", is_cancel );
    json_obj_end( j );
    HeapFree( GetProcessHeap(), 0, copy );
}

static const struct { TASKDIALOG_COMMON_BUTTON_FLAGS flag; int id, string; } common_buttons[] =
{
    /* the order wine's (and Windows') task dialog puts them in; the strings are comctl32's */
    { TDCBF_OK_BUTTON, IDOK, 3003 },
    { TDCBF_YES_BUTTON, IDYES, 3000 },
    { TDCBF_NO_BUTTON, IDNO, 3001 },
    { TDCBF_RETRY_BUTTON, IDRETRY, 3002 },
    { TDCBF_CANCEL_BUTTON, IDCANCEL, 3004 },
    { TDCBF_CLOSE_BUTTON, IDCLOSE, 3005 },
};

/* NSAlert puts its first button on the right as the default: that one first,
 * then the rest in order */
static void json_buttons( struct json *j, const TASKDIALOGCONFIG *config, HINSTANCE strings )
{
    struct { const WCHAR *title; WCHAR *owned; int id; } list[64];
    int count = 0, i, def = -1;
    UINT b;
    BOOL has_cancel = FALSE;

    for (b = 0; b < config->cButtons && count < 32; b++)
    {
        WCHAR *title = w2s_resource_string( config->hInstance, config->pButtons[b].pszButtonText );
        list[count].title = title ? title : L"";
        list[count].owned = title;
        list[count++].id = config->pButtons[b].nButtonID;
    }
    for (i = 0; i < ARRAYSIZE(common_buttons); i++)
    {
        WCHAR buf[64];
        if (!(config->dwCommonButtons & common_buttons[i].flag)) continue;
        buf[0] = 0;
        if (!LoadStringW( strings, common_buttons[i].string, buf, ARRAYSIZE(buf) ))
        {
            WCHAR *label = w2s_msgbox_label( 0, common_buttons[i].id );
            if (label) { lstrcpynW( buf, label, ARRAYSIZE(buf) ); HeapFree( GetProcessHeap(), 0, label ); }
        }
        list[count].owned = strdupW( buf );
        list[count].title = list[count].owned;
        list[count++].id = common_buttons[i].id;
    }
    if (!count)
    {
        WCHAR buf[64] = L"OK";
        LoadStringW( strings, 3003, buf, ARRAYSIZE(buf) );
        list[0].owned = strdupW( buf );
        list[0].title = list[0].owned;
        list[count++].id = IDOK;
    }
    for (i = 0; i < count; i++)
    {
        if (list[i].id == config->nDefaultButton) def = i;
        if (list[i].id == IDCANCEL) has_cancel = TRUE;
    }
    if (def < 0) def = 0;
    json_arr_begin( j, "buttons" );
    json_button( j, list[def].title, list[def].id, TRUE, list[def].id == IDCANCEL );
    for (i = 0; i < count; i++)
        if (i != def) json_button( j, list[i].title, list[i].id, FALSE, list[i].id == IDCANCEL );
    json_arr_end( j );
    /* Escape: IDCANCEL, with or without such a button when cancelling is allowed */
    json_bool( j, "cancelable", has_cancel || (config->dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) );
    for (i = 0; i < count; i++) if (list[i].owned) HeapFree( GetProcessHeap(), 0, list[i].owned );
}

static const char *alert_style( const TASKDIALOGCONFIG *config )
{
    if (config->dwFlags & TDF_USE_HICON_MAIN) return "informational";
    if (config->pszMainIcon == TD_ERROR_ICON) return "critical";
    if (config->pszMainIcon == TD_WARNING_ICON) return "warning";
    return "informational";
}

/***********************************************************************
 *      W2STaskDialog  (win32swiftui.@)
 *
 * comctl32's TaskDialogIndirect asks first; strings is comctl32's module, for
 * the common buttons' localized labels. FALSE: wine's task dialog runs.
 */
BOOL WINAPI W2STaskDialog( const TASKDIALOGCONFIG *config, int *button, int *radio_button,
                           BOOL *verification_checked, HINSTANCE strings, HRESULT *hr )
{
    static ATOM atom;
    struct task_dialog td;
    struct json j;
    WCHAR *main_instruction, *content;
    BOOL links = (config->dwFlags & TDF_ENABLE_HYPERLINKS) != 0, ok;
    char *result;
    double value;
    UINT i;

    if (!config || config->cbSize != sizeof(*config)) return FALSE;
    if (!atom)
    {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = task_dialog_proc;
        wc.hInstance = GetModuleHandleW( L"win32swiftui.dll" );
        wc.lpszClassName = class_name;
        atom = RegisterClassW( &wc );
    }

    memset( &td, 0, sizeof(td) );
    td.handler.event = on_event;
    td.handler.idle = on_idle;
    td.config = config;
    td.progress_max = 100;
    td.verified = (config->dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
    td.radio = config->cRadioButtons && !(config->dwFlags & TDF_NO_DEFAULT_RADIO_BUTTON) ?
               (config->nDefaultRadioButton ? config->nDefaultRadioButton : config->pRadioButtons[0].nButtonID) : 0;
    td.button = IDCANCEL;
    td.hwnd = CreateWindowExW( 0, class_name, NULL, WS_POPUP, 0, 0, 0, 0, config->hwndParent, NULL,
                               GetModuleHandleW( L"win32swiftui.dll" ), NULL );
    if (!td.hwnd) return FALSE;
    SetWindowLongPtrW( td.hwnd, GWLP_USERDATA, (LONG_PTR)&td );

    main_instruction = w2s_resource_string( config->hInstance, config->pszMainInstruction );
    content = w2s_resource_string( config->hInstance, config->pszContent );

    json_init( &j );
    json_obj_begin( &j );
    /* no main instruction: the content is the message, as a message box's text is */
    if (main_instruction && main_instruction[0])
    {
        json_str( &j, "messageText", main_instruction );
        if (content && !links) json_str( &j, "informativeText", content );
        if (content && links) json_str( &j, "content", content );
    }
    else if (content && !links) json_str( &j, "messageText", content );
    else if (content) json_str( &j, "content", content );
    json_bool( &j, "links", links );
    json_str_a( &j, "style", alert_style( config ) );
    json_buttons( &j, config, strings );
    if (config->cRadioButtons)
    {
        json_arr_begin( &j, "radios" );
        for (i = 0; i < config->cRadioButtons; i++)
        {
            WCHAR *title = w2s_resource_string( config->hInstance, config->pRadioButtons[i].pszButtonText );
            json_obj_begin( &j );
            json_str( &j, "title", title ? title : L"" );
            json_int( &j, "id", config->pRadioButtons[i].nButtonID );
            json_obj_end( &j );
            if (title) HeapFree( GetProcessHeap(), 0, title );
        }
        json_arr_end( &j );
        json_int( &j, "radio", td.radio );
    }
    if (config->pszVerificationText)
    {
        json_text( &j, "verification", config->hInstance, config->pszVerificationText );
        json_bool( &j, "verified", td.verified );
    }
    if (config->pszExpandedInformation)
    {
        json_text( &j, "expandedText", config->hInstance, config->pszExpandedInformation );
        json_bool( &j, "expanded", (config->dwFlags & TDF_EXPANDED_BY_DEFAULT) != 0 );
        json_bool( &j, "expandFooter", (config->dwFlags & TDF_EXPAND_FOOTER_AREA) != 0 );
        if (config->pszExpandedControlText) json_text( &j, "collapseLabel", config->hInstance, config->pszExpandedControlText );
        else
        {
            WCHAR buf[64] = L"Hide details";
            LoadStringW( strings, 3020, buf, ARRAYSIZE(buf) );
            json_str( &j, "collapseLabel", buf );
        }
        if (config->pszCollapsedControlText) json_text( &j, "expandLabel", config->hInstance, config->pszCollapsedControlText );
        else
        {
            WCHAR buf[64] = L"See details";
            LoadStringW( strings, 3021, buf, ARRAYSIZE(buf) );
            json_str( &j, "expandLabel", buf );
        }
    }
    if (config->pszFooter) json_text( &j, "footer", config->hInstance, config->pszFooter );
    json_bool( &j, "progress", (config->dwFlags & (TDF_SHOW_PROGRESS_BAR | TDF_SHOW_MARQUEE_PROGRESS_BAR)) != 0 );
    json_bool( &j, "marquee", (config->dwFlags & TDF_SHOW_MARQUEE_PROGRESS_BAR) != 0 );
    json_obj_end( &j );
    if (main_instruction) HeapFree( GetProcessHeap(), 0, main_instruction );
    if (content) HeapFree( GetProcessHeap(), 0, content );

    if (!(config->dwFlags & TDF_NO_SET_FOREGROUND) && config->hwndParent) SetForegroundWindow( config->hwndParent );
    ok = w2s_run_request_ex( "taskdialog", config->hwndParent, j.buf, &td.handler, &result );
    json_free( &j );
    if (!ok)
    {
        DestroyWindow( td.hwnd );
        return FALSE;
    }
    if (json_get_num( result, "button", &value )) td.button = (int)value;
    if (json_get_num( result, "radio", &value )) td.radio = (int)value;
    if (json_get_num( result, "verified", &value )) td.verified = value != 0;
    HeapFree( GetProcessHeap(), 0, result );

    notify( &td, TDN_DESTROYED, 0, 0 );
    SetWindowLongPtrW( td.hwnd, GWLP_USERDATA, 0 );
    DestroyWindow( td.hwnd );
    if (button) *button = td.button;
    if (radio_button) *radio_button = td.radio;
    if (verification_checked) *verification_checked = td.verified;
    *hr = S_OK;
    return TRUE;
}
