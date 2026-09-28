/*
 * win32swiftui.dll: whole dialogs replaced by macOS panels.
 *
 * The panel is started in win32swiftui.so and shown by AppKit without a modal
 * session (as a sheet on the owner, or its own window). Modality is ours: the
 * calling thread disables the owner and pumps messages until the answer
 * arrives, as DialogBox does, so the app's other windows keep painting.
 */
#include "w2s_pe.h"

static WCHAR *(CDECL *p_wine_get_dos_file_name)( const char * );
static char *(CDECL *p_wine_get_unix_file_name)( const WCHAR * );

static void init_path_funcs(void)
{
    HMODULE kernel32 = GetModuleHandleW( L"kernel32.dll" );
    if (p_wine_get_dos_file_name) return;
    p_wine_get_dos_file_name = (void *)GetProcAddress( kernel32, "wine_get_dos_file_name" );
    p_wine_get_unix_file_name = (void *)GetProcAddress( kernel32, "wine_get_unix_file_name" );
}

struct thread_windows
{
    HWND *list;
    int count, cap;
};

static BOOL CALLBACK disable_proc( HWND hwnd, LPARAM lparam )
{
    struct thread_windows *tw = (struct thread_windows *)lparam;
    if (!IsWindowEnabled( hwnd )) return TRUE;
    if (tw->count == tw->cap)
    {
        tw->cap = tw->cap ? tw->cap * 2 : 16;
        tw->list = tw->list ? HeapReAlloc( GetProcessHeap(), 0, tw->list, tw->cap * sizeof(HWND) )
                            : HeapAlloc( GetProcessHeap(), 0, tw->cap * sizeof(HWND) );
    }
    tw->list[tw->count++] = hwnd;
    EnableWindow( hwnd, FALSE );
    return TRUE;
}

/* one poll: the result (done), the events raised meanwhile, or nothing */
static char *poll_request( UINT64 id, BOOL *done, UINT32 *len )
{
    struct w2s_request_poll_params poll;
    char small[4096], *buf;

    poll.id = id;
    poll.buffer = W2S_PTR( small );
    poll.size = sizeof(small);
    poll.len = 0;
    poll.done = 0;
    w2s_call( unix_w2s_request_poll, &poll );
    while (poll.len > poll.size)
    {
        char *big = HeapAlloc( GetProcessHeap(), 0, poll.len );
        poll.buffer = W2S_PTR( big );
        poll.size = poll.len;
        poll.len = 0;
        poll.done = 0;
        w2s_call( unix_w2s_request_poll, &poll );
        if (poll.len <= poll.size)
        {
            *done = poll.done;
            *len = poll.len;
            return big;
        }
        HeapFree( GetProcessHeap(), 0, big );   /* it grew meanwhile */
    }
    *done = poll.done;
    *len = poll.len;
    buf = HeapAlloc( GetProcessHeap(), 0, poll.len + 1 );
    memcpy( buf, small, poll.len );
    buf[poll.len] = 0;
    return buf;
}

void w2s_request_update( UINT64 id, const char *json )
{
    struct w2s_request_update_params params = { id, W2S_PTR( json ), strlen( json ) };
    w2s_call( unix_w2s_request_update, &params );
}

BOOL w2s_run_request( const char *kind, HWND owner, const char *json, char **result )
{
    return w2s_run_request_ex( kind, owner, json, NULL, result );
}

BOOL w2s_run_request_ex( const char *kind, HWND owner, const char *json, struct w2s_request_handler *handler,
                         char **result )
{
    struct w2s_request_start_params start;
    struct w2s_host owner_host = { 0 };
    struct thread_windows disabled = { 0 };
    BOOL have_host = FALSE;
    MSG msg;
    int i;

    *result = NULL;
    if (owner) owner = GetAncestor( owner, GA_ROOT );
    /* the owner's NSWindow, for a sheet */
    if (owner && IsWindowVisible( owner )) have_host = w2s_get_host( owner, &owner_host );

    start.kind = W2S_PTR( kind );
    start.window = have_host ? owner_host.window : 0;
    start.json = W2S_PTR( json );
    start.json_len = strlen( json );
    start.id = 0;
    if (w2s_call( unix_w2s_request_start, &start ) || !start.id)
    {
        if (have_host) w2s_release_host( owner, &owner_host );
        return FALSE;
    }

    if (owner) disable_proc( owner, (LPARAM)&disabled );
    else EnumThreadWindows( GetCurrentThreadId(), disable_proc, (LPARAM)&disabled );

    for (;;)
    {
        BOOL done;
        UINT32 len;
        char *text = poll_request( start.id, &done, &len );

        if (done)
        {
            *result = text;
            break;
        }
        if (len && handler && handler->event)
        {
            struct w2s_event *events;
            int count = json_parse_events( text, &events );
            TRACE( "request %s events %s\n", kind, text );
            for (i = 0; i < count; i++) handler->event( handler, start.id, &events[i] );
            json_free_events( events, count );
        }
        HeapFree( GetProcessHeap(), 0, text );
        if (handler && handler->idle) handler->idle( handler, start.id );

        MsgWaitForMultipleObjectsEx( 0, NULL, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE );
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT)
            {
                PostQuitMessage( (int)msg.wParam );
                break;
            }
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    }

    for (i = 0; i < disabled.count; i++) EnableWindow( disabled.list[i], TRUE );
    if (disabled.list) HeapFree( GetProcessHeap(), 0, disabled.list );
    if (owner) SetActiveWindow( owner );
    if (have_host) w2s_release_host( owner, &owner_host );
    TRACE( "request %s -> %s\n", kind, *result ? *result : "(none)" );
    return *result != NULL;
}

/* ---------- MessageBox ---------- */

/* The label of an item in one of wine's own dialog templates, in wine's
 * current language (lang 0: the thread's), without the '&': user32's MSGBOX
 * buttons, comdlg32's colour and font dialogs. So a French wine gets French
 * buttons. */
WCHAR *w2s_dialog_label( HMODULE module, const WCHAR *dialog, LANGID lang, int id )
{
    HRSRC res = lang ? FindResourceExW( module, (LPCWSTR)RT_DIALOG, dialog, lang )
                     : FindResourceW( module, dialog, (LPCWSTR)RT_DIALOG );
    const BYTE *p;
    const DLGTEMPLATE *tmpl;
    DWORD style;
    WORD count, i;

    if (!module) return NULL;
    if (!res && !(res = FindResourceExW( module, (LPCWSTR)RT_DIALOG, dialog, LANG_NEUTRAL ))) return NULL;
    if (!(tmpl = LockResource( LoadResource( module, res ) ))) return NULL;
    if (*(const WORD *)((const BYTE *)tmpl + 2) == 0xffff) return NULL;   /* a DIALOGEX */
    style = tmpl->style;
    count = tmpl->cdit;
    p = (const BYTE *)(tmpl + 1);

#define SKIP_SZ_OR_ORD() do { if (*(const WORD *)p == 0xffff) p += 4; else p += (wcslen( (const WCHAR *)p ) + 1) * sizeof(WCHAR); } while (0)
    SKIP_SZ_OR_ORD(); /* menu */
    SKIP_SZ_OR_ORD(); /* class */
    p += (wcslen( (const WCHAR *)p ) + 1) * sizeof(WCHAR); /* title */
    if (style & DS_SETFONT)
    {
        p += sizeof(WORD);
        p += (wcslen( (const WCHAR *)p ) + 1) * sizeof(WCHAR);
    }
    for (i = 0; i < count; i++)
    {
        const DLGITEMTEMPLATE *item;
        const WCHAR *title;
        WORD extra;

        p = (const BYTE *)(((UINT_PTR)p + 3) & ~(UINT_PTR)3);
        item = (const DLGITEMTEMPLATE *)p;
        p = (const BYTE *)(item + 1);
        SKIP_SZ_OR_ORD(); /* class */
        title = (const WCHAR *)p;
        SKIP_SZ_OR_ORD();
        extra = *(const WORD *)p;
        p += sizeof(WORD) + extra;
        if (item->id == id && *(const WORD *)title != 0xffff)
        {
            /* no '&' access keys on macOS */
            WCHAR *out = strdupW( title ), *s, *d;
            for (s = d = out; *s; s++) if (*s != '&') *d++ = *s;
            *d = 0;
            return out;
        }
    }
#undef SKIP_SZ_OR_ORD
    return NULL;
}

/* the button labels wine shows in a message box */
WCHAR *w2s_msgbox_label( LANGID lang, int id )
{
    return w2s_dialog_label( GetModuleHandleW( L"user32.dll" ), L"MSGBOX", lang ? lang : LANG_NEUTRAL, id );
}

WCHAR *w2s_resource_string( HINSTANCE inst, const WCHAR *s )
{
    WCHAR buf[1024];
    if (!s) return NULL;
    if (!IS_INTRESOURCE( s )) return strdupW( s );
    if (!LoadStringW( inst, LOWORD( s ), buf, ARRAYSIZE(buf) )) return NULL;
    return strdupW( buf );
}

static void add_button( struct json *j, LANGID lang, int id, BOOL is_default, BOOL is_cancel )
{
    WCHAR *label = w2s_msgbox_label( lang, id );
    json_obj_begin( j );
    json_str( j, "title", label ? label : L"OK" );
    json_int( j, "id", id );
    json_bool( j, "default", is_default );
    json_bool( j, "cancel", is_cancel );
    json_obj_end( j );
    if (label) HeapFree( GetProcessHeap(), 0, label );
}

/***********************************************************************
 *      W2SMessageBox  (win32swiftui.@)
 */
BOOL WINAPI W2SMessageBox( const MSGBOXPARAMSW *params, INT *ret )
{
    static const int sets[][3] =
    {
        { IDOK },                          /* MB_OK */
        { IDOK, IDCANCEL },                /* MB_OKCANCEL */
        { IDABORT, IDRETRY, IDIGNORE },    /* MB_ABORTRETRYIGNORE */
        { IDYES, IDNO, IDCANCEL },         /* MB_YESNOCANCEL */
        { IDYES, IDNO },                   /* MB_YESNO */
        { IDRETRY, IDCANCEL },             /* MB_RETRYCANCEL */
        { IDCANCEL, IDTRYAGAIN, IDCONTINUE }, /* MB_CANCELTRYCONTINUE */
    };
    UINT type = params->dwStyle & MB_TYPEMASK, icon = params->dwStyle & MB_ICONMASK;
    UINT def = (params->dwStyle & MB_DEFMASK) >> 8;
    WCHAR *text, *caption, module[MAX_PATH], *exe;
    const char *style;
    struct json j;
    char *result;
    double button;
    int n = 0, i, cancel_id = 0;
    BOOL is_title;

    if (type >= ARRAYSIZE(sets) || (params->dwStyle & MB_HELP) || (params->dwStyle & MB_SERVICE_NOTIFICATION))
        return FALSE;
    for (i = 0; i < 3 && sets[type][i]; i++) n++;
    for (i = 0; i < n; i++) if (sets[type][i] == IDCANCEL) cancel_id = IDCANCEL;
    if (!cancel_id && n == 1) cancel_id = sets[type][0]; /* Escape closes a lone OK */
    if (def >= (UINT)n) def = 0;

    text = w2s_resource_string( params->hInstance, params->lpszText );
    caption = w2s_resource_string( params->hInstance, params->lpszCaption );

    /* macOS alerts show the app icon, so a caption that's just the program's
     * name adds nothing: the text becomes the bold message. Otherwise the
     * caption ("Error", "Save changes?") is the message and the text its detail. */
    is_title = caption && caption[0];
    if (is_title && GetModuleFileNameW( NULL, module, MAX_PATH ) && (exe = wcsrchr( module, '\\' )))
    {
        WCHAR *dot = wcsrchr( ++exe, '.' );
        if (dot) *dot = 0;
        if (!_wcsicmp( exe, caption )) is_title = FALSE;
    }
    if (is_title && params->hwndOwner)
    {
        WCHAR owner_title[256];
        if (GetWindowTextW( GetAncestor( params->hwndOwner, GA_ROOT ), owner_title, ARRAYSIZE(owner_title) ) &&
            !_wcsicmp( owner_title, caption ))
            is_title = FALSE;
    }

    style = icon == MB_ICONERROR ? "critical" : icon == MB_ICONWARNING ? "warning" : "informational";

    json_init( &j );
    json_obj_begin( &j );
    json_str( &j, "messageText", is_title ? caption : (text ? text : L"") );
    json_str( &j, "informativeText", is_title ? (text ? text : L"") : L"" );
    json_str_a( &j, "style", style );
    json_bool( &j, "floating", (params->dwStyle & (MB_TOPMOST | MB_SYSTEMMODAL)) != 0 );
    json_arr_begin( &j, "buttons" );
    /* NSAlert: the first button is the default and sits on the right */
    add_button( &j, params->dwLanguageId, sets[type][def], TRUE, sets[type][def] == cancel_id );
    for (i = 0; i < n; i++)
        if ((UINT)i != def) add_button( &j, params->dwLanguageId, sets[type][i], FALSE, sets[type][i] == cancel_id );
    json_arr_end( &j );
    json_obj_end( &j );

    if (params->dwStyle & MB_SETFOREGROUND) SetForegroundWindow( params->hwndOwner ? params->hwndOwner : GetActiveWindow() );

    if (!w2s_run_request( "alert", params->hwndOwner, j.buf, &result ))
    {
        json_free( &j );
        if (text) HeapFree( GetProcessHeap(), 0, text );
        if (caption) HeapFree( GetProcessHeap(), 0, caption );
        return FALSE;
    }
    *ret = json_get_num( result, "button", &button ) ? (int)button : IDCANCEL;
    HeapFree( GetProcessHeap(), 0, result );
    json_free( &j );
    if (text) HeapFree( GetProcessHeap(), 0, text );
    if (caption) HeapFree( GetProcessHeap(), 0, caption );
    return TRUE;
}

/* ---------- ShellAbout ---------- */

/* a string of the program's version resource, in its first language */
static WCHAR *version_string( void *info, const WCHAR *name )
{
    struct { WORD lang, codepage; } *trans;
    WCHAR path[80], *value;
    UINT len;

    if (!VerQueryValueW( info, L"\\VarFileInfo\\Translation", (void **)&trans, &len ) || len < sizeof(*trans))
        return NULL;
    wsprintfW( path, L"\\StringFileInfo\\%04x%04x\\%s", trans->lang, trans->codepage, name );
    if (!VerQueryValueW( info, path, (void **)&value, &len ) || !len || !value[0]) return NULL;
    return value;
}

/* the icon as the About panel shows it: a stock icon's macOS image, else its
 * pixels, at the panel's size when it comes from a resource that has it */
static void json_about_icon( struct json *j, HICON icon )
{
    enum { SIZE = 128 };   /* 64 points on a Retina screen */
    ICONINFOEXW info = { sizeof(info) };
    HICON large = NULL;
    const char *spec;
    BITMAP bm;
    BYTE *bits;
    int w = 0, h = 0;

    if (GetIconInfoExW( icon, &info ))
    {
        HMODULE module = info.szModName[0] ? GetModuleHandleW( info.szModName ) : NULL;
        const WCHAR *res = info.wResID ? MAKEINTRESOURCEW( info.wResID ) : info.szResName[0] ? info.szResName : NULL;

        if (info.hbmColor && GetObjectW( info.hbmColor, sizeof(bm), &bm )) { w = bm.bmWidth; h = bm.bmHeight; }
        else if (info.hbmMask && GetObjectW( info.hbmMask, sizeof(bm), &bm )) { w = bm.bmWidth; h = bm.bmHeight / 2; }
        if (info.hbmColor) DeleteObject( info.hbmColor );
        if (info.hbmMask) DeleteObject( info.hbmMask );
        if (module && res && w < SIZE && (large = LoadImageW( module, res, IMAGE_ICON, SIZE, SIZE, 0 )))
            w = h = SIZE;
    }
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    if (!(bits = w2s_image_bgra( large ? large : icon, NULL, w, h ))) goto done;
    if ((spec = w2s_stock_icon( icon, bits, w, h, NULL ))) json_str_a( j, "iconSymbol", spec );
    else
    {
        json_int( j, "iconWidth", w );
        json_int( j, "iconHeight", h );
        json_base64( j, "iconBGRA", bits, (size_t)w * h * 4 );
    }
    HeapFree( GetProcessHeap(), 0, bits );
done:
    if (large) DestroyIcon( large );
}

static BOOL CALLBACK first_icon_group( HMODULE module, const WCHAR *type, WCHAR *name, LONG_PTR param )
{
    *(HICON *)param = LoadImageW( module, name, IMAGE_ICON, 128, 128, 0 );
    return FALSE;
}

/* the program's own icon, as Finder and the Dock show it */
static HICON program_icon(void)
{
    HICON icon = NULL;

    EnumResourceNamesW( GetModuleHandleW( NULL ), (const WCHAR *)RT_GROUP_ICON, first_icon_group, (LONG_PTR)&icon );
    return icon;
}

/***********************************************************************
 *      W2SShellAbout  (win32swiftui.@)
 *
 * ShellAbout as the standard macOS About panel: the program's name and icon,
 * its version and copyright (from its version resource, as a Mac app's come
 * from its Info.plist) and the caller's text as credits. It returns when the
 * panel closes, as wine's dialog does. ShellAboutA goes through ShellAboutW.
 */
BOOL WINAPI W2SShellAbout( HWND owner, const WCHAR *app, const WCHAR *other, HICON icon, BOOL *ret )
{
    WCHAR module[MAX_PATH], *name = NULL, *hash, *credits;
    const WCHAR *line = NULL, *value;
    void *info = NULL;
    DWORD size, handle;
    HICON own;
    struct json j;
    char *result;
    size_t len;

    /* "title#first line": the title is the program's name */
    if (app && (name = HeapAlloc( GetProcessHeap(), 0, (wcslen( app ) + 1) * sizeof(WCHAR) )))
    {
        wcscpy( name, app );
        if ((hash = wcschr( name, '#' )))
        {
            *hash = 0;
            if (hash[1] && wcscmp( hash + 1, name )) line = hash + 1;
        }
    }
    len = (line ? wcslen( line ) + 2 : 0) + (other ? wcslen( other ) : 0) + 1;
    if (!(credits = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) )))
    {
        HeapFree( GetProcessHeap(), 0, name );
        return FALSE;
    }
    credits[0] = 0;
    if (line) wcscat( wcscat( credits, line ), other && other[0] ? L"\n" : L"" );
    if (other) wcscat( credits, other );

    if (GetModuleFileNameW( NULL, module, MAX_PATH ) && (size = GetFileVersionInfoSizeW( module, &handle )) &&
        (info = HeapAlloc( GetProcessHeap(), 0, size )) && !GetFileVersionInfoW( module, 0, size, info ))
    {
        HeapFree( GetProcessHeap(), 0, info );
        info = NULL;
    }

    json_init( &j );
    json_obj_begin( &j );
    json_str( &j, "name", name ? name : L"" );
    json_str( &j, "credits", credits );
    if (info && ((value = version_string( info, L"ProductVersion" )) || (value = version_string( info, L"FileVersion" ))))
        json_str( &j, "version", value );
    if (info && (value = version_string( info, L"LegalCopyright" ))) json_str( &j, "copyright", value );
    if (icon) json_about_icon( &j, icon );
    else if ((own = program_icon()))
    {
        json_about_icon( &j, own );
        DestroyIcon( own );
    }
    /* no icon at all: macOS's generic application icon */
    else json_str_a( &j, "iconSymbol", "uttype:com.apple.application-bundle" );
    json_obj_end( &j );
    HeapFree( GetProcessHeap(), 0, info );
    HeapFree( GetProcessHeap(), 0, credits );
    HeapFree( GetProcessHeap(), 0, name );

    if (!w2s_run_request( "about", owner, j.buf, &result ))
    {
        json_free( &j );
        return FALSE;
    }
    json_free( &j );
    HeapFree( GetProcessHeap(), 0, result );
    *ret = TRUE;
    return TRUE;
}

/* ---------- GetOpenFileName / GetSaveFileName ---------- */

static void json_unix_path( struct json *j, const char *key, const WCHAR *path )
{
    char *unix_path;
    if (!path || !path[0] || !p_wine_get_unix_file_name) return;
    if (!(unix_path = p_wine_get_unix_file_name( path ))) return;
    json_str_a( j, key, unix_path );
    HeapFree( GetProcessHeap(), 0, unix_path );
}

/* "Text files\0*.txt;*.log\0All\0*.*\0\0" -> [{name, exts:[txt, log]}, {name, exts:[]}] */
static void json_filters( struct json *j, const WCHAR *filter )
{
    json_arr_begin( j, "filters" );
    while (filter && *filter)
    {
        const WCHAR *name = filter, *spec = name + wcslen( name ) + 1, *p;
        if (!*spec) break;
        json_obj_begin( j );
        json_str( j, "name", name );
        json_arr_begin( j, "exts" );
        for (p = spec; *p;)
        {
            const WCHAR *end = wcschr( p, ';' ), *dot;
            WCHAR ext[64];
            int len = end ? (int)(end - p) : (int)wcslen( p );
            while (len && *p == ' ') { p++; len--; }
            dot = p;
            while (dot < p + len && *dot != '.') dot++;
            if (dot < p + len && len - (int)(dot - p) - 1 > 0 && len - (int)(dot - p) - 1 < 64)
            {
                int elen = len - (int)(dot - p) - 1;
                memcpy( ext, dot + 1, elen * sizeof(WCHAR) );
                ext[elen] = 0;
                if (!wcschr( ext, '*' ) && !wcschr( ext, '?' )) json_str( j, NULL, ext );
            }
            if (!end) break;
            p = end + 1;
        }
        json_arr_end( j );
        json_obj_end( j );
        filter = spec + wcslen( spec ) + 1;
    }
    json_arr_end( j );
}

static void set_path_result( OPENFILENAMEW *ofn, const WCHAR *path )
{
    const WCHAR *name = wcsrchr( path, '\\' ), *ext;
    name = name ? name + 1 : path;
    ext = wcsrchr( name, '.' );
    ofn->nFileOffset = (WORD)(name - path);
    ofn->nFileExtension = ext ? (WORD)(ext - path + 1) : 0;
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle)
    {
        lstrcpynW( ofn->lpstrFileTitle, name, ofn->nMaxFileTitle );
    }
}

static BOOL file_dialog( OPENFILENAMEW *ofn, BOOL save, DWORD fos, BOOL *ret );

/***********************************************************************
 *      W2SFileDialog  (win32swiftui.@)
 */
BOOL WINAPI W2SFileDialog( OPENFILENAMEW *ofn, BOOL save, BOOL *ret )
{
    return file_dialog( ofn, save, 0, ret );
}

/***********************************************************************
 *      W2SItemDialog  (win32swiftui.@)
 *
 * IFileOpenDialog / IFileSaveDialog (comdlg32's itemdlg.c): the dialog's
 * state comes as an OPENFILENAMEW plus its FOS_* options; the result is the
 * same as GetOpenFileName's ("dir\0name\0name\0\0" for several files).
 */
BOOL WINAPI W2SItemDialog( OPENFILENAMEW *ofn, BOOL save, DWORD fos, BOOL *ret )
{
    ofn->Flags = OFN_EXPLORER;
    if (fos & FOS_ALLOWMULTISELECT) ofn->Flags |= OFN_ALLOWMULTISELECT;
    if (fos & FOS_FORCESHOWHIDDEN) ofn->Flags |= OFN_FORCESHOWHIDDEN;
    if (fos & FOS_NODEREFERENCELINKS) ofn->Flags |= OFN_NODEREFERENCELINKS;
    if (fos & FOS_NOCHANGEDIR) ofn->Flags |= OFN_NOCHANGEDIR;
    return file_dialog( ofn, save, fos, ret );
}

static BOOL file_dialog( OPENFILENAMEW *ofn, BOOL save, DWORD fos, BOOL *ret )
{
    struct json j;
    char *result;
    WCHAR **paths, *initial_dir = NULL, *slash, dir[MAX_PATH];
    double filter_index;
    int count, i;
    BOOL multi = (ofn->Flags & OFN_ALLOWMULTISELECT) && !save;
    BOOL folders = (fos & FOS_PICKFOLDERS) && !save;

    if (ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) return FALSE;
    if (multi && !(ofn->Flags & OFN_EXPLORER)) return FALSE; /* old space-separated list */
    if (!ofn->lpstrFile || !ofn->nMaxFile) return FALSE;
    init_path_funcs();
    if (!p_wine_get_dos_file_name || !p_wine_get_unix_file_name) return FALSE;

    json_init( &j );
    json_obj_begin( &j );
    json_bool( &j, "save", save );
    json_bool( &j, "multi", multi );
    json_bool( &j, "showHidden", (ofn->Flags & OFN_FORCESHOWHIDDEN) != 0 );
    json_bool( &j, "resolveLinks", !(ofn->Flags & OFN_NODEREFERENCELINKS) );
    json_bool( &j, "folders", folders );
    json_bool( &j, "strict", (fos & FOS_STRICTFILETYPES) != 0 );
    if (ofn->lpstrTitle) json_str( &j, "title", ofn->lpstrTitle );
    if (!folders) json_filters( &j, ofn->lpstrFilter );
    json_int( &j, "filterIndex", ofn->nFilterIndex ? ofn->nFilterIndex : 1 );
    if (ofn->lpstrDefExt) json_str( &j, "defExt", ofn->lpstrDefExt );

    /* start folder: lpstrInitialDir, else the folder part of lpstrFile */
    if (ofn->lpstrInitialDir && ofn->lpstrInitialDir[0]) initial_dir = strdupW( ofn->lpstrInitialDir );
    if (ofn->lpstrFile[0])
    {
        WCHAR *file = strdupW( ofn->lpstrFile );
        if ((slash = wcsrchr( file, '\\' )))
        {
            *slash = 0;
            if (!initial_dir) initial_dir = strdupW( file );
            json_str( &j, "name", slash + 1 );
        }
        else json_str( &j, "name", file );
        HeapFree( GetProcessHeap(), 0, file );
    }
    if (!initial_dir && GetCurrentDirectoryW( ARRAYSIZE(dir), dir )) initial_dir = strdupW( dir );
    json_unix_path( &j, "dir", initial_dir );
    if (initial_dir) HeapFree( GetProcessHeap(), 0, initial_dir );
    json_obj_end( &j );

    if (!w2s_run_request( save ? "save" : "open", ofn->hwndOwner, j.buf, &result ))
    {
        json_free( &j );
        return FALSE;
    }
    json_free( &j );

    count = json_get_str_array( result, "paths", &paths );
    if (json_get_num( result, "filterIndex", &filter_index )) ofn->nFilterIndex = (DWORD)filter_index;
    HeapFree( GetProcessHeap(), 0, result );

    *ret = FALSE;
    if (count > 0)
    {
        WCHAR **dos = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*dos) );
        size_t need;

        for (i = 0; i < count; i++)
        {
            char *utf8 = utf8_from_wide( paths[i], -1 );
            dos[i] = p_wine_get_dos_file_name( utf8 );
            HeapFree( GetProcessHeap(), 0, utf8 );
        }
        if (count == 1 && dos[0])
        {
            need = wcslen( dos[0] ) + 1;
            if (save && ofn->lpstrDefExt && ofn->lpstrDefExt[0] && !wcschr( wcsrchr( dos[0], '\\' ) ? wcsrchr( dos[0], '\\' ) : dos[0], '.' ))
                need += wcslen( ofn->lpstrDefExt ) + 1;
            if (need > ofn->nMaxFile)
            {
                if (ofn->nMaxFile >= 2) *(WORD *)ofn->lpstrFile = (WORD)need;
            }
            else
            {
                wcscpy( ofn->lpstrFile, dos[0] );
                if (need > wcslen( dos[0] ) + 1)
                {
                    wcscat( ofn->lpstrFile, L"." );
                    wcscat( ofn->lpstrFile, ofn->lpstrDefExt );
                }
                set_path_result( ofn, ofn->lpstrFile );
                *ret = TRUE;
            }
        }
        else if (count > 1)
        {
            /* "dir\0name1\0name2\0\0" */
            WCHAR *first = dos[0], *sep = first ? wcsrchr( first, '\\' ) : NULL;
            size_t dirlen = sep ? (size_t)(sep - first) : 0, total = dirlen + 2;
            for (i = 0; i < count; i++) if (dos[i]) total += wcslen( wcsrchr( dos[i], '\\' ) + 1 ) + 1;
            if (total <= ofn->nMaxFile && sep)
            {
                WCHAR *p = ofn->lpstrFile;
                memcpy( p, first, dirlen * sizeof(WCHAR) );
                p += dirlen;
                *p++ = 0;
                for (i = 0; i < count; i++)
                {
                    const WCHAR *name;
                    if (!dos[i]) continue;
                    name = wcsrchr( dos[i], '\\' ) + 1;
                    wcscpy( p, name );
                    p += wcslen( name ) + 1;
                }
                *p = 0;
                ofn->nFileOffset = (WORD)(dirlen + 1);
                ofn->nFileExtension = 0;
                *ret = TRUE;
            }
            else if (ofn->nMaxFile >= 2) *(WORD *)ofn->lpstrFile = (WORD)total;
        }
        if (*ret && !(ofn->Flags & OFN_NOCHANGEDIR))
        {
            lstrcpynW( dir, ofn->lpstrFile, min( ARRAYSIZE(dir), (size_t)ofn->nFileOffset + 1 ) );
            if (count == 1 && ofn->nFileOffset) dir[ofn->nFileOffset - 1] = 0;
            SetCurrentDirectoryW( dir );
        }
        for (i = 0; i < count; i++) if (dos[i]) HeapFree( GetProcessHeap(), 0, dos[i] );
        HeapFree( GetProcessHeap(), 0, dos );
    }
    for (i = 0; i < count; i++) HeapFree( GetProcessHeap(), 0, paths[i] );
    if (paths) HeapFree( GetProcessHeap(), 0, paths );
    return TRUE;
}

/* ---------- SHBrowseForFolder ---------- */

/* The app's callback gets a hidden window of ours: BFFM_SETSELECTION,
 * BFFM_SETOKTEXT, BFFM_SETSTATUSTEXT and BFFM_ENABLEOK sent to it before the
 * panel opens (in BFFM_INITIALIZED) set it up; sent later, they update it. */
struct folder_dialog
{
    struct w2s_request_handler handler;     /* first: the handler is the state */
    BROWSEINFOW *bi;
    HWND hwnd;
    UINT64 id;              /* 0 until the panel is up */
    WCHAR selection[MAX_PATH];
    WCHAR ok_text[64];
    WCHAR status[512];
    WCHAR title[256];
    BOOL ok_enabled;
};

static BOOL (WINAPI *p_SHGetPathFromIDListW)( LPCITEMIDLIST, WCHAR * );
static LPITEMIDLIST (WINAPI *p_ILCreateFromPathW)( const WCHAR * );
static void (WINAPI *p_ILFree)( LPITEMIDLIST );

static void folder_update( struct folder_dialog *fd, const char *key, const WCHAR *value, BOOL unix_path )
{
    struct json j;

    if (!fd->id) return;
    json_init( &j );
    json_obj_begin( &j );
    if (unix_path) json_unix_path( &j, key, value );
    else json_str( &j, key, value );
    json_obj_end( &j );
    w2s_request_update( fd->id, j.buf );
    json_free( &j );
}

static void folder_text( WCHAR *dst, int size, LPARAM lparam, BOOL ansi )
{
    if (!lparam) dst[0] = 0;
    else if (ansi) MultiByteToWideChar( CP_ACP, 0, (const char *)lparam, -1, dst, size );
    else lstrcpynW( dst, (const WCHAR *)lparam, size );
}

static LRESULT CALLBACK folder_dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct folder_dialog *fd = (struct folder_dialog *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    char json[64];

    if (!fd) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case BFFM_SETSELECTIONA:
    case BFFM_SETSELECTIONW:
        if (!wparam)    /* a PIDL */
        {
            if (!p_SHGetPathFromIDListW || !p_SHGetPathFromIDListW( (LPCITEMIDLIST)lparam, fd->selection )) return FALSE;
        }
        else folder_text( fd->selection, MAX_PATH, lparam, msg == BFFM_SETSELECTIONA );
        folder_update( fd, "dir", fd->selection, TRUE );
        return TRUE;
    case BFFM_SETOKTEXT:
        folder_text( fd->ok_text, ARRAYSIZE(fd->ok_text), lparam, FALSE );
        folder_update( fd, "prompt", fd->ok_text, FALSE );
        return TRUE;
    case BFFM_SETSTATUSTEXTA:
    case BFFM_SETSTATUSTEXTW:
        folder_text( fd->status, ARRAYSIZE(fd->status), lparam, msg == BFFM_SETSTATUSTEXTA );
        folder_update( fd, "status", fd->status, FALSE );
        return TRUE;
    case BFFM_ENABLEOK:
        fd->ok_enabled = lparam != 0;
        snprintf( json, sizeof(json), "{\"okEnabled\":%s}", lparam ? "true" : "false" );
        if (fd->id) w2s_request_update( fd->id, json );
        return TRUE;
    case BFFM_SETEXPANDED:
        return TRUE;
    case WM_SETTEXT:    /* the dialog's caption, which apps often set in BFFM_INITIALIZED */
        folder_text( fd->title, ARRAYSIZE(fd->title), lparam, FALSE );
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void folder_event( struct w2s_request_handler *handler, UINT64 id, const struct w2s_event *ev )
{
    struct folder_dialog *fd = (struct folder_dialog *)handler;
    char *utf8;
    WCHAR *dos;
    LPITEMIDLIST pidl;

    fd->id = id;
    if (strcmp( ev->type, "selchange" ) || !ev->string || !fd->bi->lpfn || !p_ILCreateFromPathW) return;
    utf8 = utf8_from_wide( ev->string, -1 );
    dos = p_wine_get_dos_file_name( utf8 );
    HeapFree( GetProcessHeap(), 0, utf8 );
    if (!dos) return;
    if ((pidl = p_ILCreateFromPathW( dos )))
    {
        fd->bi->lpfn( fd->hwnd, BFFM_SELCHANGED, (LPARAM)pidl, fd->bi->lParam );
        p_ILFree( pidl );
    }
    HeapFree( GetProcessHeap(), 0, dos );
}

static void folder_idle( struct w2s_request_handler *handler, UINT64 id )
{
    ((struct folder_dialog *)handler)->id = id;
}

/***********************************************************************
 *      W2SBrowseForFolder  (win32swiftui.@)
 *
 * shell32's SHBrowseForFolderW asks first. TRUE: handled, and *chosen says
 * whether path (MAX_PATH) holds the chosen folder, which shell32 turns into
 * the PIDL it returns. FALSE: wine's dialog runs.
 */
BOOL WINAPI W2SBrowseForFolder( BROWSEINFOW *bi, WCHAR *path, BOOL *chosen )
{
    static ATOM atom;
    HMODULE shell32 = GetModuleHandleW( L"shell32.dll" );
    struct folder_dialog fd;
    struct json j;
    WCHAR **paths, message[1024];
    char *result;
    int count, i;
    BOOL ok;

    *chosen = FALSE;
    /* computers and printers aren't file-system choices */
    if (bi->ulFlags & (BIF_BROWSEFORCOMPUTER | BIF_BROWSEFORPRINTER)) return FALSE;
    init_path_funcs();
    if (!p_wine_get_dos_file_name || !p_wine_get_unix_file_name || !shell32) return FALSE;
    p_SHGetPathFromIDListW = (void *)GetProcAddress( shell32, "SHGetPathFromIDListW" );
    p_ILCreateFromPathW = (void *)GetProcAddress( shell32, "ILCreateFromPathW" );
    p_ILFree = (void *)GetProcAddress( shell32, "ILFree" );
    if (!atom)
    {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = folder_dialog_proc;
        wc.hInstance = GetModuleHandleW( L"win32swiftui.dll" );
        wc.lpszClassName = L"Win32ToSwiftUI.BrowseForFolder";
        atom = RegisterClassW( &wc );
    }

    memset( &fd, 0, sizeof(fd) );
    fd.handler.event = folder_event;
    fd.handler.idle = folder_idle;
    fd.bi = bi;
    fd.ok_enabled = TRUE;
    fd.hwnd = CreateWindowExW( 0, L"Win32ToSwiftUI.BrowseForFolder", NULL, WS_POPUP, 0, 0, 0, 0, bi->hwndOwner,
                               NULL, GetModuleHandleW( L"win32swiftui.dll" ), NULL );
    if (!fd.hwnd) return FALSE;
    SetWindowLongPtrW( fd.hwnd, GWLP_USERDATA, (LONG_PTR)&fd );

    /* the root is where the panel starts, unless the callback picks a folder */
    if (bi->pidlRoot && !IS_INTRESOURCE( bi->pidlRoot ) && p_SHGetPathFromIDListW)
        p_SHGetPathFromIDListW( bi->pidlRoot, fd.selection );
    if (bi->lpfn) bi->lpfn( fd.hwnd, BFFM_INITIALIZED, 0, bi->lParam );

    message[0] = 0;
    if (bi->lpszTitle) lstrcpynW( message, bi->lpszTitle, ARRAYSIZE(message) );
    if (fd.status[0])
    {
        if (message[0]) wcsncat( message, L"\n", ARRAYSIZE(message) - wcslen( message ) - 1 );
        wcsncat( message, fd.status, ARRAYSIZE(message) - wcslen( message ) - 1 );
    }
    json_init( &j );
    json_obj_begin( &j );
    json_bool( &j, "folders", TRUE );
    json_bool( &j, "files", (bi->ulFlags & BIF_BROWSEINCLUDEFILES) != 0 );
    json_bool( &j, "newFolder", !(bi->ulFlags & BIF_NONEWFOLDERBUTTON) );
    json_bool( &j, "resolveLinks", !(bi->ulFlags & BIF_NOTRANSLATETARGETS) );
    json_bool( &j, "events", bi->lpfn != NULL );
    json_bool( &j, "okEnabled", fd.ok_enabled );
    if (message[0]) json_str( &j, "message", message );
    if (fd.ok_text[0]) json_str( &j, "prompt", fd.ok_text );
    if (fd.title[0]) json_str( &j, "windowTitle", fd.title );
    if (fd.selection[0]) json_unix_path( &j, "dir", fd.selection );
    json_obj_end( &j );

    ok = w2s_run_request_ex( "open", bi->hwndOwner, j.buf, &fd.handler, &result );
    json_free( &j );
    SetWindowLongPtrW( fd.hwnd, GWLP_USERDATA, 0 );
    DestroyWindow( fd.hwnd );
    if (!ok) return FALSE;

    count = json_get_str_array( result, "paths", &paths );
    HeapFree( GetProcessHeap(), 0, result );
    if (count > 0)
    {
        char *utf8 = utf8_from_wide( paths[0], -1 );
        WCHAR *dos = p_wine_get_dos_file_name( utf8 );
        HeapFree( GetProcessHeap(), 0, utf8 );
        if (dos && wcslen( dos ) < MAX_PATH)
        {
            wcscpy( path, dos );
            *chosen = TRUE;
        }
        if (dos) HeapFree( GetProcessHeap(), 0, dos );
    }
    for (i = 0; i < count; i++) HeapFree( GetProcessHeap(), 0, paths[i] );
    if (paths) HeapFree( GetProcessHeap(), 0, paths );
    return TRUE;
}
