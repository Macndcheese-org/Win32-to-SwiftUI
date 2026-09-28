/*
 * win32swiftui.dll: ChooseColor, ChooseFont, PrintDlg and PrintDlgEx as the
 * macOS colour, font and print panels (map: colordialog, fontdialog,
 * printdialog). wine's comdlg32 offers each call first; an app's hook or
 * template keeps wine's dialog, since the app draws into it or drives it.
 *
 * What comes back is written into the app's structures the way comdlg32
 * does: COLORREF and the custom colours, a LOGFONT GDI can create (the face
 * checked against what GDI enumerates), and DEVMODE/DEVNAMES built by the
 * printer's own driver (winspool DocumentProperties), plus the DC if asked.
 */
#include "w2s_pe.h"
#include <winspool.h>
#include <dlgs.h>

static void json_rgb( struct json *j, const char *key, COLORREF c )
{
    json_arr_begin( j, key );
    json_int( j, NULL, GetRValue( c ) );
    json_int( j, NULL, GetGValue( c ) );
    json_int( j, NULL, GetBValue( c ) );
    json_arr_end( j );
}

static BOOL result_rgb( const char *result, const char *key, COLORREF *c )
{
    int *v, n = json_get_int_array( result, key, &v );
    BOOL ok = n >= 3;
    if (ok) *c = RGB( v[0], v[1], v[2] );
    if (v) HeapFree( GetProcessHeap(), 0, v );
    return ok;
}

static BOOL result_flag( const char *result, const char *key )
{
    double v;
    return json_get_num( result, key, &v ) && v != 0;
}

static int result_int( const char *result, const char *key, int def )
{
    double v;
    return json_get_num( result, key, &v ) ? (int)v : def;
}

/* a label of one of comdlg32's own dialogs, in wine's language */
static void json_label( struct json *j, const char *key, const WCHAR *dialog, int id )
{
    WCHAR *label = w2s_dialog_label( GetModuleHandleW( L"comdlg32.dll" ), dialog, 0, id );
    if (label)
    {
        json_str( j, key, label );
        HeapFree( GetProcessHeap(), 0, label );
    }
}

/* ---------- ChooseColor ---------- */

/***********************************************************************
 *      W2SChooseColor  (win32swiftui.@)
 *
 * ChooseColorA goes through ChooseColorW, so this covers both.
 */
BOOL WINAPI W2SChooseColor( CHOOSECOLORW *cc, BOOL *ret )
{
    struct json j;
    char *result;
    int *custom = NULL, i;

    if (!cc || cc->lStructSize < sizeof(*cc)) return FALSE;
    if (cc->Flags & (CC_ENABLEHOOK | CC_ENABLETEMPLATE | CC_ENABLETEMPLATEHANDLE)) return FALSE;

    json_init( &j );
    json_obj_begin( &j );
    json_rgb( &j, "color", (cc->Flags & CC_RGBINIT) ? cc->rgbResult : RGB( 0, 0, 0 ) );
    if (cc->lpCustColors)
    {
        json_arr_begin( &j, "custom" );
        for (i = 0; i < 16; i++)
        {
            json_int( &j, NULL, GetRValue( cc->lpCustColors[i] ) );
            json_int( &j, NULL, GetGValue( cc->lpCustColors[i] ) );
            json_int( &j, NULL, GetBValue( cc->lpCustColors[i] ) );
        }
        json_arr_end( &j );
    }
    json_label( &j, "ok", L"CHOOSE_COLOR", IDOK );
    json_label( &j, "cancel", L"CHOOSE_COLOR", IDCANCEL );
    json_obj_end( &j );

    if (!w2s_run_request( "color", cc->hwndOwner, j.buf, &result ))
    {
        json_free( &j );
        return FALSE;
    }
    json_free( &j );

    *ret = result_flag( result, "ok" );
    if (*ret) result_rgb( result, "color", &cc->rgbResult );
    /* the custom colours change as the user adds them, OK or not (as in wine's dialog) */
    if (cc->lpCustColors && json_get_int_array( result, "custom", &custom ) >= 48)
        for (i = 0; i < 16; i++) cc->lpCustColors[i] = RGB( custom[i * 3], custom[i * 3 + 1], custom[i * 3 + 2] );
    if (custom) HeapFree( GetProcessHeap(), 0, custom );
    HeapFree( GetProcessHeap(), 0, result );
    TRACE( "ChooseColor -> %d %06lx\n", *ret, cc->rgbResult );
    return TRUE;
}

/* ---------- ChooseFont ---------- */

struct face_search
{
    LOGFONTW found;
    BOOL ok;
};

static int CALLBACK face_proc( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lparam )
{
    struct face_search *search = (struct face_search *)lparam;
    search->found = *lf;
    search->ok = TRUE;
    return 0;
}

static BOOL find_face( HDC hdc, const WCHAR *name, LOGFONTW *found )
{
    struct face_search search = { { 0 } };
    LOGFONTW lf = { 0 };

    if (!name || !name[0]) return FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW( lf.lfFaceName, name, LF_FACESIZE );
    EnumFontFamiliesExW( hdc, &lf, face_proc, (LPARAM)&search, 0 );
    if (search.ok) *found = search.found;
    return search.ok;
}

/* the macOS family if GDI has it, else the closest face GDI enumerates:
 * shorter forms of the name ("Helvetica Neue" -> "Helvetica"), then the
 * face the app started with */
static BOOL resolve_face( HDC hdc, const WCHAR *family, const WCHAR *fallback, LOGFONTW *found )
{
    WCHAR name[LF_FACESIZE], *space;

    if (find_face( hdc, family, found )) return TRUE;
    if (family)
    {
        lstrcpynW( name, family, LF_FACESIZE );
        while ((space = wcsrchr( name, ' ' )))
        {
            *space = 0;
            if (find_face( hdc, name, found )) return TRUE;
        }
    }
    return find_face( hdc, fallback, found );
}

/***********************************************************************
 *      W2SChooseFont  (win32swiftui.@)
 *
 * wine's ChooseFontA converts to this and back.
 */
BOOL WINAPI W2SChooseFont( CHOOSEFONTW *cf, BOOL *ret )
{
    LOGFONTW *lf, found;
    WCHAR *family, *face;
    struct json j;
    char *result;
    HDC hdc, screen = NULL;
    double size = 0;
    int dpi, weight;
    BOOL italic;

    if (!cf || cf->lStructSize < sizeof(*cf) || !(lf = cf->lpLogFont)) return FALSE;
    if (cf->Flags & (CF_ENABLEHOOK | CF_ENABLETEMPLATE | CF_ENABLETEMPLATEHANDLE)) return FALSE;
    /* printer-only fonts: the Mac's font list isn't the printer's */
    if ((cf->Flags & CF_BOTH) == CF_PRINTERFONTS) return FALSE;

    hdc = (cf->Flags & CF_PRINTERFONTS) && cf->hDC ? cf->hDC : (screen = GetDC( NULL ));
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    if (dpi <= 0) dpi = 96;

    json_init( &j );
    json_obj_begin( &j );
    if (cf->Flags & CF_INITTOLOGFONTSTRUCT)
    {
        json_str( &j, "family", lf->lfFaceName );
        if (lf->lfHeight) size = (double)abs( lf->lfHeight ) * 72 / dpi;
        json_int( &j, "weight", lf->lfWeight ? lf->lfWeight : FW_NORMAL );
        json_bool( &j, "italic", lf->lfItalic != 0 );
        json_bool( &j, "underline", lf->lfUnderline != 0 );
        json_bool( &j, "strikeout", lf->lfStrikeOut != 0 );
    }
    if (!size && cf->iPointSize > 0) size = cf->iPointSize / 10.0;
    json_num( &j, "size", size > 0 ? size : 12 );
    json_bool( &j, "effects", (cf->Flags & CF_EFFECTS) != 0 );
    if (cf->Flags & CF_EFFECTS) json_rgb( &j, "color", cf->rgbColors );
    json_bool( &j, "fixedOnly", (cf->Flags & CF_FIXEDPITCHONLY) != 0 );
    if (cf->Flags & CF_LIMITSIZE)
    {
        json_int( &j, "minSize", cf->nSizeMin );
        json_int( &j, "maxSize", cf->nSizeMax );
    }
    json_label( &j, "ok", L"CHOOSE_FONT", IDOK );
    json_label( &j, "cancel", L"CHOOSE_FONT", IDCANCEL );
    json_label( &j, "strikeoutTitle", L"CHOOSE_FONT", chx1 );
    json_label( &j, "underlineTitle", L"CHOOSE_FONT", chx2 );
    json_label( &j, "colorTitle", L"CHOOSE_FONT", stc4 );
    json_obj_end( &j );

    if (!w2s_run_request( "font", cf->hwndOwner, j.buf, &result ))
    {
        json_free( &j );
        if (screen) ReleaseDC( NULL, screen );
        return FALSE;
    }
    json_free( &j );

    *ret = result_flag( result, "ok" );
    if (*ret)
    {
        family = json_get_str( result, "family" );
        face = json_get_str( result, "face" );
        json_get_num( result, "size", &size );
        weight = result_int( result, "weight", FW_NORMAL );
        italic = result_flag( result, "italic" );

        memset( &found, 0, sizeof(found) );
        if (!resolve_face( hdc, family, (cf->Flags & CF_INITTOLOGFONTSTRUCT) ? lf->lfFaceName : NULL, &found ))
        {
            NONCLIENTMETRICSW ncm = { sizeof(ncm) };
            SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0 );
            found = ncm.lfMessageFont;
        }
        TRACE( "ChooseFont: %ls -> %ls, %.1f pt\n", family ? family : L"(none)", found.lfFaceName, size );

        cf->iPointSize = (INT)(size * 10 + 0.5);
        lf->lfHeight = -MulDiv( cf->iPointSize, dpi, 720 );
        lf->lfWidth = 0;
        lf->lfWeight = weight;
        lf->lfItalic = italic;
        if (cf->Flags & CF_EFFECTS)
        {
            lf->lfUnderline = result_flag( result, "underline" );
            lf->lfStrikeOut = result_flag( result, "strikeout" );
            result_rgb( result, "color", &cf->rgbColors );
        }
        lf->lfCharSet = found.lfCharSet;
        lf->lfOutPrecision = found.lfOutPrecision;
        lf->lfClipPrecision = found.lfClipPrecision;
        lf->lfQuality = found.lfQuality;
        lf->lfPitchAndFamily = found.lfPitchAndFamily;
        lstrcpynW( lf->lfFaceName, found.lfFaceName, LF_FACESIZE );
        cf->nFontType = SCREEN_FONTTYPE | (weight >= FW_SEMIBOLD ? BOLD_FONTTYPE : 0) | (italic ? ITALIC_FONTTYPE : 0);
        if (!(cf->nFontType & (BOLD_FONTTYPE | ITALIC_FONTTYPE))) cf->nFontType |= REGULAR_FONTTYPE;
        if ((cf->Flags & CF_USESTYLE) && cf->lpszStyle && face) lstrcpynW( cf->lpszStyle, face, LF_FACESIZE );
        if (family) HeapFree( GetProcessHeap(), 0, family );
        if (face) HeapFree( GetProcessHeap(), 0, face );
    }
    HeapFree( GetProcessHeap(), 0, result );
    if (screen) ReleaseDC( NULL, screen );
    return TRUE;
}

/* ---------- PrintDlg / PrintDlgEx ---------- */

struct print_job
{
    HWND owner;
    HGLOBAL *devmode;       /* in/out, the app's */
    HGLOBAL *devnames;      /* in/out, the app's */
    DWORD flags;            /* in/out: PD_PAGENUMS, PD_SELECTION, PD_COLLATE */
    int copies;             /* in/out */
    int from, to;           /* in/out */
    int min, max;
    HDC dc;                 /* out, with PD_RETURNDC / PD_RETURNIC */
};

/* the printer the app asked for (hDevNames, hDevMode), else the default */
static BOOL initial_printer( struct print_job *job, WCHAR *name, DWORD size )
{
    DWORD len = size;

    name[0] = 0;
    if (*job->devnames)
    {
        DEVNAMES *dn = GlobalLock( *job->devnames );
        if (dn) lstrcpynW( name, (WCHAR *)dn + dn->wDeviceOffset, size );
        GlobalUnlock( *job->devnames );
    }
    if (!name[0] && *job->devmode)
    {
        DEVMODEW *dm = GlobalLock( *job->devmode );
        if (dm) lstrcpynW( name, dm->dmDeviceName, min( size, CCHDEVICENAME ) );
        GlobalUnlock( *job->devmode );
    }
    if (!name[0] && !GetDefaultPrinterW( name, &len )) name[0] = 0;
    return name[0] != 0;
}

static PRINTER_INFO_2W *printer_info( const WCHAR *name )
{
    PRINTER_INFO_2W *info = NULL;
    HANDLE printer;
    DWORD needed = 0;

    if (!OpenPrinterW( (WCHAR *)name, &printer, NULL )) return NULL;
    GetPrinterW( printer, 2, NULL, 0, &needed );
    if (needed && (info = HeapAlloc( GetProcessHeap(), 0, needed )) &&
        !GetPrinterW( printer, 2, (BYTE *)info, needed, &needed ))
    {
        HeapFree( GetProcessHeap(), 0, info );
        info = NULL;
    }
    ClosePrinter( printer );
    return info;
}

/* the macOS printer the panel chose, among wine's: wine names a CUPS printer
 * by its queue and keeps the name macOS shows (printer-info) as the comment */
static BOOL wine_printer( const WCHAR *mac_name, WCHAR *name, DWORD size )
{
    PRINTER_INFO_2W *list;
    DWORD needed = 0, count = 0, i;
    BOOL ok = FALSE;

    EnumPrintersW( PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &count );
    if (!needed || !(list = HeapAlloc( GetProcessHeap(), 0, needed ))) return FALSE;
    if (EnumPrintersW( PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, (BYTE *)list, needed, &needed, &count ))
    {
        for (i = 0; i < count && !ok; i++)
            if ((list[i].pComment && !wcscmp( list[i].pComment, mac_name )) || !wcscmp( list[i].pPrinterName, mac_name ))
            {
                lstrcpynW( name, list[i].pPrinterName, size );
                ok = TRUE;
            }
    }
    HeapFree( GetProcessHeap(), 0, list );
    return ok;
}

/* PWG paper names (what macOS reports) and DMPAPER_* */
static const struct { const WCHAR *name; short paper; } papers[] =
{
    { L"na-letter", DMPAPER_LETTER }, { L"na-legal", DMPAPER_LEGAL }, { L"na-ledger", DMPAPER_TABLOID },
    { L"na-executive", DMPAPER_EXECUTIVE }, { L"iso-a3", DMPAPER_A3 }, { L"iso-a4", DMPAPER_A4 },
    { L"iso-a5", DMPAPER_A5 }, { L"iso-b5", DMPAPER_B5 }, { L"jis-b5", DMPAPER_B5 },
    { L"na-number-10", DMPAPER_ENV_10 }, { L"iso-dl", DMPAPER_ENV_DL }, { L"iso-c5", DMPAPER_ENV_C5 },
};

static short paper_size( const WCHAR *pwg )
{
    unsigned int i;
    for (i = 0; i < ARRAYSIZE(papers); i++)
        if (!_wcsnicmp( pwg, papers[i].name, wcslen( papers[i].name ) )) return papers[i].paper;
    return 0;
}

static const WCHAR *paper_pwg( short paper )
{
    unsigned int i;
    for (i = 0; i < ARRAYSIZE(papers); i++) if (papers[i].paper == paper) return papers[i].name;
    return NULL;
}

static BOOL set_devnames( HGLOBAL *handle, const WCHAR *driver, const WCHAR *device, const WCHAR *port )
{
    const WCHAR *slash = wcsrchr( driver, '\\' );
    WCHAR def[256];
    DWORD def_len = ARRAYSIZE(def);
    SIZE_T size;
    DEVNAMES *dn;
    WCHAR *p;
    HGLOBAL mem;

    if (slash) driver = slash + 1;
    size = sizeof(DEVNAMES) + (wcslen( driver ) + wcslen( device ) + wcslen( port ) + 3) * sizeof(WCHAR);
    mem = *handle ? GlobalReAlloc( *handle, size, GMEM_MOVEABLE ) : GlobalAlloc( GMEM_MOVEABLE, size );
    if (!mem) return FALSE;
    *handle = mem;
    dn = GlobalLock( mem );
    p = (WCHAR *)(dn + 1);
    dn->wDriverOffset = p - (WCHAR *)dn;
    wcscpy( p, driver );
    p += wcslen( driver ) + 1;
    dn->wDeviceOffset = p - (WCHAR *)dn;
    wcscpy( p, device );
    p += wcslen( device ) + 1;
    dn->wOutputOffset = p - (WCHAR *)dn;
    wcscpy( p, port );
    dn->wDefault = GetDefaultPrinterW( def, &def_len ) && !wcscmp( def, device ) ? DN_DEFAULTPRN : 0;
    GlobalUnlock( mem );
    return TRUE;
}

/* the printer driver's DEVMODE for name, starting from the app's when it is
 * for the same printer, with the panel's choices in it */
static BOOL set_devmode( struct print_job *job, const WCHAR *name, const char *result, BOOL copies_in_devmode )
{
    DEVMODEW *app = NULL, *dm;
    HANDLE printer;
    WCHAR *paper;
    HGLOBAL mem;
    LONG size;
    short paper_id;
    BOOL ok = FALSE;

    if (!OpenPrinterW( (WCHAR *)name, &printer, NULL )) return FALSE;
    size = DocumentPropertiesW( job->owner, printer, (WCHAR *)name, NULL, NULL, 0 );
    if (size <= 0 || !(dm = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
    {
        ClosePrinter( printer );
        return FALSE;
    }
    if (*job->devmode && (app = GlobalLock( *job->devmode )) && wcsncmp( app->dmDeviceName, name, CCHDEVICENAME - 1 ))
    {
        GlobalUnlock( *job->devmode );
        app = NULL;
    }
    if (DocumentPropertiesW( job->owner, printer, (WCHAR *)name, dm, app, app ? DM_IN_BUFFER | DM_OUT_BUFFER : DM_OUT_BUFFER ) == IDOK)
    {
        dm->dmFields |= DM_ORIENTATION | DM_COPIES | DM_COLLATE;
        dm->dmOrientation = result_flag( result, "landscape" ) ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
        dm->dmCopies = copies_in_devmode ? job->copies : 1;
        dm->dmCollate = copies_in_devmode && (job->flags & PD_COLLATE) ? DMCOLLATE_TRUE : DMCOLLATE_FALSE;
        if ((paper = json_get_str( result, "paper" )))
        {
            if ((paper_id = paper_size( paper )))
            {
                dm->dmFields |= DM_PAPERSIZE;
                dm->dmPaperSize = paper_id;
            }
            HeapFree( GetProcessHeap(), 0, paper );
        }
        /* let the driver check and complete it */
        DocumentPropertiesW( job->owner, printer, (WCHAR *)name, dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER );
        mem = *job->devmode;
        if (app) GlobalUnlock( mem );
        size = dm->dmSize + dm->dmDriverExtra;
        if (!mem || GlobalSize( mem ) < (SIZE_T)size)
            mem = mem ? GlobalReAlloc( mem, size, GMEM_MOVEABLE ) : GlobalAlloc( GMEM_MOVEABLE, size );
        if (mem)
        {
            memcpy( GlobalLock( mem ), dm, size );
            GlobalUnlock( mem );
            *job->devmode = mem;
            ok = TRUE;
        }
        app = NULL;
    }
    if (app) GlobalUnlock( *job->devmode );
    HeapFree( GetProcessHeap(), 0, dm );
    ClosePrinter( printer );
    return ok;
}

/* the printer a panel chose into the app's hDevMode and hDevNames */
static BOOL store_printer( struct print_job *job, const WCHAR *name, const char *result, BOOL copies_in_devmode )
{
    PRINTER_INFO_2W *info;
    DRIVER_INFO_3W *driver = NULL;
    HANDLE printer;
    DWORD needed = 0;
    BOOL ok = FALSE;

    if (!(info = printer_info( name ))) return FALSE;
    if (OpenPrinterW( (WCHAR *)name, &printer, NULL ))
    {
        GetPrinterDriverW( printer, NULL, 3, NULL, 0, &needed );
        if (needed && (driver = HeapAlloc( GetProcessHeap(), 0, needed )) &&
            !GetPrinterDriverW( printer, NULL, 3, (BYTE *)driver, needed, &needed ))
        {
            HeapFree( GetProcessHeap(), 0, driver );
            driver = NULL;
        }
        ClosePrinter( printer );
        ok = driver && set_devmode( job, name, result, copies_in_devmode ) &&
             set_devnames( job->devnames, driver->pDriverPath, info->pPrinterName, info->pPortName );
    }
    if (driver) HeapFree( GetProcessHeap(), 0, driver );
    HeapFree( GetProcessHeap(), 0, info );
    return ok;
}

/* shows the panel; FALSE: declined (no owner window to put the sheet on, no
 * printer), wine's dialog takes over. *printed: the user chose Print. */
static BOOL run_print_panel( struct print_job *job, DWORD in_flags, BOOL *printed )
{
    WCHAR name[256], chosen[256], *mac_name;
    PRINTER_INFO_2W *info;
    DEVMODEW *dm;
    struct json j;
    char *result;
    BOOL copies_in_devmode = (in_flags & PD_USEDEVMODECOPIESANDCOLLATE) != 0, landscape = FALSE, ok;

    *printed = FALSE;
    if (!job->owner || !IsWindowVisible( GetAncestor( job->owner, GA_ROOT ) )) return FALSE;
    if (!initial_printer( job, name, ARRAYSIZE(name) ) || !(info = printer_info( name ))) return FALSE;

    if (*job->devmode && (dm = GlobalLock( *job->devmode )))
    {
        if (dm->dmFields & DM_ORIENTATION) landscape = dm->dmOrientation == DMORIENT_LANDSCAPE;
        if (copies_in_devmode && (dm->dmFields & DM_COPIES)) job->copies = dm->dmCopies;
        if (copies_in_devmode && (dm->dmFields & DM_COLLATE) && dm->dmCollate == DMCOLLATE_TRUE) job->flags |= PD_COLLATE;
        GlobalUnlock( *job->devmode );
    }

    json_init( &j );
    json_obj_begin( &j );
    json_str( &j, "printer", info->pComment && info->pComment[0] ? info->pComment : info->pPrinterName );
    json_int( &j, "copies", max( 1, job->copies ) );
    json_bool( &j, "collate", (job->flags & PD_COLLATE) != 0 );
    json_bool( &j, "pageNums", (job->flags & PD_PAGENUMS) != 0 );
    json_bool( &j, "noPageNums", (in_flags & PD_NOPAGENUMS) != 0 );
    json_bool( &j, "noSelection", (in_flags & PD_NOSELECTION) != 0 );
    json_bool( &j, "selection", (job->flags & PD_SELECTION) != 0 );
    json_int( &j, "from", max( job->from, 1 ) );
    json_int( &j, "to", max( job->to, max( job->from, 1 ) ) );
    json_bool( &j, "landscape", landscape );
    json_obj_end( &j );
    HeapFree( GetProcessHeap(), 0, info );

    ok = w2s_run_request( "print", job->owner, j.buf, &result );
    json_free( &j );
    if (!ok) return FALSE;
    if (result_flag( result, "declined" ))
    {
        HeapFree( GetProcessHeap(), 0, result );
        return FALSE;
    }
    if (!result_flag( result, "ok" ))
    {
        HeapFree( GetProcessHeap(), 0, result );
        return TRUE;
    }

    /* a printer wine doesn't have (added since wine started): the one the app had */
    if ((mac_name = json_get_str( result, "printer" )))
    {
        if (wine_printer( mac_name, chosen, ARRAYSIZE(chosen) )) lstrcpyW( name, chosen );
        TRACE( "print panel chose %.200ls -> %.200ls\n", mac_name, name );
        HeapFree( GetProcessHeap(), 0, mac_name );
    }

    job->copies = max( 1, result_int( result, "copies", 1 ) );
    if (result_flag( result, "collate" )) job->flags |= PD_COLLATE;
    else job->flags &= ~PD_COLLATE;
    job->flags &= ~(PD_PAGENUMS | PD_SELECTION);
    if (result_flag( result, "selection" )) job->flags |= PD_SELECTION;
    else if (!result_flag( result, "allPages" ))
    {
        job->flags |= PD_PAGENUMS;
        job->from = result_int( result, "from", 1 );
        job->to = result_int( result, "to", job->from );
        if (job->max >= job->min && job->max > 0)
        {
            job->from = min( max( job->from, job->min ), job->max );
            job->to = min( max( job->to, job->from ), job->max );
        }
    }

    ok = store_printer( job, name, result, copies_in_devmode );
    if (ok && copies_in_devmode) job->copies = 1;   /* the driver makes them */
    if (ok && (in_flags & (PD_RETURNDC | PD_RETURNIC)))
    {
        /* as comdlg32 does it: from what DEVNAMES and DEVMODE now say */
        DEVNAMES *dn = GlobalLock( *job->devnames );
        DEVMODEW *mode = GlobalLock( *job->devmode );
        const WCHAR *drv = (WCHAR *)dn + dn->wDriverOffset, *dev = (WCHAR *)dn + dn->wDeviceOffset;
        const WCHAR *port = (WCHAR *)dn + dn->wOutputOffset;

        job->dc = (in_flags & PD_RETURNDC) ? CreateDCW( drv, dev, port, mode ) : CreateICW( drv, dev, port, mode );
        GlobalUnlock( *job->devmode );
        GlobalUnlock( *job->devnames );
    }
    HeapFree( GetProcessHeap(), 0, result );
    *printed = ok;
    return TRUE;
}

/***********************************************************************
 *      W2SPrintDlg  (win32swiftui.@)
 */
BOOL WINAPI W2SPrintDlg( PRINTDLGW *pd, BOOL *ret )
{
    struct print_job job = { 0 };
    BOOL printed;

    if (!pd || pd->lStructSize != sizeof(*pd)) return FALSE;
    /* no UI, the Print Setup dialog, or the app's hooks and templates: wine's */
    if (pd->Flags & (PD_RETURNDEFAULT | PD_PRINTSETUP | PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK |
                     PD_ENABLEPRINTTEMPLATE | PD_ENABLESETUPTEMPLATE | PD_ENABLEPRINTTEMPLATEHANDLE |
                     PD_ENABLESETUPTEMPLATEHANDLE))
        return FALSE;

    job.owner = pd->hwndOwner;
    job.devmode = &pd->hDevMode;
    job.devnames = &pd->hDevNames;
    job.flags = pd->Flags;
    job.copies = pd->nCopies;
    job.from = pd->nFromPage;
    job.to = pd->nToPage;
    job.min = pd->nMinPage;
    job.max = pd->nMaxPage;
    if (!run_print_panel( &job, pd->Flags, &printed )) return FALSE;

    *ret = printed;
    if (printed)
    {
        pd->Flags = (pd->Flags & ~(PD_PAGENUMS | PD_SELECTION | PD_COLLATE)) | (job.flags & (PD_PAGENUMS | PD_SELECTION | PD_COLLATE));
        pd->nCopies = job.copies;
        if (job.flags & PD_PAGENUMS)
        {
            pd->nFromPage = job.from;
            pd->nToPage = job.to;
        }
        if (pd->Flags & (PD_RETURNDC | PD_RETURNIC)) pd->hDC = job.dc;
    }
    return TRUE;
}

/***********************************************************************
 *      W2SPrintDlgEx  (win32swiftui.@)
 */
BOOL WINAPI W2SPrintDlgEx( PRINTDLGEXW *pd, HRESULT *hr )
{
    struct print_job job = { 0 };
    BOOL printed;

    if (!pd || pd->lStructSize != sizeof(*pd) || !pd->hwndOwner) return FALSE;
    /* the app's own pages, callback or template: wine's */
    if ((pd->Flags & (PD_RETURNDEFAULT | PD_ENABLEPRINTTEMPLATE | PD_ENABLEPRINTTEMPLATEHANDLE)) ||
        pd->lpCallback || pd->nPropertyPages || pd->nStartPage != START_PAGE_GENERAL)
        return FALSE;

    job.owner = pd->hwndOwner;
    job.devmode = &pd->hDevMode;
    job.devnames = &pd->hDevNames;
    job.flags = pd->Flags;
    job.copies = pd->nCopies;
    job.min = pd->nMinPage;
    job.max = pd->nMaxPage;
    if (pd->nPageRanges && pd->lpPageRanges)
    {
        job.from = pd->lpPageRanges[0].nFromPage;
        job.to = pd->lpPageRanges[0].nToPage;
    }
    if (!run_print_panel( &job, pd->Flags, &printed )) return FALSE;

    *hr = S_OK;
    pd->dwResultAction = printed ? PD_RESULT_PRINT : PD_RESULT_CANCEL;
    if (printed)
    {
        pd->Flags = (pd->Flags & ~(PD_PAGENUMS | PD_SELECTION | PD_COLLATE | PD_CURRENTPAGE)) |
                    (job.flags & (PD_PAGENUMS | PD_SELECTION | PD_COLLATE));
        pd->nCopies = job.copies;
        if ((job.flags & PD_PAGENUMS) && pd->lpPageRanges && pd->nMaxPageRanges)
        {
            pd->nPageRanges = 1;
            pd->lpPageRanges[0].nFromPage = job.from;
            pd->lpPageRanges[0].nToPage = job.to;
        }
        if (pd->Flags & (PD_RETURNDC | PD_RETURNIC)) pd->hDC = job.dc;
    }
    return TRUE;
}

/* ---------- PageSetupDlg ---------- */

/***********************************************************************
 *      W2SPageSetupDlg  (win32swiftui.@)
 *
 * The macOS Page Setup sheet (NSPageLayout): the printer to format for, the
 * paper size and the orientation, into hDevMode, hDevNames and ptPaperSize.
 * It has no margins: the app's stay as they are (1 inch when it gave none,
 * where wine's dialog starts). comdlg32 calls it with the printer and the
 * units filled in. Hooks, templates and the PSD_DISABLE* flags keep wine's
 * dialog: the sheet can't honour them.
 */
BOOL WINAPI W2SPageSetupDlg( PAGESETUPDLGW *psd, BOOL *ret )
{
    struct print_job job = { 0 };
    WCHAR name[256], chosen[256], *mac_name;
    const WCHAR *pwg = NULL;
    PRINTER_INFO_2W *info;
    DEVMODEW *dm;
    struct json j;
    char *result;
    double width = 0, height = 0, per_point;
    BOOL landscape = FALSE, ok;

    if (!psd || psd->lStructSize != sizeof(*psd)) return FALSE;
    if (psd->Flags & (PSD_RETURNDEFAULT | PSD_ENABLEPAGESETUPHOOK | PSD_ENABLEPAGEPAINTHOOK | PSD_ENABLEPAGESETUPTEMPLATE |
                      PSD_ENABLEPAGESETUPTEMPLATEHANDLE | PSD_DISABLEPRINTER | PSD_DISABLEORIENTATION | PSD_DISABLEPAPER))
        return FALSE;
    job.owner = psd->hwndOwner;
    job.devmode = &psd->hDevMode;
    job.devnames = &psd->hDevNames;
    if (!job.owner || !IsWindowVisible( GetAncestor( job.owner, GA_ROOT ) )) return FALSE;
    if (!initial_printer( &job, name, ARRAYSIZE(name) ) || !(info = printer_info( name ))) return FALSE;

    job.copies = 1;
    if (*job.devmode && (dm = GlobalLock( *job.devmode )))
    {
        if (dm->dmFields & DM_ORIENTATION) landscape = dm->dmOrientation == DMORIENT_LANDSCAPE;
        if (dm->dmFields & DM_PAPERSIZE) pwg = paper_pwg( dm->dmPaperSize );
        /* Page Setup leaves the copies as they are */
        if (dm->dmFields & DM_COPIES) job.copies = dm->dmCopies;
        if ((dm->dmFields & DM_COLLATE) && dm->dmCollate == DMCOLLATE_TRUE) job.flags |= PD_COLLATE;
        GlobalUnlock( *job.devmode );
    }

    json_init( &j );
    json_obj_begin( &j );
    json_str( &j, "printer", info->pComment && info->pComment[0] ? info->pComment : info->pPrinterName );
    if (pwg) json_str( &j, "paper", pwg );
    json_bool( &j, "landscape", landscape );
    json_obj_end( &j );
    HeapFree( GetProcessHeap(), 0, info );

    ok = w2s_run_request( "pagesetup", job.owner, j.buf, &result );
    json_free( &j );
    if (!ok) return FALSE;
    if (result_flag( result, "declined" ))
    {
        HeapFree( GetProcessHeap(), 0, result );
        return FALSE;
    }
    *ret = FALSE;
    if (result_flag( result, "ok" ))
    {
        if ((mac_name = json_get_str( result, "printer" )))
        {
            if (wine_printer( mac_name, chosen, ARRAYSIZE(chosen) )) lstrcpyW( name, chosen );
            HeapFree( GetProcessHeap(), 0, mac_name );
        }
        if (store_printer( &job, name, result, TRUE ))
        {
            /* points -> the app's units, landscape across */
            per_point = (psd->Flags & PSD_INHUNDREDTHSOFMILLIMETERS) ? 2540.0 / 72 : 1000.0 / 72;
            json_get_num( result, "paperWidth", &width );
            json_get_num( result, "paperHeight", &height );
            if (result_flag( result, "landscape" ) == (width < height))
            {
                double t = width;
                width = height;
                height = t;
            }
            psd->ptPaperSize.x = (LONG)(width * per_point + 0.5);
            psd->ptPaperSize.y = (LONG)(height * per_point + 0.5);
            if (!(psd->Flags & PSD_MARGINS))
            {
                LONG inch = (psd->Flags & PSD_INHUNDREDTHSOFMILLIMETERS) ? 2540 : 1000;
                SetRect( &psd->rtMargin, inch, inch, inch, inch );
            }
            *ret = TRUE;
        }
    }
    HeapFree( GetProcessHeap(), 0, result );
    TRACE( "PageSetupDlg -> %d, paper %ldx%ld\n", *ret, psd->ptPaperSize.x, psd->ptPaperSize.y );
    return TRUE;
}
