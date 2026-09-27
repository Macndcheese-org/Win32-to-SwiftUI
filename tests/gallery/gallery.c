/*
 * Win32-to-SwiftUI gallery: one window with every control the runtime
 * translates (milestones 1 and 2), plus buttons for the dialogs it replaces.
 * Split buttons, command links and task dialogs only exist in comctl32 v6;
 * the gallery creates those under a v6 activation context and leaves the
 * rest on the classic classes, as most older apps get them.
 *
 *   gallery.exe            interactive
 *   gallery.exe /selftest  drives every control both ways through
 *                          win32swiftui.dll's debug hooks and prints PASS/FAIL;
 *                          the exit code is the number of failures
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>

enum
{
    ID_PUSH = 100, ID_DEFAULT, ID_CHECK, ID_RADIO1, ID_RADIO2, ID_GROUP, ID_LABEL, ID_SEP, ID_ICON,
    ID_EDIT, ID_PASSWORD, ID_NUMBER, ID_READONLY, ID_COMBO, ID_LIST, ID_MULTI, ID_REPORT, ID_LVLIST,
    ID_PROGRESS, ID_TRACK, ID_TAB, ID_MSGBOX, ID_OPEN, ID_SAVE, ID_PUSHLIKE,
    /* milestone 2 */
    ID_3STATE, ID_SPLIT, ID_CMDLINK, ID_FRAME, ID_RECT, ID_MLEDIT, ID_ECOMBO, ID_TREE, ID_UDEDIT, ID_UPDOWN,
    ID_DATETIME, ID_MONTHCAL, ID_STATUS, ID_LVCHECK, ID_LVICON, ID_TOOLTIP,
    /* dialogs (milestone 2) */
    ID_TASKDLG, ID_FOLDER, ID_COLOR, ID_FONT, ID_PRINT, ID_ITEMDLG, ID_PROPSHEET, ID_WIZARD,
    ID_LAST
};

static HWND ctl[ID_LAST];
static HWND main_window;
static int got_command[ID_LAST][16];   /* [id][notification code & 15] */
static int got_hscroll, got_tabchange, got_lvchanged, got_dropdown, got_status_click = -1;
static int got_expanding, got_treesel, got_deltapos, got_vscroll, got_datechange, got_mcselchange, got_mcselect;
static int got_dispinfo, got_check_changed, got_icon_changed, got_icon_activate;
static HTREEITEM tree_fruits, tree_apple, tree_pear, tree_veg;
static BOOL in_selftest;

/* comctl32 v6 for the controls only it has */
static HANDLE v6_context;

static BOOL v6_begin( ULONG_PTR *cookie )
{
    static const char manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" "
        "version=\"6.0.0.0\" processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
        "</dependentAssembly></dependency>\n</assembly>\n";

    if (!v6_context)
    {
        WCHAR path[MAX_PATH];
        ACTCTXW actctx = { sizeof(actctx) };
        HANDLE file;
        DWORD written;

        GetTempPathW( MAX_PATH, path );
        wcscat( path, L"w2s-gallery-v6.manifest" );
        file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (file == INVALID_HANDLE_VALUE) return FALSE;
        WriteFile( file, manifest, sizeof(manifest) - 1, &written, NULL );
        CloseHandle( file );
        actctx.lpSource = path;
        v6_context = CreateActCtxW( &actctx );
        if (v6_context == INVALID_HANDLE_VALUE) v6_context = NULL;
        if (!v6_context) return FALSE;
    }
    return ActivateActCtx( v6_context, cookie );
}

static void v6_end( ULONG_PTR cookie )
{
    DeactivateActCtx( 0, cookie );
}

/* ---------- folder pickers ---------- */

static int bff_selchanged, bff_native_ok;

static int CALLBACK browse_callback( HWND hwnd, UINT msg, LPARAM lparam, LPARAM data )
{
    if (msg == BFFM_INITIALIZED)
    {
        SendMessageW( hwnd, BFFM_SETSELECTIONW, TRUE, (LPARAM)L"Z:\\tmp" );
        SendMessageW( hwnd, BFFM_SETOKTEXT, 0, (LPARAM)L"Use Folder" );
    }
    if (msg == BFFM_SELCHANGED) bff_selchanged++;
    return 0;
}

static BOOL browse_for_folder( WCHAR *path )
{
    BROWSEINFOW bi = { 0 };
    WCHAR name[MAX_PATH];
    LPITEMIDLIST pidl;
    BOOL ok;

    bi.hwndOwner = main_window;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"Choose the folder to export to";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_callback;
    if (!(pidl = SHBrowseForFolderW( &bi ))) return FALSE;
    ok = SHGetPathFromIDListW( pidl, path );
    CoTaskMemFree( pidl );
    return ok;
}

static HRESULT item_dialog_folder( WCHAR *path )
{
    IFileOpenDialog *dialog;
    IShellItem *item;
    WCHAR *name;
    DWORD options;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog );
    if (FAILED(hr)) return hr;
    IFileOpenDialog_GetOptions( dialog, &options );
    IFileOpenDialog_SetOptions( dialog, options | FOS_PICKFOLDERS );
    IFileOpenDialog_SetTitle( dialog, L"Pick a folder" );
    hr = IFileOpenDialog_Show( dialog, main_window );
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IFileOpenDialog_GetResult( dialog, &item )))
    {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName( item, SIGDN_FILESYSPATH, &name )))
        {
            lstrcpynW( path, name, MAX_PATH );
            CoTaskMemFree( name );
        }
        IShellItem_Release( item );
    }
    IFileOpenDialog_Release( dialog );
    return hr;
}

/* ---------- task dialog (comctl32 v6 only) ---------- */

static HRESULT (WINAPI *pTaskDialogIndirect)( const TASKDIALOGCONFIG *, int *, int *, BOOL * );
static int td_created, td_radio = -1, td_verify = -1, td_clicks, td_vetoed, td_native_ok;

static HRESULT CALLBACK td_callback( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LONG_PTR data )
{
    switch (msg)
    {
    case TDN_CREATED:
        td_created++;
        SendMessageW( hwnd, TDM_SET_PROGRESS_BAR_POS, 40, 0 );
        SendMessageW( hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)L"Updated content" );
        break;
    case TDN_RADIO_BUTTON_CLICKED:
        td_radio = (int)wparam;
        break;
    case TDN_VERIFICATION_CLICKED:
        td_verify = (int)wparam;
        break;
    case TDN_BUTTON_CLICKED:
        td_clicks++;
        /* the first "Don't Save" is refused: the dialog stays open */
        if (wparam == 1002 && in_selftest && !td_vetoed++) return S_FALSE;
        break;
    }
    return S_OK;
}

static HRESULT run_task_dialog( int *button, int *radio, BOOL *verified )
{
    static const TASKDIALOG_BUTTON buttons[] = { { 1001, L"Save" }, { 1002, L"Don't Save" } };
    static const TASKDIALOG_BUTTON radios[] = { { 2001, L"Keep a copy" }, { 2002, L"Replace it" } };
    TASKDIALOGCONFIG tdc = { sizeof(tdc) };

    if (!pTaskDialogIndirect)
    {
        ULONG_PTR cookie;
        if (!v6_begin( &cookie )) return E_FAIL;
        pTaskDialogIndirect = (void *)GetProcAddress( LoadLibraryW( L"comctl32.dll" ), "TaskDialogIndirect" );
        v6_end( cookie );
        if (!pTaskDialogIndirect) return E_FAIL;
    }
    tdc.hwndParent = main_window;
    tdc.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
    tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tdc.pszMainInstruction = L"Save changes to the document?";
    tdc.pszContent = L"Your changes will be lost if you don't save them. <a href=\"help\">Why?</a>";
    tdc.cButtons = ARRAYSIZE(buttons);
    tdc.pButtons = buttons;
    tdc.nDefaultButton = 1001;
    tdc.cRadioButtons = ARRAYSIZE(radios);
    tdc.pRadioButtons = radios;
    tdc.pszVerificationText = L"Don't ask again";
    tdc.pszExpandedInformation = L"The document was changed 3 minutes ago.";
    tdc.pszFooter = L"Changes are kept for 30 days.";
    tdc.pfCallback = td_callback;
    return pTaskDialogIndirect( &tdc, button, radio, verified );
}

static char *(WINAPI *pQuery)( HWND );
static char *(WINAPI *pInject)( HWND, const char * );
static void (WINAPI *pFree)( char * );
static BOOL (WINAPI *pIsTranslated)( HWND );

static HWND make( const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id )
{
    HWND hwnd = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, main_window,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW( NULL ), NULL );
    SendMessageW( hwnd, WM_SETFONT, (WPARAM)GetStockObject( DEFAULT_GUI_FONT ), TRUE );
    ctl[id] = hwnd;
    return hwnd;
}

static void create_controls2(void);

static void create_controls(void)
{
    LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH };
    LVITEMW item = { LVIF_TEXT };
    TCITEMW tab = { TCIF_TEXT };
    int i;

    make( L"Button", L"&Push button", BS_PUSHBUTTON | WS_TABSTOP, 16, 16, 120, 24, ID_PUSH );
    make( L"Button", L"&Default", BS_DEFPUSHBUTTON | WS_TABSTOP, 144, 16, 120, 24, ID_DEFAULT );
    make( L"Button", L"Push-&like", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, 272, 16, 120, 24, ID_PUSHLIKE );
    make( L"Button", L"Group", BS_GROUPBOX, 16, 48, 376, 96, ID_GROUP );
    make( L"Button", L"&Check box", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 68, 160, 20, ID_CHECK );
    make( L"Button", L"Radio &one", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 92, 160, 20, ID_RADIO1 );
    make( L"Button", L"Radio &two", BS_AUTORADIOBUTTON, 28, 114, 160, 20, ID_RADIO2 );
    make( L"Static", L"Static text that wraps onto a second line when it is long enough.", SS_LEFT, 200, 68, 180, 40, ID_LABEL );
    make( L"Static", NULL, SS_ETCHEDHORZ, 16, 156, 376, 2, ID_SEP );
    make( L"Static", NULL, SS_ICON, 352, 104, 32, 32, ID_ICON );
    SendMessageW( ctl[ID_ICON], STM_SETICON, (WPARAM)LoadIconW( NULL, (LPCWSTR)IDI_INFORMATION ), 0 );

    make( L"Edit", L"Editable", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 16, 170, 180, 22, ID_EDIT );
    SendMessageW( ctl[ID_EDIT], EM_SETCUEBANNER, TRUE, (LPARAM)L"Type here" );
    make( L"Edit", L"secret", ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 212, 170, 180, 22, ID_PASSWORD );
    make( L"Edit", L"42", ES_NUMBER | WS_BORDER | WS_TABSTOP, 16, 200, 180, 22, ID_NUMBER );
    make( L"Edit", L"Read-only value", ES_READONLY | WS_BORDER, 212, 200, 180, 22, ID_READONLY );

    make( L"ComboBox", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 16, 232, 180, 200, ID_COMBO );
    SendMessageW( ctl[ID_COMBO], CB_ADDSTRING, 0, (LPARAM)L"Windows 10" );
    SendMessageW( ctl[ID_COMBO], CB_ADDSTRING, 0, (LPARAM)L"Windows 7" );
    SendMessageW( ctl[ID_COMBO], CB_ADDSTRING, 0, (LPARAM)L"Windows XP" );
    SendMessageW( ctl[ID_COMBO], CB_SETCURSEL, 0, 0 );

    make( L"ListBox", NULL, LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 16, 264, 180, 90, ID_LIST );
    make( L"ListBox", NULL, LBS_NOTIFY | LBS_EXTENDEDSEL | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 212, 264, 180, 90, ID_MULTI );
    for (i = 0; i < 6; i++)
    {
        WCHAR text[32];
        swprintf( text, 32, L"Item %d", i + 1 );
        SendMessageW( ctl[ID_LIST], LB_ADDSTRING, 0, (LPARAM)text );
        SendMessageW( ctl[ID_MULTI], LB_ADDSTRING, 0, (LPARAM)text );
    }

    make( WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 408, 16, 300, 150, ID_REPORT );
    make( WC_LISTVIEWW, NULL, LVS_LIST | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP, 408, 176, 300, 90, ID_LVLIST );
    col.pszText = (WCHAR *)L"Name";
    col.cx = 160;
    SendMessageW( ctl[ID_REPORT], LVM_INSERTCOLUMNW, 0, (LPARAM)&col );
    col.pszText = (WCHAR *)L"Size";
    col.cx = 100;
    SendMessageW( ctl[ID_REPORT], LVM_INSERTCOLUMNW, 1, (LPARAM)&col );
    for (i = 0; i < 5; i++)
    {
        WCHAR name[32], size[32];
        swprintf( name, 32, L"file%d.txt", i + 1 );
        swprintf( size, 32, L"%d KB", (i + 1) * 12 );
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = name;
        SendMessageW( ctl[ID_REPORT], LVM_INSERTITEMW, 0, (LPARAM)&item );
        SendMessageW( ctl[ID_LVLIST], LVM_INSERTITEMW, 0, (LPARAM)&item );
        item.iSubItem = 1;
        item.pszText = size;
        SendMessageW( ctl[ID_REPORT], LVM_SETITEMTEXTW, i, (LPARAM)&item );
    }

    make( PROGRESS_CLASSW, NULL, 0, 408, 276, 300, 18, ID_PROGRESS );
    SendMessageW( ctl[ID_PROGRESS], PBM_SETPOS, 60, 0 );
    make( TRACKBAR_CLASSW, NULL, TBS_AUTOTICKS | WS_TABSTOP, 408, 300, 300, 30, ID_TRACK );
    SendMessageW( ctl[ID_TRACK], TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
    SendMessageW( ctl[ID_TRACK], TBM_SETTICFREQ, 10, 0 );
    SendMessageW( ctl[ID_TRACK], TBM_SETPOS, TRUE, 30 );

    make( WC_TABCONTROLW, NULL, WS_TABSTOP, 408, 340, 300, 60, ID_TAB );
    tab.pszText = (WCHAR *)L"General";
    SendMessageW( ctl[ID_TAB], TCM_INSERTITEMW, 0, (LPARAM)&tab );
    tab.pszText = (WCHAR *)L"Graphics";
    SendMessageW( ctl[ID_TAB], TCM_INSERTITEMW, 1, (LPARAM)&tab );
    tab.pszText = (WCHAR *)L"Audio";
    SendMessageW( ctl[ID_TAB], TCM_INSERTITEMW, 2, (LPARAM)&tab );

    make( L"Button", L"Message box…", BS_PUSHBUTTON | WS_TABSTOP, 16, 368, 120, 24, ID_MSGBOX );
    make( L"Button", L"Open…", BS_PUSHBUTTON | WS_TABSTOP, 144, 368, 120, 24, ID_OPEN );
    make( L"Button", L"Save…", BS_PUSHBUTTON | WS_TABSTOP, 272, 368, 120, 24, ID_SAVE );

    create_controls2();
}

/* milestone 2, in the third column (x 724..1124) and along the bottom */
static void create_controls2(void)
{
    ULONG_PTR cookie;

    make( L"Button", L"Three-&state", BS_AUTO3STATE | WS_TABSTOP, 724, 16, 180, 20, ID_3STATE );
    SendMessageW( ctl[ID_3STATE], BM_SETCHECK, BST_INDETERMINATE, 0 );
    if (v6_begin( &cookie ))
    {
        make( L"Button", L"&Split", BS_SPLITBUTTON | WS_TABSTOP, 916, 14, 150, 26, ID_SPLIT );
        make( L"Button", L"Command &link", BS_COMMANDLINK | WS_TABSTOP, 724, 48, 392, 56, ID_CMDLINK );
        SendMessageW( ctl[ID_CMDLINK], BCM_SETNOTE, 0, (LPARAM)L"With a note under the title" );
        v6_end( cookie );
    }
    make( L"Edit", L"Line one\r\nLine two\r\nLine three",
          ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 724, 180, 392, 110, ID_MLEDIT );
    make( L"ComboBox", NULL, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 724, 298, 180, 200, ID_ECOMBO );
    SendMessageW( ctl[ID_ECOMBO], CB_ADDSTRING, 0, (LPARAM)L"Red" );
    SendMessageW( ctl[ID_ECOMBO], CB_ADDSTRING, 0, (LPARAM)L"Green" );
    SendMessageW( ctl[ID_ECOMBO], CB_ADDSTRING, 0, (LPARAM)L"Blue" );
    SendMessageW( ctl[ID_ECOMBO], CB_SETCURSEL, 1, 0 );
    make( WC_TREEVIEWW, NULL, TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
          724, 330, 180, 160, ID_TREE );
    {
        TVINSERTSTRUCTW ins = { TVI_ROOT, TVI_LAST };
        ins.item.mask = TVIF_TEXT;
        ins.item.pszText = (WCHAR *)L"Fruits";
        tree_fruits = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        /* filled when it is first expanded, as file browsers do */
        ins.item.mask = TVIF_TEXT | TVIF_CHILDREN;
        ins.item.cChildren = 1;
        ins.item.pszText = (WCHAR *)L"Vegetables";
        tree_veg = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        ins.hParent = tree_fruits;
        ins.item.mask = TVIF_TEXT;
        ins.item.pszText = (WCHAR *)L"Apple";
        tree_apple = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        ins.item.pszText = (WCHAR *)L"Pear";
        tree_pear = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
    }
    make( L"Edit", L"5", ES_NUMBER | WS_BORDER | WS_TABSTOP, 916, 112, 80, 22, ID_UDEDIT );
    make( UPDOWN_CLASSW, NULL, UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0, ID_UPDOWN );
    SendMessageW( ctl[ID_UPDOWN], UDM_SETRANGE32, 0, 100 );
    SendMessageW( ctl[ID_UPDOWN], UDM_SETPOS32, 0, 5 );
    {
        SYSTEMTIME st = { 2024, 5, 5, 17, 9, 30, 0, 0 };
        make( DATETIMEPICK_CLASSW, NULL, DTS_SHORTDATECENTURYFORMAT | WS_TABSTOP, 916, 142, 180, 24, ID_DATETIME );
        SendMessageW( ctl[ID_DATETIME], DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st );
        make( MONTHCAL_CLASSW, NULL, WS_BORDER | WS_TABSTOP, 916, 330, 208, 160, ID_MONTHCAL );
        SendMessageW( ctl[ID_MONTHCAL], MCM_SETCURSEL, 0, (LPARAM)&st );
    }
    {
        TTTOOLINFOW ti = { sizeof(ti) };
        ctl[ID_TOOLTIP] = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                           main_window, NULL, GetModuleHandleW( NULL ), NULL );
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = main_window;
        ti.uId = (UINT_PTR)ctl[ID_PUSH];
        ti.lpszText = (WCHAR *)L"Pushes the button";
        SendMessageW( ctl[ID_TOOLTIP], TTM_ADDTOOLW, 0, (LPARAM)&ti );
        ti.uId = (UINT_PTR)ctl[ID_CHECK];
        ti.lpszText = LPSTR_TEXTCALLBACKW;      /* answered in WM_NOTIFY/TTN_GETDISPINFO */
        SendMessageW( ctl[ID_TOOLTIP], TTM_ADDTOOLW, 0, (LPARAM)&ti );
    }
    {
        static const WCHAR *tasks[] = { L"Write", L"Test", L"Ship" };
        static const WCHAR *names[] = { L"App", L"Info", L"Warning" };
        static const LPCWSTR icons[] = { (LPCWSTR)IDI_APPLICATION, (LPCWSTR)IDI_INFORMATION, (LPCWSTR)IDI_WARNING };
        LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH };
        LVITEMW item = { LVIF_TEXT };
        HIMAGELIST himl = ImageList_Create( 32, 32, ILC_COLOR32 | ILC_MASK, 3, 0 );
        int i;

        /* check boxes come as an extended style after creation: the runtime picks the entry again */
        make( WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 16, 410, 300, 120, ID_LVCHECK );
        SendMessageW( ctl[ID_LVCHECK], LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT,
                      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT );
        col.pszText = (WCHAR *)L"Task";
        col.cx = 200;
        SendMessageW( ctl[ID_LVCHECK], LVM_INSERTCOLUMNW, 0, (LPARAM)&col );
        make( WC_LISTVIEWW, NULL, LVS_ICON | LVS_AUTOARRANGE | WS_BORDER | WS_TABSTOP, 332, 410, 376, 120, ID_LVICON );
        for (i = 0; i < 3; i++) ImageList_AddIcon( himl, LoadIconW( NULL, icons[i] ) );
        SendMessageW( ctl[ID_LVICON], LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)himl );
        for (i = 0; i < 3; i++)
        {
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = (WCHAR *)tasks[i];
            SendMessageW( ctl[ID_LVCHECK], LVM_INSERTITEMW, 0, (LPARAM)&item );
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iImage = i;
            item.pszText = (WCHAR *)names[i];
            SendMessageW( ctl[ID_LVICON], LVM_INSERTITEMW, 0, (LPARAM)&item );
        }
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK( 2 );
        SendMessageW( ctl[ID_LVCHECK], LVM_SETITEMSTATE, 1, (LPARAM)&item );
    }
    {
        static const WCHAR *titles[] = { L"Task dialog…", L"Folder…", L"Colour…", L"Font…", L"Print…",
                                         L"Item dialog…", L"Property sheet…", L"Wizard…" };
        int i;
        for (i = 0; i < ARRAYSIZE(titles); i++)
            make( L"Button", titles[i], BS_PUSHBUTTON | WS_TABSTOP, 16 + i * 128, 544, 120, 24, ID_TASKDLG + i );
    }
    make( L"Static", NULL, SS_ETCHEDFRAME, 724, 112, 120, 60, ID_FRAME );
    make( L"Static", NULL, SS_GRAYRECT, 852, 112, 40, 60, ID_RECT );

    {
        static const int parts[] = { 200, 400, -1 };
        make( STATUSCLASSNAMEW, NULL, SBARS_SIZEGRIP, 0, 0, 0, 0, ID_STATUS );
        SendMessageW( ctl[ID_STATUS], SB_SETPARTS, ARRAYSIZE(parts), (LPARAM)parts );
        SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 0, (LPARAM)L"Ready" );
        SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 1, (LPARAM)L"Ln 1, Col 1" );
        SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 2, (LPARAM)L"\tUTF-8" );
    }
}

static void open_file( BOOL save )
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = main_window;
    ofn.lpstrFilter = L"Text files\0*.txt;*.log\0All files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW( &ofn ) : GetOpenFileNameW( &ofn ))
        MessageBoxW( main_window, file, save ? L"Save to" : L"Opened", MB_OK | MB_ICONINFORMATION );
}

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_COMMAND:
    {
        int id = LOWORD( wparam ), code = HIWORD( wparam );
        if (id <= ID_PUSHLIKE && lparam) got_command[id][code & 15]++;
        if (code == BN_CLICKED && id == ID_MSGBOX)
        {
            int r = MessageBoxW( hwnd, L"Do you want to save the changes you made?",
                                 L"Gallery", MB_YESNOCANCEL | MB_ICONWARNING );
            WCHAR text[64];
            swprintf( text, 64, L"MessageBox returned %d", r );
            SetWindowTextW( ctl[ID_LABEL], text );
        }
        if (code == BN_CLICKED && (id == ID_OPEN || id == ID_SAVE)) open_file( id == ID_SAVE );
        if (code == BN_CLICKED && (id == ID_FOLDER || id == ID_ITEMDLG) && !in_selftest)
        {
            WCHAR path[MAX_PATH] = L"";
            if (id == ID_FOLDER ? browse_for_folder( path ) : SUCCEEDED(item_dialog_folder( path )))
                SetWindowTextW( ctl[ID_LABEL], path );
        }
        if (code == BN_CLICKED && id == ID_TASKDLG && !in_selftest)
        {
            int button = 0, radio = 0;
            BOOL verified = FALSE;
            WCHAR text[96];
            run_task_dialog( &button, &radio, &verified );
            swprintf( text, 96, L"Task dialog: button %d, radio %d, verified %d", button, radio, verified );
            SetWindowTextW( ctl[ID_LABEL], text );
        }
        return 0;
    }
    case WM_SIZE:
        if (ctl[ID_STATUS]) SendMessageW( ctl[ID_STATUS], WM_SIZE, 0, 0 );
        return 0;
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lparam;
        if (dis->hwndItem == ctl[ID_STATUS])
        {
            SetBkMode( dis->hDC, TRANSPARENT );
            DrawTextW( dis->hDC, L"drawn by the app", -1, &dis->rcItem, DT_SINGLELINE | DT_VCENTER | DT_CENTER );
            return TRUE;
        }
        return FALSE;
    }
    case WM_HSCROLL:
        if ((HWND)lparam == ctl[ID_TRACK]) got_hscroll++;
        return 0;
    case WM_VSCROLL:
        if ((HWND)lparam == ctl[ID_UPDOWN]) got_vscroll++;
        return 0;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lparam;
        if (hdr->hwndFrom == ctl[ID_TAB] && hdr->code == TCN_SELCHANGE) got_tabchange++;
        if (hdr->hwndFrom == ctl[ID_REPORT] && hdr->code == LVN_ITEMCHANGED) got_lvchanged++;
        if (hdr->hwndFrom == ctl[ID_TREE] && hdr->code == TVN_ITEMEXPANDINGW)
        {
            NMTREEVIEWW *nm = (NMTREEVIEWW *)lparam;
            got_expanding++;
            if (nm->action == TVE_EXPAND && nm->itemNew.hItem == tree_veg &&
                !SendMessageW( hdr->hwndFrom, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)tree_veg ))
            {
                TVINSERTSTRUCTW ins = { tree_veg, TVI_LAST };
                ins.item.mask = TVIF_TEXT;
                ins.item.pszText = (WCHAR *)L"Carrot";
                SendMessageW( hdr->hwndFrom, TVM_INSERTITEMW, 0, (LPARAM)&ins );
                ins.item.pszText = (WCHAR *)L"Leek";
                SendMessageW( hdr->hwndFrom, TVM_INSERTITEMW, 0, (LPARAM)&ins );
            }
        }
        if (hdr->hwndFrom == ctl[ID_TREE] && hdr->code == TVN_SELCHANGEDW) got_treesel++;
        if (hdr->hwndFrom == ctl[ID_UPDOWN] && hdr->code == UDN_DELTAPOS) got_deltapos++;
        if (hdr->hwndFrom == ctl[ID_LVCHECK] && hdr->code == LVN_ITEMCHANGED &&
            (((NMLISTVIEW *)lparam)->uChanged & LVIF_STATE) &&
            ((((NMLISTVIEW *)lparam)->uNewState ^ ((NMLISTVIEW *)lparam)->uOldState) & LVIS_STATEIMAGEMASK))
            got_check_changed++;
        if (hdr->hwndFrom == ctl[ID_LVICON] && hdr->code == LVN_ITEMCHANGED) got_icon_changed++;
        if (hdr->hwndFrom == ctl[ID_LVICON] && hdr->code == LVN_ITEMACTIVATE) got_icon_activate++;
        if (hdr->hwndFrom == ctl[ID_TOOLTIP] && hdr->code == TTN_GETDISPINFOW && hdr->idFrom == (UINT_PTR)ctl[ID_CHECK])
        {
            got_dispinfo++;
            ((NMTTDISPINFOW *)lparam)->lpszText = (WCHAR *)L"Toggles the option";
        }
        if (hdr->hwndFrom == ctl[ID_DATETIME] && hdr->code == DTN_DATETIMECHANGE) got_datechange++;
        if (hdr->hwndFrom == ctl[ID_MONTHCAL] && hdr->code == MCN_SELCHANGE) got_mcselchange++;
        if (hdr->hwndFrom == ctl[ID_MONTHCAL] && hdr->code == MCN_SELECT) got_mcselect++;
        if (hdr->hwndFrom == ctl[ID_STATUS] && hdr->code == NM_CLICK)
            got_status_click = (int)((NMMOUSE *)lparam)->dwItemSpec;
        if (hdr->hwndFrom == ctl[ID_SPLIT] && hdr->code == BCN_DROPDOWN)
        {
            NMBCDROPDOWN *nm = (NMBCDROPDOWN *)lparam;
            got_dropdown++;
            if (!in_selftest)
            {
                HMENU menu = CreatePopupMenu();
                POINT pt = { nm->rcButton.left, nm->rcButton.bottom };
                AppendMenuW( menu, MF_STRING, 1, L"First choice" );
                AppendMenuW( menu, MF_STRING, 2, L"Second choice" );
                ClientToScreen( hdr->hwndFrom, &pt );
                TrackPopupMenu( menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL );
                DestroyMenu( menu );
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* ---------- self-test ---------- */

static int failures, passes;

static void pump( int ms )
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((int)(end - GetTickCount()) > 0)
    {
        MsgWaitForMultipleObjects( 0, NULL, FALSE, 10, QS_ALLINPUT );
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    }
}

static void check( BOOL ok, const char *what )
{
    printf( "%s  %s\n", ok ? "PASS" : "FAIL", what );
    fflush( stdout );
    if (ok) passes++; else failures++;
}

static BOOL query_has( HWND hwnd, const char *needle )
{
    char *q = pQuery( hwnd );
    BOOL ok = q && strstr( q, needle );
    if (!ok) printf( "      query: %s\n", q ? q : "(null)" );
    pFree( q );
    return ok;
}

static BOOL query_lacks( HWND hwnd, const char *needle )
{
    char *q = pQuery( hwnd );
    BOOL ok = q && !strstr( q, needle );
    if (!ok) printf( "      query: %s\n", q ? q : "(null)" );
    pFree( q );
    return ok;
}

static void inject( HWND hwnd, const char *event )
{
    char *r = pInject( hwnd, event );
    if (!r || !strstr( r, "ok" )) printf( "      inject %s: %s\n", event, r ? r : "(null)" );
    fflush( stdout );
    pFree( r );
}

static void CALLBACK press_alert_button( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"alertButton\",\"v\":1}" );
}

static void CALLBACK task_dialog_second( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"press\",\"v\":1001}" );
}

/* while the task dialog is up: read it, then answer it natively */
static void CALLBACK task_dialog_first( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    char *q = pQuery( NULL );

    KillTimer( hwnd, id );
    td_native_ok = q && strstr( q, "\"progressPos\":40" ) && strstr( q, "Updated content" );
    if (!td_native_ok) printf( "      task dialog: %s\n", q ? q : "(null)" );
    pFree( q );
    inject( NULL, "{\"t\":\"radio\",\"v\":2002}" );
    inject( NULL, "{\"t\":\"verify\",\"v\":1}" );
    inject( NULL, "{\"t\":\"press\",\"v\":1002}" );    /* refused by the callback */
    SetTimer( hwnd, 4, 500, task_dialog_second );
}

/* while the folder picker is up: check what the callback set, browse, choose */
static void CALLBACK folder_panel( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    char *q = pQuery( NULL );

    KillTimer( hwnd, id );
    bff_native_ok = q && strstr( q, "\"prompt\":\"Use Folder\"" ) && strstr( q, "\"folders\":true" ) &&
                    strstr( q, "Choose the folder to export to" ) && strstr( q, "tmp" );
    if (!bff_native_ok) printf( "      folder panel: %s\n", q ? q : "(null)" );
    pFree( q );
    inject( NULL, "{\"t\":\"look\",\"s\":\"/tmp\"}" );
    inject( NULL, "{\"t\":\"choose\",\"s\":\"/tmp\"}" );
}

static void CALLBACK choose_folder( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"choose\",\"s\":\"/tmp\"}" );
}

static void CALLBACK choose_file( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"choose\",\"s\":\"/tmp/w2s-gallery-test.txt\"}" );
}

/* milestone 2 controls, both ways */
static void selftest2(void)
{
    WCHAR text[256];

    /* three-state check box */
    check( query_has( ctl[ID_3STATE], "\"checked\":2" ), "BST_INDETERMINATE reaches the native check box" );
    inject( ctl[ID_3STATE], "{\"t\":\"click\"}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_3STATE], BM_GETCHECK, 0, 0 ) != BST_INDETERMINATE &&
           got_command[ID_3STATE][BN_CLICKED] > 0, "native click moves the three-state box on (BN_CLICKED)" );

    /* split button */
    {
        BUTTON_SPLITINFO info = { BCSIF_STYLE };
        info.uSplitStyle = BCSS_NOSPLIT;
        SendMessageW( ctl[ID_SPLIT], BCM_SETSPLITINFO, 0, (LPARAM)&info );
        pump( 150 );
        check( query_has( ctl[ID_SPLIT], "\"noSplit\":true" ), "BCM_SETSPLITINFO reaches the native split button" );
        info.uSplitStyle = 0;
        SendMessageW( ctl[ID_SPLIT], BCM_SETSPLITINFO, 0, (LPARAM)&info );
    }
    inject( ctl[ID_SPLIT], "{\"t\":\"click\"}" );
    inject( ctl[ID_SPLIT], "{\"t\":\"dropdown\"}" );
    pump( 200 );
    check( got_command[ID_SPLIT][BN_CLICKED] > 0 && got_dropdown > 0, "native split button -> BN_CLICKED and BCN_DROPDOWN" );

    /* command link */
    SendMessageW( ctl[ID_CMDLINK], BCM_SETNOTE, 0, (LPARAM)L"A new note" );
    pump( 150 );
    check( query_has( ctl[ID_CMDLINK], "\"note\":\"A new note\"" ), "BCM_SETNOTE reaches the native command link" );
    inject( ctl[ID_CMDLINK], "{\"t\":\"click\"}" );
    pump( 200 );
    check( got_command[ID_CMDLINK][BN_CLICKED] > 0, "native command link click -> BN_CLICKED" );

    /* frames and rectangles: drawn, never in the way */
    check( query_has( ctl[ID_FRAME], "\"passThrough\":true" ), "the native frame lets clicks through" );
    SetWindowLongW( ctl[ID_RECT], GWL_STYLE, (GetWindowLongW( ctl[ID_RECT], GWL_STYLE ) & ~SS_TYPEMASK) | SS_BLACKRECT );
    pump( 150 );
    check( query_has( ctl[ID_RECT], "\"fill\":\"label\"" ) && query_has( ctl[ID_RECT], "\"entry\":\"static.frame\"" ),
           "a style change within the family updates the same native view" );

    /* status bar */
    SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 1, (LPARAM)L"Ln 2, Col 5" );
    pump( 150 );
    check( query_has( ctl[ID_STATUS], "Ln 2, Col 5" ), "SB_SETTEXT reaches the native status bar" );
    inject( ctl[ID_STATUS], "{\"t\":\"click\",\"v\":1}" );
    pump( 200 );
    check( got_status_click == 1, "native click on a pane -> NM_CLICK for that pane" );
    {
        HRGN rgn = CreateRectRgn( 0, 0, 0, 0 );
        RECT box = { 0 }, pane = { 0 };
        SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 2 | SBT_OWNERDRAW, 0 );
        pump( 150 );
        SendMessageW( ctl[ID_STATUS], SB_GETRECT, 2, (LPARAM)&pane );
        GetWindowRgn( ctl[ID_STATUS], rgn );
        GetRgnBox( rgn, &box );
        check( EqualRect( &box, &pane ) && query_has( ctl[ID_STATUS], "\"ownerDraw\":true" ),
               "an owner-drawn pane is left to wine (window region = that pane)" );
        SendMessageW( ctl[ID_STATUS], SB_SETTEXTW, 2, (LPARAM)L"\tUTF-8" );
        pump( 150 );
        check( GetWindowRgn( ctl[ID_STATUS], rgn ) == NULLREGION, "back to an empty region without it" );
        DeleteObject( rgn );
    }
    /* multi-line edit: CRLF on the Win32 side, "\n" natively */
    SetWindowTextW( ctl[ID_MLEDIT], L"a\r\nb\r\nc\r\nd" );
    check( SendMessageW( ctl[ID_MLEDIT], EM_GETLINECOUNT, 0, 0 ) == 4,
           "answers stay right before the native view has the new text (Win32 answers)" );
    SetWindowTextW( ctl[ID_MLEDIT], L"Alpha\r\nBeta" );
    pump( 200 );
    check( query_has( ctl[ID_MLEDIT], "\"text\":\"Alpha\\nBeta\"" ), "WM_SETTEXT reaches the native text view (CRLF -> LF)" );
    SendMessageW( ctl[ID_MLEDIT], EM_SETSEL, 7, 11 );
    pump( 200 );
    check( query_has( ctl[ID_MLEDIT], "\"sel\":[6,10]" ), "EM_SETSEL reaches the native text view (offsets without CR)" );
    inject( ctl[ID_MLEDIT], "{\"t\":\"replace\",\"a\":[5,0],\"s\":\"\\n!\"}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_MLEDIT], text, ARRAYSIZE(text) );
    check( !wcscmp( text, L"Alpha\r\n!\r\nBeta" ) && got_command[ID_MLEDIT][EN_CHANGE & 15] > 0,
           "a native edit replaces its range in Win32 (LF -> CRLF) with EN_CHANGE" );
    inject( ctl[ID_MLEDIT], "{\"t\":\"sel\",\"a\":[2,4]}" );
    pump( 300 );
    {
        char *q = pQuery( ctl[ID_MLEDIT] ), *p = q ? strstr( q, "\"peAnswers\":" ) : NULL;
        unsigned int before = p ? atoi( p + 12 ) : 0;
        DWORD start = 0, end = 0;
        LRESULT lines, index;
        pFree( q );
        SendMessageW( ctl[ID_MLEDIT], EM_GETSEL, (WPARAM)&start, (LPARAM)&end );
        lines = SendMessageW( ctl[ID_MLEDIT], EM_GETLINECOUNT, 0, 0 );
        index = SendMessageW( ctl[ID_MLEDIT], EM_LINEINDEX, 2, 0 );
        q = pQuery( ctl[ID_MLEDIT] );
        p = q ? strstr( q, "\"peAnswers\":" ) : NULL;
        printf( "      EM_GETSEL %lu-%lu, %d lines, line 2 at %d, answered %u -> %d\n", start, end, (int)lines,
                (int)index, before, p ? atoi( p + 12 ) : -1 );
        check( start == 2 && end == 4 && lines == 3 && index == 10,
               "EM_GETSEL / EM_GETLINECOUNT / EM_LINEINDEX give the native selection and lines" );
        check( p && (unsigned int)atoi( p + 12 ) == before + 3, "... and the native view answered them" );
        pFree( q );
    }

    /* editable combo box */
    SetWindowTextW( ctl[ID_ECOMBO], L"Purple" );
    SendMessageW( ctl[ID_ECOMBO], CB_ADDSTRING, 0, (LPARAM)L"Cyan" );
    pump( 150 );
    check( query_has( ctl[ID_ECOMBO], "\"text\":\"Purple\"" ) && query_has( ctl[ID_ECOMBO], "\"Cyan\"" ),
           "the combo box's text and items reach the native combo box" );
    inject( ctl[ID_ECOMBO], "{\"t\":\"text\",\"s\":\"Teal\"}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_ECOMBO], text, ARRAYSIZE(text) );
    check( !wcscmp( text, L"Teal" ) && got_command[ID_ECOMBO][CBN_EDITCHANGE] > 0,
           "native typing -> the combo box's edit part + CBN_EDITCHANGE" );
    inject( ctl[ID_ECOMBO], "{\"t\":\"select\",\"v\":2}" );
    inject( ctl[ID_ECOMBO], "{\"t\":\"open\"}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_ECOMBO], text, ARRAYSIZE(text) );
    check( SendMessageW( ctl[ID_ECOMBO], CB_GETCURSEL, 0, 0 ) == 2 && !wcscmp( text, L"Blue" ) &&
           got_command[ID_ECOMBO][CBN_SELCHANGE] > 0 && got_command[ID_ECOMBO][CBN_DROPDOWN] > 0,
           "native choice -> CB_GETCURSEL, the edit text, CBN_SELCHANGE; opening -> CBN_DROPDOWN" );

    /* tree view */
    {
        char event[128], needle[64];
        HWND tree = ctl[ID_TREE];

        check( query_has( tree, "\"text\":\"Vegetables\",\"kids\":true" ) || query_has( tree, "\"kids\":true" ),
               "the tree's nodes reach the native outline" );
        SendMessageW( tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)tree_fruits );
        SendMessageW( tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)tree_pear );
        pump( 150 );
        snprintf( needle, sizeof(needle), "\"selection\":%lld", (long long)(INT_PTR)tree_pear );
        check( query_has( tree, "\"Apple\"" ) && query_has( tree, needle ),
               "TVM_EXPAND and TVM_SELECTITEM reach the native outline" );
        snprintf( event, sizeof(event), "{\"t\":\"expand\",\"v\":%lld}", (long long)(INT_PTR)tree_veg );
        inject( tree, event );
        pump( 250 );
        check( got_expanding > 0 && (SendMessageW( tree, TVM_GETITEMSTATE, (WPARAM)tree_veg, TVIS_EXPANDED ) & TVIS_EXPANDED) &&
               query_has( tree, "\"Carrot\"" ), "native expansion -> TVN_ITEMEXPANDING first, then the children appear" );
        got_expanding = 0;
        snprintf( event, sizeof(event), "{\"t\":\"collapse\",\"v\":%lld}", (long long)(INT_PTR)tree_veg );
        inject( tree, event );
        snprintf( event, sizeof(event), "{\"t\":\"select\",\"v\":%lld}", (long long)(INT_PTR)tree_apple );
        inject( tree, event );
        pump( 250 );
        check( got_expanding > 0 && !(SendMessageW( tree, TVM_GETITEMSTATE, (WPARAM)tree_veg, TVIS_EXPANDED ) & TVIS_EXPANDED),
               "native collapse -> TVN_ITEMEXPANDING, collapsed" );
        check( (HTREEITEM)SendMessageW( tree, TVM_GETNEXTITEM, TVGN_CARET, 0 ) == tree_apple && got_treesel > 0,
               "native selection -> TVM_GETNEXTITEM(TVGN_CARET) + TVN_SELCHANGED" );
    }

    /* up-down with its buddy edit */
    SendMessageW( ctl[ID_UPDOWN], UDM_SETPOS32, 0, 42 );
    pump( 150 );
    check( query_has( ctl[ID_UPDOWN], "\"value\":42" ) && query_has( ctl[ID_UDEDIT], "\"text\":\"42\"" ),
           "UDM_SETPOS32 reaches the native stepper and the buddy field" );
    inject( ctl[ID_UPDOWN], "{\"t\":\"step\",\"v\":1}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_UDEDIT], text, ARRAYSIZE(text) );
    check( SendMessageW( ctl[ID_UPDOWN], UDM_GETPOS32, 0, 0 ) == 43 && !wcscmp( text, L"43" ) &&
           got_deltapos > 0 && got_vscroll > 0, "native step -> UDN_DELTAPOS, position 43, buddy text, WM_VSCROLL" );

    /* date and time picker */
    {
        SYSTEMTIME st = { 2023, 12, 0, 24, 18, 0, 0, 0 };
        SendMessageW( ctl[ID_DATETIME], DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st );
        pump( 150 );
        check( query_has( ctl[ID_DATETIME], "\"date\":[2023,12,24,18,0,0]" ), "DTM_SETSYSTEMTIME reaches the native date picker" );
        inject( ctl[ID_DATETIME], "{\"t\":\"date\",\"a\":[2025,1,2,0,0,0]}" );
        pump( 200 );
        SendMessageW( ctl[ID_DATETIME], DTM_GETSYSTEMTIME, 0, (LPARAM)&st );
        check( st.wYear == 2025 && st.wMonth == 1 && st.wDay == 2 && st.wHour == 18 && got_datechange > 0,
               "native date -> DTM_GETSYSTEMTIME (time kept) + DTN_DATETIMECHANGE" );
    }

    /* tooltips become .help on the controls that are tools */
    check( query_has( ctl[ID_PUSH], "\"help\":\"Pushes the button\"" ), "a tooltip tool's text reaches its native control" );
    check( query_has( ctl[ID_CHECK], "\"help\":\"Toggles the option\"" ) && got_dispinfo > 0,
           "a callback tool's text comes from the app (TTN_GETDISPINFO)" );
    {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.hwnd = main_window;
        ti.uId = (UINT_PTR)ctl[ID_PUSH];
        ti.lpszText = (WCHAR *)L"Pushes it now";
        SendMessageW( ctl[ID_TOOLTIP], TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti );
        pump( 150 );
        check( query_has( ctl[ID_PUSH], "\"help\":\"Pushes it now\"" ), "TTM_UPDATETIPTEXT updates the native help" );
        SendMessageW( ctl[ID_TOOLTIP], TTM_ACTIVATE, FALSE, 0 );
        pump( 150 );
        check( query_lacks( ctl[ID_PUSH], "\"help\"" ), "TTM_ACTIVATE FALSE removes it" );
        SendMessageW( ctl[ID_TOOLTIP], TTM_ACTIVATE, TRUE, 0 );
    }

    /* list views: list mode, check boxes, icons */
    check( query_has( ctl[ID_LVLIST], "\"rowCount\":5" ), "items inserted into a list-mode list view reach the native list" );
    check( query_has( ctl[ID_LVCHECK], "\"entry\":\"listview.checkboxes\"" ) &&
           query_has( ctl[ID_LVCHECK], "\"checks\":[false,true,false]" ),
           "LVS_EX_CHECKBOXES after creation makes the report a check-box list" );
    {
        LVITEMW item = { 0 };
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK( 2 );
        SendMessageW( ctl[ID_LVCHECK], LVM_SETITEMSTATE, 0, (LPARAM)&item );
        pump( 150 );
        check( query_has( ctl[ID_LVCHECK], "\"checks\":[true,true,false]" ), "a Win32 check reaches the native box" );
    }
    inject( ctl[ID_LVCHECK], "{\"t\":\"check\",\"a\":[2,1]}" );
    pump( 200 );
    check( (SendMessageW( ctl[ID_LVCHECK], LVM_GETITEMSTATE, 2, LVIS_STATEIMAGEMASK ) >> 12) == 2 && got_check_changed > 0,
           "a native check -> the state image + LVN_ITEMCHANGED" );
    check( query_has( ctl[ID_LVICON], "\"entry\":\"listview.icon\"" ) && query_has( ctl[ID_LVICON], "\"icons\":[0,1,2]" ) &&
           query_has( ctl[ID_LVICON], "\"imageCount\":3" ), "the icon view and its image list reach the native grid" );
    {
        LVITEMW item = { 0 };
        item.pszText = (WCHAR *)L"Information";
        SendMessageW( ctl[ID_LVICON], LVM_SETITEMTEXTW, 1, (LPARAM)&item );
        pump( 150 );
        check( query_has( ctl[ID_LVICON], "\"Information\"" ), "LVM_SETITEMTEXT reaches the native grid" );
    }
    inject( ctl[ID_LVICON], "{\"t\":\"selectMany\",\"a\":[2]}" );
    inject( ctl[ID_LVICON], "{\"t\":\"activate\",\"v\":1}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_LVICON], LVM_GETNEXTITEM, -1, LVNI_SELECTED ) == 1 && got_icon_changed > 0 &&
           got_icon_activate > 0, "native selection and double click -> LVN_ITEMCHANGED, LVN_ITEMACTIVATE" );
    SendMessageW( ctl[ID_LVICON], LVM_SETVIEW, LV_VIEW_DETAILS, 0 );
    pump( 150 );
    check( query_has( ctl[ID_LVICON], "\"entry\":\"listview.report\"" ), "LVM_SETVIEW switches the native view" );
    SendMessageW( ctl[ID_LVICON], LVM_SETVIEW, LV_VIEW_ICON, 0 );
    pump( 150 );
    check( query_has( ctl[ID_LVICON], "\"entry\":\"listview.icon\"" ) && query_has( ctl[ID_LVICON], "\"imageCount\":3" ),
           "... and back, with its icons sent again" );

    /* month calendar */
    {
        SYSTEMTIME st = { 2024, 2, 0, 29, 0, 0, 0, 0 };
        SendMessageW( ctl[ID_MONTHCAL], MCM_SETCURSEL, 0, (LPARAM)&st );
        pump( 150 );
        check( query_has( ctl[ID_MONTHCAL], "\"date\":[2024,2,29," ), "MCM_SETCURSEL reaches the native calendar" );
        inject( ctl[ID_MONTHCAL], "{\"t\":\"date\",\"a\":[2024,3,3]}" );
        pump( 200 );
        SendMessageW( ctl[ID_MONTHCAL], MCM_GETCURSEL, 0, (LPARAM)&st );
        check( st.wMonth == 3 && st.wDay == 3 && got_mcselchange > 0 && got_mcselect > 0,
               "native day -> MCM_GETCURSEL + MCN_SELCHANGE + MCN_SELECT" );
    }
}

static int selftest(void)
{
    static const int ids[] = { ID_PUSH, ID_DEFAULT, ID_PUSHLIKE, ID_GROUP, ID_CHECK, ID_RADIO1, ID_RADIO2, ID_LABEL,
                               ID_SEP, ID_ICON, ID_EDIT, ID_PASSWORD, ID_NUMBER, ID_READONLY, ID_COMBO, ID_LIST,
                               ID_MULTI, ID_REPORT, ID_LVLIST, ID_PROGRESS, ID_TRACK, ID_TAB,
                               ID_3STATE, ID_SPLIT, ID_CMDLINK, ID_FRAME, ID_RECT, ID_STATUS, ID_MLEDIT,
                               ID_ECOMBO, ID_TREE, ID_UDEDIT, ID_UPDOWN, ID_DATETIME, ID_MONTHCAL,
                               ID_LVCHECK, ID_LVICON };
    HMODULE w2s = GetModuleHandleW( L"win32swiftui.dll" );
    WCHAR text[256], file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    char what[128];
    unsigned int i;
    int r;

    check( w2s != NULL, "win32swiftui.dll is loaded (WINE_MNC_NATIVE_UI on)" );
    if (!w2s) return failures;
    pQuery = (void *)GetProcAddress( w2s, "W2SDebugQuery" );
    pInject = (void *)GetProcAddress( w2s, "W2SDebugInject" );
    pFree = (void *)GetProcAddress( w2s, "W2SDebugFree" );
    pIsTranslated = (void *)GetProcAddress( w2s, "W2SIsTranslated" );
    pump( 1500 );

    for (i = 0; i < ARRAYSIZE(ids); i++)
    {
        snprintf( what, sizeof(what), "control %d is translated and in its window", ids[i] );
        check( pIsTranslated( ctl[ids[i]] ) && query_has( ctl[ids[i]], "\"inWindow\":true" ), what );
    }
    /* wine's own drawing of a translated control must never show */
    for (i = 0, r = 0; i < ARRAYSIZE(ids); i++)
    {
        HRGN rgn = CreateRectRgn( 0, 0, 1, 1 );
        if (GetWindowRgn( ctl[ids[i]], rgn ) == NULLREGION) r++;
        else printf( "      control %d has no empty window region\n", ids[i] );
        DeleteObject( rgn );
    }
    check( r == ARRAYSIZE(ids), "translated controls have an empty window region" );

    /* Win32 -> native */
    SendMessageW( ctl[ID_CHECK], BM_SETCHECK, BST_CHECKED, 0 );
    pump( 150 );
    check( query_has( ctl[ID_CHECK], "\"checked\":1" ), "BM_SETCHECK reaches the native check box" );
    SetWindowTextW( ctl[ID_EDIT], L"from Win32" );
    pump( 150 );
    check( query_has( ctl[ID_EDIT], "from Win32" ), "WM_SETTEXT reaches the native text field" );
    SendMessageW( ctl[ID_COMBO], CB_SETCURSEL, 2, 0 );
    pump( 150 );
    check( query_has( ctl[ID_COMBO], "\"selection\":2" ), "CB_SETCURSEL reaches the native pop-up" );
    SendMessageW( ctl[ID_LIST], LB_SETCURSEL, 3, 0 );
    pump( 150 );
    check( query_has( ctl[ID_LIST], "\"selection\":3" ), "LB_SETCURSEL reaches the native list" );
    SendMessageW( ctl[ID_PROGRESS], PBM_SETPOS, 80, 0 );
    pump( 150 );
    check( query_has( ctl[ID_PROGRESS], "\"value\":80" ), "PBM_SETPOS reaches the native progress bar" );
    SendMessageW( ctl[ID_TRACK], TBM_SETPOS, TRUE, 70 );
    pump( 150 );
    check( query_has( ctl[ID_TRACK], "\"value\":70" ), "TBM_SETPOS reaches the native slider" );
    check( query_has( ctl[ID_REPORT], "\"columns\":[\"Name\",\"Size\"]" ), "every report column reaches the native table" );
    {
        LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH };
        col.pszText = (WCHAR *)L"Type";
        col.cx = 80;
        SendMessageW( ctl[ID_REPORT], LVM_INSERTCOLUMNW, 2, (LPARAM)&col );
        pump( 150 );
        check( query_has( ctl[ID_REPORT], "\"columns\":[\"Name\",\"Size\",\"Type\"]" ), "LVM_INSERTCOLUMN adds a native column" );
        SendMessageW( ctl[ID_REPORT], LVM_SETCOLUMNWIDTH, 2, 0 );
        pump( 150 );
        check( query_has( ctl[ID_REPORT], "\"columns\":[\"Name\",\"Size\"]" ), "a zero-width column is hidden natively" );
        SendMessageW( ctl[ID_REPORT], LVM_DELETECOLUMN, 2, 0 );
    }
    SendMessageW( ctl[ID_TAB], TCM_SETCURSEL, 2, 0 );
    pump( 150 );
    check( query_has( ctl[ID_TAB], "\"selection\":2" ), "TCM_SETCURSEL reaches the native tabs" );
    ShowWindow( ctl[ID_PUSH], SW_HIDE );
    pump( 150 );
    check( query_has( ctl[ID_PUSH], "\"hidden\":true" ), "hiding the control hides the native view" );
    ShowWindow( ctl[ID_PUSH], SW_SHOW );
    pump( 150 );
    check( query_has( ctl[ID_PUSH], "\"hidden\":false" ), "showing it shows the native view again" );
    EnableWindow( ctl[ID_DEFAULT], FALSE );
    pump( 150 );
    check( query_has( ctl[ID_DEFAULT], "\"enabled\":false" ), "EnableWindow reaches the native button" );
    EnableWindow( ctl[ID_DEFAULT], TRUE );

    /* native -> Win32 */
    inject( ctl[ID_PUSH], "{\"t\":\"click\"}" );
    pump( 200 );
    check( got_command[ID_PUSH][BN_CLICKED] == 1, "native click -> BN_CLICKED" );
    inject( ctl[ID_CHECK], "{\"t\":\"click\"}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_CHECK], BM_GETCHECK, 0, 0 ) == BST_UNCHECKED, "native check box click toggles the Win32 state" );
    inject( ctl[ID_RADIO2], "{\"t\":\"click\"}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_RADIO2], BM_GETCHECK, 0, 0 ) == BST_CHECKED &&
           query_has( ctl[ID_RADIO2], "\"checked\":1" ), "native radio click checks it" );
    inject( ctl[ID_EDIT], "{\"t\":\"text\",\"s\":\"typed natively\"}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_EDIT], text, ARRAYSIZE(text) );
    check( !wcscmp( text, L"typed natively" ) && got_command[ID_EDIT][EN_CHANGE & 15] > 0,
           "native typing -> Win32 text + EN_CHANGE" );
    inject( ctl[ID_NUMBER], "{\"t\":\"text\",\"s\":\"42x\"}" );
    pump( 200 );
    GetWindowTextW( ctl[ID_NUMBER], text, ARRAYSIZE(text) );
    check( query_has( ctl[ID_NUMBER], "\"text\"" ), "ES_NUMBER field answers after native typing" );
    inject( ctl[ID_COMBO], "{\"t\":\"select\",\"v\":1}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_COMBO], CB_GETCURSEL, 0, 0 ) == 1 && got_command[ID_COMBO][CBN_SELCHANGE & 15] > 0,
           "native pop-up choice -> CB_GETCURSEL + CBN_SELCHANGE" );
    inject( ctl[ID_LIST], "{\"t\":\"select\",\"v\":5}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_LIST], LB_GETCURSEL, 0, 0 ) == 5 && got_command[ID_LIST][LBN_SELCHANGE & 15] > 0,
           "native list selection -> LB_GETCURSEL + LBN_SELCHANGE" );
    inject( ctl[ID_MULTI], "{\"t\":\"selectMany\",\"a\":[1,3]}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_MULTI], LB_GETSELCOUNT, 0, 0 ) == 2, "native multiple selection -> LB_GETSELCOUNT 2" );
    inject( ctl[ID_REPORT], "{\"t\":\"selectMany\",\"a\":[2]}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_REPORT], LVM_GETNEXTITEM, -1, LVNI_SELECTED ) == 2 && got_lvchanged > 0,
           "native table selection -> LVN_ITEMCHANGED" );
    inject( ctl[ID_TRACK], "{\"t\":\"valueEnd\",\"v\":40}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_TRACK], TBM_GETPOS, 0, 0 ) == 40 && got_hscroll > 0, "native slider -> TBM_GETPOS + WM_HSCROLL" );
    inject( ctl[ID_TAB], "{\"t\":\"select\",\"v\":1}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_TAB], TCM_GETCURSEL, 0, 0 ) == 1 && got_tabchange > 0, "native tab -> TCN_SELCHANGE" );

    selftest2();

    /* message box -> NSAlert: press the second button ("No") */
    SetTimer( main_window, 1, 800, press_alert_button );
    r = MessageBoxW( main_window, L"Save changes?", L"Gallery test", MB_YESNO | MB_ICONQUESTION );
    check( r == IDNO, "native alert returns IDNO for its second button" );

    /* task dialog -> NSAlert with an accessory */
    {
        int button = 0, radio = 0;
        BOOL verified = FALSE;
        HRESULT hr;

        SetTimer( main_window, 3, 1000, task_dialog_first );
        hr = run_task_dialog( &button, &radio, &verified );
        printf( "      task dialog returned %#lx: button %d, radio %d, verified %d\n", hr, button, radio, verified );
        check( hr == S_OK && td_created == 1 && td_native_ok,
               "TDN_CREATED, and TDM_SET_PROGRESS_BAR_POS / TDM_SET_ELEMENT_TEXT reach the open alert" );
        check( td_radio == 2002 && td_verify == 1 && td_vetoed == 1 && td_clicks == 2,
               "native radio, check box and buttons -> TDN_* (a refused button keeps it open)" );
        check( button == 1001 && radio == 2002 && verified, "the task dialog returns the button, radio and check box" );
    }

    /* open panel: choose a Unix path, get a drive path back */
    CloseHandle( CreateFileW( L"Z:\\tmp\\w2s-gallery-test.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL ) );
    ofn.hwndOwner = main_window;
    ofn.lpstrFilter = L"Text files\0*.txt\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR;
    SetTimer( main_window, 2, 1200, choose_file );
    r = GetOpenFileNameW( &ofn );
    printf( "      open panel returned %d: %ls\n", r, file );
    check( r && file[1] == ':' && wcsstr( file, L"w2s-gallery-test.txt" ) && ofn.nFileOffset > 0,
           "native open panel returns a Windows path" );

    /* SHBrowseForFolder -> the open panel in folder mode, with the app's callback */
    {
        WCHAR path[MAX_PATH] = L"";
        BOOL ok;

        SetTimer( main_window, 5, 1200, folder_panel );
        ok = browse_for_folder( path );
        printf( "      folder picker returned %d: %ls\n", ok, path );
        check( bff_native_ok, "BFFM_SETSELECTION / BFFM_SETOKTEXT and the title reach the folder panel" );
        check( ok && !_wcsicmp( path, L"Z:\\tmp" ) && bff_selchanged > 0,
               "the chosen folder comes back as a PIDL; browsing raised BFFM_SELCHANGED" );
    }

    /* IFileOpenDialog with FOS_PICKFOLDERS -> the same panel */
    {
        WCHAR path[MAX_PATH] = L"";
        HRESULT hr;

        CoInitialize( NULL );
        SetTimer( main_window, 6, 1200, choose_folder );
        hr = item_dialog_folder( path );
        printf( "      IFileOpenDialog returned %#lx: %ls\n", hr, path );
        check( hr == S_OK && !_wcsicmp( path, L"Z:\\tmp" ), "IFileOpenDialog (FOS_PICKFOLDERS) returns the chosen folder" );
    }

    printf( "%d passed, %d failed\n", passes, failures );
    return failures;
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES };
    WNDCLASSW wc = { 0 };
    MSG msg;
    int ret = 0;

    InitCommonControlsEx( &icc );
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"W2SGallery";
    RegisterClassW( &wc );
    main_window = CreateWindowExW( 0, L"W2SGallery", L"Win32-to-SwiftUI gallery", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1150, 720, NULL, NULL, inst, NULL );
    create_controls();
    ShowWindow( main_window, SW_SHOW );
    UpdateWindow( main_window );

    if (wcsstr( cmdline, L"/selftest" ))
    {
        in_selftest = TRUE;
        ret = selftest();
        if (!wcsstr( cmdline, L"/stay" ))
        {
            DestroyWindow( main_window );
            return ret;
        }
    }
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return ret;
}
