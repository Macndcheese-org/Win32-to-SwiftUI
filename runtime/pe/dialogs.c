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
    poll.buffer = small;
    poll.size = sizeof(small);
    poll.len = 0;
    poll.done = 0;
    w2s_call( unix_w2s_request_poll, &poll );
    while (poll.len > poll.size)
    {
        char *big = HeapAlloc( GetProcessHeap(), 0, poll.len );
        poll.buffer = big;
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
    struct w2s_request_update_params params = { id, json, strlen( json ) };
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

    start.kind = kind;
    start.window = have_host ? owner_host.window : 0;
    start.json = json;
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

/* The button labels wine shows, in its current language: taken from user32's
 * MSGBOX dialog template, so a French wine gets French buttons. */
static WCHAR *label_from_template( LANGID lang, int id )
{
    HMODULE user32 = GetModuleHandleW( L"user32.dll" );
    HRSRC res = FindResourceExW( user32, (LPCWSTR)RT_DIALOG, L"MSGBOX", lang );
    const BYTE *p;
    const DLGTEMPLATE *tmpl;
    DWORD style;
    WORD count, i;

    if (!res && !(res = FindResourceExW( user32, (LPCWSTR)RT_DIALOG, L"MSGBOX", LANG_NEUTRAL ))) return NULL;
    if (!(tmpl = LockResource( LoadResource( user32, res ) ))) return NULL;
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

static WCHAR *resource_string( HINSTANCE inst, const WCHAR *s )
{
    WCHAR buf[1024];
    if (!s) return NULL;
    if (!IS_INTRESOURCE( s )) return strdupW( s );
    if (!LoadStringW( inst, LOWORD( s ), buf, ARRAYSIZE(buf) )) return NULL;
    return strdupW( buf );
}

static void add_button( struct json *j, LANGID lang, int id, BOOL is_default, BOOL is_cancel )
{
    WCHAR *label = label_from_template( lang, id );
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

    text = resource_string( params->hInstance, params->lpszText );
    caption = resource_string( params->hInstance, params->lpszCaption );

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

/***********************************************************************
 *      W2SFileDialog  (win32swiftui.@)
 */
BOOL WINAPI W2SFileDialog( OPENFILENAMEW *ofn, BOOL save, BOOL *ret )
{
    struct json j;
    char *result;
    WCHAR **paths, *initial_dir = NULL, *slash, dir[MAX_PATH];
    double filter_index;
    int count, i;
    BOOL multi = (ofn->Flags & OFN_ALLOWMULTISELECT) && !save;

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
    if (ofn->lpstrTitle) json_str( &j, "title", ofn->lpstrTitle );
    json_filters( &j, ofn->lpstrFilter );
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
