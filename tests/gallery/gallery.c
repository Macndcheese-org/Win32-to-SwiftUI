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
#include <shellapi.h>
#include <shobjidl.h>
#include <winspool.h>
#include <stdio.h>
#include <string.h>

#ifndef DCX_USESTYLE
#define DCX_USESTYLE 0x00010000 /* undocumented; what GetDC uses (class and window style decide the clipping) */
#endif

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
    /* light and dark */
    ID_WHITEPANEL, ID_WHITECHECK,
    /* toolbars, ComboBoxEx, links */
    ID_TOOLBAR, ID_CBEX, ID_LINK, ID_VTRACK,
    ID_LAST
};

static HWND ctl[ID_LAST];
static HWND main_window;
static int got_command[ID_LAST][16];   /* [id][notification code & 15] */
static int got_tb_command[4], got_tb_dropdown;  /* the gallery toolbar's buttons 9101..9104 */
static int got_link_click = -1;                 /* NM_CLICK's link index */
static WCHAR got_link_id[MAX_LINKID_TEXT];
static int got_hscroll, got_tabchange, got_lvchanged, got_dropdown, got_status_click = -1;
static int got_menu_new, got_initmenupopup, status_bar_checked = 1;
static int got_vtrack_pos = -1, got_vtrack_end;
static int got_expanding, got_treesel, got_deltapos, got_vscroll, got_datechange, got_mcselchange, got_mcselect;
static int got_dispinfo, got_check_changed, got_icon_changed, got_icon_activate;
static int got_wiznext, got_wizback;
static int got_syscolorchange, got_themechanged;
static HWND test_wizard;
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
        /* shell32's folder and document icons, as a file browser's tree shows them */
        HMODULE shell32 = LoadLibraryW( L"shell32.dll" );
        HIMAGELIST himl = ImageList_Create( 16, 16, ILC_COLOR32 | ILC_MASK, 2, 0 );
        TVINSERTSTRUCTW ins = { TVI_ROOT, TVI_LAST };

        ImageList_AddIcon( himl, LoadImageW( shell32, MAKEINTRESOURCEW( 4 ), IMAGE_ICON, 16, 16, 0 ) );  /* IDI_SHELL_FOLDER */
        ImageList_AddIcon( himl, LoadImageW( shell32, MAKEINTRESOURCEW( 2 ), IMAGE_ICON, 16, 16, 0 ) );  /* IDI_SHELL_DOCUMENT */
        SendMessageW( ctl[ID_TREE], TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)himl );
        ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        ins.item.pszText = (WCHAR *)L"Fruits";
        tree_fruits = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        /* filled when it is first expanded, as file browsers do */
        ins.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        ins.item.cChildren = 1;
        ins.item.pszText = (WCHAR *)L"Vegetables";
        tree_veg = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        ins.hParent = tree_fruits;
        ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        ins.item.iImage = ins.item.iSelectedImage = 1;
        ins.item.pszText = (WCHAR *)L"Apple";
        tree_apple = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
        ins.item.pszText = (WCHAR *)L"Pear";
        tree_pear = (HTREEITEM)SendMessageW( ctl[ID_TREE], TVM_INSERTITEMW, 0, (LPARAM)&ins );
    }
    /* a toolbar with comctl32's standard images, a check button and a drop-down */
    make( TOOLBARCLASSNAMEW, NULL, TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NOPARENTALIGN | CCS_NORESIZE | CCS_NODIVIDER,
          724, 500, 180, 28, ID_TOOLBAR );
    {
        TBBUTTON b[5];
        memset( b, 0, sizeof(b) );
        SendMessageW( ctl[ID_TOOLBAR], TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0 );
        SendMessageW( ctl[ID_TOOLBAR], TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS );
        SendMessageW( ctl[ID_TOOLBAR], TB_LOADIMAGES, IDB_STD_SMALL_COLOR, (LPARAM)HINST_COMMCTRL );
        b[0].iBitmap = STD_FILENEW; b[0].idCommand = 9101; b[0].fsState = TBSTATE_ENABLED; b[0].fsStyle = BTNS_BUTTON;
        b[1].iBitmap = STD_FILESAVE; b[1].idCommand = 9102; b[1].fsState = TBSTATE_ENABLED; b[1].fsStyle = BTNS_BUTTON;
        b[2].fsStyle = BTNS_SEP;
        b[3].iBitmap = STD_FIND; b[3].idCommand = 9103; b[3].fsState = TBSTATE_ENABLED; b[3].fsStyle = BTNS_CHECK;
        b[4].iBitmap = STD_PRINT; b[4].idCommand = 9104; b[4].fsState = TBSTATE_ENABLED; b[4].fsStyle = BTNS_DROPDOWN;
        SendMessageW( ctl[ID_TOOLBAR], TB_ADDBUTTONSW, 5, (LPARAM)b );
    }
    /* a ComboBoxEx, as wordpad's font list */
    make( WC_COMBOBOXEXW, NULL, CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 916, 500, 180, 150, ID_CBEX );
    {
        static const WCHAR *fonts[] = { L"Arial", L"Courier New", L"Times New Roman" };
        COMBOBOXEXITEMW item;
        int i;
        for (i = 0; i < 3; i++)
        {
            memset( &item, 0, sizeof(item) );
            item.mask = CBEIF_TEXT;
            item.iItem = i;
            item.pszText = (WCHAR *)fonts[i];
            SendMessageW( ctl[ID_CBEX], CBEM_INSERTITEMW, 0, (LPARAM)&item );
        }
        SendMessageW( ctl[ID_CBEX], CB_SETCURSEL, 2, 0 );
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
    make( TRACKBAR_CLASSW, NULL, TBS_VERT | TBS_AUTOTICKS | WS_TABSTOP, 740, 596, 30, 90, ID_VTRACK );
    SendMessageW( ctl[ID_VTRACK], TBM_SETRANGE, TRUE, MAKELPARAM( 0, 10 ) );
    SendMessageW( ctl[ID_VTRACK], TBM_SETPOS, TRUE, 2 );
    {
        /* SysLink only exists in v6 */
        INITCOMMONCONTROLSEX link = { sizeof(link), ICC_LINK_CLASS };
        ULONG_PTR cookie;

        if (v6_begin( &cookie ))
        {
            InitCommonControlsEx( &link );
            make( WC_LINK, L"Read the <a href=\"https://www.winehq.org\" id=\"site\">Wine site</a> or the <a id=\"help\">help</a>.",
                  WS_TABSTOP, 408, 600, 300, 20, ID_LINK );
            v6_end( cookie );
        }
    }
    {
        HWND panel = make( L"W2SWhitePanel", NULL, 0, 16, 580, 376, 56, ID_WHITEPANEL );
        ctl[ID_WHITECHECK] = CreateWindowExW( 0, L"Button", L"A check box on a page the app paints white",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 12, 18, 340, 20,
                                              panel, (HMENU)ID_WHITECHECK, GetModuleHandleW( NULL ), NULL );
        SendMessageW( ctl[ID_WHITECHECK], WM_SETFONT, (WPARAM)GetStockObject( DEFAULT_GUI_FONT ), TRUE );
    }
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

/* ---------- property sheets ---------- */

/* an in-memory page template: a static text naming the page */
static DLGTEMPLATE *page_template( const WCHAR *text )
{
    static const WCHAR font[] = L"MS Shell Dlg";
    BYTE *buf = calloc( 1, 1024 ), *p;
    DLGTEMPLATE *tmpl = (DLGTEMPLATE *)buf;
    DLGITEMTEMPLATE *item;

    tmpl->style = DS_SETFONT | WS_CHILD | WS_DISABLED | WS_CAPTION;
    tmpl->cdit = 1;
    tmpl->cx = 220;
    tmpl->cy = 120;
    p = (BYTE *)(tmpl + 1);
    p += 2 * sizeof(WORD);                               /* no menu, default class */
    p += sizeof(WCHAR);                                  /* no title */
    *(WORD *)p = 8; p += sizeof(WORD);                   /* font size */
    memcpy( p, font, sizeof(font) ); p += sizeof(font);
    p = (BYTE *)(((UINT_PTR)p + 3) & ~(UINT_PTR)3);
    item = (DLGITEMTEMPLATE *)p;
    item->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    item->x = 7; item->y = 7; item->cx = 200; item->cy = 12;
    item->id = 1000;
    p = (BYTE *)(item + 1);
    *(WORD *)p = 0xffff; p += sizeof(WORD);
    *(WORD *)p = 0x0082; p += sizeof(WORD);              /* Static */
    memcpy( p, text, (wcslen( text ) + 1) * sizeof(WCHAR) );
    return tmpl;
}

static INT_PTR CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    return msg == WM_INITDIALOG;
}

static const WCHAR *page_titles[] = { L"General", L"Appearance", L"Desktop & Dock", L"Displays", L"Sound",
                                      L"Keyboard shortcuts", L"Accessibility" };

static INT_PTR sheet_result;     /* what PropertySheet returned */

/* PSH_MODELESS: returns the sheet; otherwise runs it and returns NULL */
static HWND property_sheet( int pages, BOOL modeless )
{
    HPROPSHEETPAGE hpages[8];
    DLGTEMPLATE *tmpl[8];
    PROPSHEETHEADERW psh = { sizeof(psh) };
    HWND sheet;
    int i;

    for (i = 0; i < pages; i++)
    {
        PROPSHEETPAGEW psp = { sizeof(psp) };
        WCHAR text[64];
        swprintf( text, 64, L"This is the %ls page.", page_titles[i] );
        tmpl[i] = page_template( text );
        psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE;
        psp.pResource = tmpl[i];
        psp.pszTitle = page_titles[i];
        psp.pfnDlgProc = page_proc;
        hpages[i] = CreatePropertySheetPageW( &psp );
    }
    psh.dwFlags = modeless ? PSH_MODELESS : 0;
    psh.hwndParent = main_window;
    psh.pszCaption = L"Gallery settings";
    psh.nPages = pages;
    psh.phpage = hpages;
    sheet = (HWND)PropertySheetW( &psh );
    sheet_result = (INT_PTR)sheet;
    /* a page is created when first shown: a modeless sheet's templates must
     * outlive it (they're small; the test leaks them) */
    if (!modeless) for (i = 0; i < pages; i++) free( tmpl[i] );
    return modeless ? sheet : NULL;
}

/* a Wizard97 wizard: a welcome page without a header, then two with one */
static const WCHAR *wizard_titles[] = { L"Introduction", L"License", L"Install" };
static const WCHAR *wizard_headers[] = { NULL, L"License agreement", L"Ready to install" };
static const WCHAR *wizard_subheaders[] = { NULL, L"Please read the terms.", L"Gallery will be installed." };

static INT_PTR CALLBACK wizard_page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    static const DWORD buttons[] = { PSWIZB_NEXT, PSWIZB_BACK | PSWIZB_NEXT, PSWIZB_BACK | PSWIZB_FINISH };

    if (msg == WM_INITDIALOG)
    {
        SetWindowLongPtrW( hwnd, DWLP_USER, ((PROPSHEETPAGEW *)lparam)->lParam );
        return TRUE;
    }
    if (msg == WM_NOTIFY)
    {
        NMHDR *hdr = (NMHDR *)lparam;
        int index = (int)GetWindowLongPtrW( hwnd, DWLP_USER );

        switch (hdr->code)
        {
        case PSN_SETACTIVE:
            SendMessageW( GetParent( hwnd ), PSM_SETWIZBUTTONS, 0, buttons[index] );
            break;
        case PSN_WIZNEXT:
            got_wiznext++;
            break;
        case PSN_WIZBACK:
            got_wizback++;
            break;
        }
        SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, 0 );
        return TRUE;
    }
    return FALSE;
}

/* what a native wizard's Back or Next should display: wine's text without its
 * arrow, or the macOS word when wine's is the English one */
static const char *wizard_word( HWND button, const char *english, const char *mac )
{
    static char needle[2][160];
    static int n;
    WCHAR text[64], *p = text, *end;
    char *out = needle[n++ % 2], word[128];

    GetWindowTextW( button, text, ARRAYSIZE(text) );
    while (*p == '<' || *p == ' ') p++;
    end = p + wcslen( p );
    while (end > p && (end[-1] == '>' || end[-1] == ' ')) *--end = 0;
    WideCharToMultiByte( CP_UTF8, 0, p, -1, word, sizeof(word), NULL, NULL );
    snprintf( out, sizeof(needle[0]), "\"display\":\"%s\"",
              !strcmp( word[0] == '&' ? word + 1 : word, english ) ? mac : word );
    return out;
}

/* PSH_MODELESS: returns the wizard; otherwise runs it and returns NULL */
static HWND wizard_sheet( BOOL modeless )
{
    HPROPSHEETPAGE hpages[3];
    DLGTEMPLATE *tmpl[3];
    PROPSHEETHEADERW psh = { sizeof(psh) };
    HWND sheet;
    int i;

    for (i = 0; i < 3; i++)
    {
        PROPSHEETPAGEW psp = { sizeof(psp) };
        WCHAR text[64];
        swprintf( text, 64, L"This is the %ls step.", wizard_titles[i] );
        tmpl[i] = page_template( text );
        psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE;
        if (wizard_headers[i]) psp.dwFlags |= PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
        else psp.dwFlags |= PSP_HIDEHEADER;
        psp.pResource = tmpl[i];
        psp.pszTitle = wizard_titles[i];
        psp.pszHeaderTitle = wizard_headers[i];
        psp.pszHeaderSubTitle = wizard_subheaders[i];
        psp.pfnDlgProc = wizard_page_proc;
        psp.lParam = i;
        hpages[i] = CreatePropertySheetPageW( &psp );
    }
    psh.dwFlags = PSH_WIZARD97 | PSH_HEADER | (modeless ? PSH_MODELESS : 0);
    psh.hwndParent = main_window;
    psh.pszCaption = L"Gallery installer";
    psh.nPages = 3;
    psh.phpage = hpages;
    sheet = (HWND)PropertySheetW( &psh );
    /* as in property_sheet(): a modeless wizard creates its pages later */
    if (!modeless) for (i = 0; i < 3; i++) free( tmpl[i] );
    return modeless ? sheet : NULL;
}

/* ---------- colour, font and print ---------- */

static COLORREF custom_colors[16];
static int color_runs, font_runs, print_runs;
static BOOL color_ok, font_ok, print_ok;
static CHOOSECOLORW last_color;
static CHOOSEFONTW last_cf;
static LOGFONTW last_font;
static PRINTDLGW last_print;

static void choose_color(void)
{
    CHOOSECOLORW cc = { sizeof(cc) };
    WCHAR text[64];

    cc.hwndOwner = main_window;
    cc.rgbResult = RGB( 10, 20, 30 );
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    color_ok = ChooseColorW( &cc );
    last_color = cc;
    color_runs++;
    swprintf( text, 64, L"Colour: %d, #%06lx", color_ok, cc.rgbResult );
    SetWindowTextW( ctl[ID_LABEL], text );
}

static void choose_font( DWORD extra_flags )
{
    CHOOSEFONTW cf = { sizeof(cf) };
    LOGFONTW lf = { 0 };
    HDC hdc = GetDC( NULL );
    WCHAR text[96];

    lf.lfHeight = -MulDiv( 12, GetDeviceCaps( hdc, LOGPIXELSY ), 72 );
    ReleaseDC( NULL, hdc );
    lf.lfWeight = FW_NORMAL;
    wcscpy( lf.lfFaceName, L"Arial" );
    cf.hwndOwner = main_window;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT | extra_flags;
    font_ok = ChooseFontW( &cf );
    last_cf = cf;
    last_font = lf;
    font_runs++;
    swprintf( text, 96, L"Font: %d, %ls %d.%d pt", font_ok, lf.lfFaceName, cf.iPointSize / 10, cf.iPointSize % 10 );
    SetWindowTextW( ctl[ID_LABEL], text );
}

static void print_dialog(void)
{
    PRINTDLGW pd = { sizeof(pd) };
    WCHAR text[160];

    pd.hwndOwner = main_window;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION;
    pd.nMinPage = 1;
    pd.nMaxPage = 9;
    pd.nFromPage = 1;
    pd.nToPage = 9;
    pd.nCopies = 1;
    print_ok = PrintDlgW( &pd );
    last_print = pd;
    print_runs++;
    if (print_ok && pd.hDevNames)
    {
        DEVNAMES *dn = GlobalLock( pd.hDevNames );
        swprintf( text, 160, L"Print: %ls, %d copies, pages %d-%d", (WCHAR *)dn + dn->wDeviceOffset,
                  pd.nCopies, pd.nFromPage, pd.nToPage );
        GlobalUnlock( pd.hDevNames );
    }
    else swprintf( text, 160, L"Print: %d (%#lx)", print_ok, CommDlgExtendedError() );
    SetWindowTextW( ctl[ID_LABEL], text );
    if (!in_selftest)
    {
        if (pd.hDC) DeleteDC( pd.hDC );
        if (pd.hDevMode) GlobalFree( pd.hDevMode );
        if (pd.hDevNames) GlobalFree( pd.hDevNames );
    }
}

/* a page the app paints white itself, whatever the system colours say */
static LRESULT CALLBACK white_panel_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect( hwnd, &rc );
        FillRect( (HDC)wparam, &rc, GetStockObject( WHITE_BRUSH ) );
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor( (HDC)wparam, RGB( 0, 0, 0 ) );
        SetBkColor( (HDC)wparam, RGB( 255, 255, 255 ) );
        return (LRESULT)GetStockObject( WHITE_BRUSH );
    case WM_COMMAND:
        return SendMessageW( main_window, msg, wparam, lparam );    /* counted there */
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* FINDMSGSTRING from FindText / ReplaceText */
static UINT find_msg;
static int got_find_next, got_find_replace, got_find_all, got_find_term;
static DWORD got_find_flags;

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (find_msg && msg == find_msg)
    {
        FINDREPLACEW *fr = (FINDREPLACEW *)lparam;
        got_find_flags = fr->Flags;
        if (fr->Flags & FR_DIALOGTERM) got_find_term++;
        else if (fr->Flags & FR_FINDNEXT) got_find_next++;
        else if (fr->Flags & FR_REPLACE) got_find_replace++;
        else if (fr->Flags & FR_REPLACEALL) got_find_all++;
        return 0;
    }
    switch (msg)
    {
    case WM_COMMAND:
    {
        int id = LOWORD( wparam ), code = HIWORD( wparam );
        if (id >= ID_PUSH && id < ID_LAST && lparam) got_command[id][code & 15]++;
        if (id >= 9101 && id <= 9104 && (HWND)lparam == ctl[ID_TOOLBAR]) got_tb_command[id - 9101]++;
        if (!lparam && id == 9001) got_menu_new++;
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
        if (code == BN_CLICKED && id == ID_PROPSHEET && !in_selftest) property_sheet( 7, FALSE );
        /* the self-test opens it modeless, to drive it */
        if (code == BN_CLICKED && id == ID_WIZARD) test_wizard = wizard_sheet( in_selftest );
        if (code == BN_CLICKED && id == ID_COLOR) choose_color();
        /* the self-test checks that a proportional font can't be chosen then */
        if (code == BN_CLICKED && id == ID_FONT) choose_font( in_selftest ? CF_FIXEDPITCHONLY : 0 );
        if (code == BN_CLICKED && id == ID_PRINT) print_dialog();
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
    case WM_SYSCOLORCHANGE:
        got_syscolorchange++;
        return 0;
    case WM_THEMECHANGED:
        got_themechanged++;
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
        if ((HWND)lparam == ctl[ID_VTRACK])
        {
            if (LOWORD( wparam ) == TB_THUMBPOSITION) got_vtrack_pos = HIWORD( wparam );
            if (LOWORD( wparam ) == TB_ENDTRACK) got_vtrack_end++;
        }
        return 0;
    case WM_INITMENUPOPUP:
        /* apps update their menus here; the native menu must show it */
        got_initmenupopup++;
        if (GetMenuState( (HMENU)wparam, 9010, MF_BYCOMMAND ) != (UINT)-1)
        {
            status_bar_checked = !status_bar_checked;
            CheckMenuItem( (HMENU)wparam, 9010, MF_BYCOMMAND | (status_bar_checked ? MF_CHECKED : MF_UNCHECKED) );
        }
        return 0;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lparam;
        if (hdr->hwndFrom == ctl[ID_TAB] && hdr->code == TCN_SELCHANGE) got_tabchange++;
        if (hdr->hwndFrom == ctl[ID_LINK] && hdr->code == NM_CLICK)
        {
            got_link_click = ((NMLINK *)hdr)->item.iLink;
            lstrcpyW( got_link_id, ((NMLINK *)hdr)->item.szID );
        }
        if (hdr->hwndFrom == ctl[ID_TOOLBAR] && hdr->code == TBN_DROPDOWN)
        {
            got_tb_dropdown++;
            return TBDDRET_DEFAULT;
        }
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

/* whether the native model's object holding "id":<id> (a toolbar button) has
 * field, whatever the order JSON gives its keys */
static BOOL query_object_has( HWND hwnd, int id, const char *field )
{
    char *q = pQuery( hwnd ), key[32], *p, *start, *end;
    BOOL ok = FALSE;

    snprintf( key, sizeof(key), "\"id\":%d", id );
    if (q && (p = strstr( q, key )))
    {
        for (start = p; start > q && *start != '{'; start--) ;
        if ((end = strchr( p, '}' )))
        {
            char saved = end[1];
            end[1] = 0;
            ok = strstr( start, field ) != NULL;
            end[1] = saved;
        }
    }
    if (!ok) printf( "      query: %s\n", q ? q : "(null)" );
    pFree( q );
    return ok;
}

/* a number field of the native model, or -1 */
static int query_int( HWND hwnd, const char *key )
{
    char *q = pQuery( hwnd ), needle[64], *p;
    int ret = -1;

    snprintf( needle, sizeof(needle), "\"%s\":", key );
    if (q && (p = strstr( q, needle ))) ret = atoi( p + strlen( needle ) );
    if (ret == -1) printf( "      query (%s): %s\n", key, q ? q : "(null)" );
    pFree( q );
    return ret;
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
    if (!r || !strstr( r, "ok" ) || strstr( event, "realClick" )) printf( "      inject %s: %s\n", event, r ? r : "(null)" );
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

static void CALLBACK choose_popup( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"popupChoose\",\"v\":9004}" );
}

static void CALLBACK choose_file( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"choose\",\"s\":\"/tmp/w2s-gallery-test.txt\"}" );
}

static void pump_until( const int *counter, int target, int ms )
{
    DWORD end = GetTickCount() + ms;
    while (*counter < target && (int)(end - GetTickCount()) > 0) pump( 50 );
}

/* a modal property sheet's own buttons, clicked natively (winecfg's OK and Cancel) */
static const char *modal_button_event;
static int modal_button_id, modal_button_stuck;

static void CALLBACK modal_button_click( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    HWND sheet = GetActiveWindow();
    KillTimer( NULL, id );
    if (sheet) inject( GetDlgItem( sheet, modal_button_id ), modal_button_event );
}

static void CALLBACK modal_button_watchdog( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    HWND sheet = GetActiveWindow();
    KillTimer( NULL, id );
    if (!sheet) return;
    modal_button_stuck++;
    PostMessageW( sheet, PSM_PRESSBUTTON, PSBTN_CANCEL, 0 );
}

static void CALLBACK panel_cancel( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    inject( NULL, "{\"t\":\"cancel\"}" );
}

static BOOL color_native_ok, font_native_ok, print_native_ok;

static void CALLBACK color_panel_ok( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    color_native_ok = query_has( NULL, "\"color\":[10,20,30]" ) && query_has( NULL, "\"custom\":[1,2,3," ) &&
                      query_has( NULL, "\"alpha\":false" );
    inject( NULL, "{\"t\":\"custom\",\"a\":[1,200,0,0]}" );
    inject( NULL, "{\"t\":\"pick\",\"a\":[200,100,50]}" );
    inject( NULL, "{\"t\":\"ok\"}" );
}

static void CALLBACK font_panel_ok( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    /* CF_FIXEDPITCHONLY: Arial can't be taken, Menlo can */
    font_native_ok = query_has( NULL, "\"family\":\"Arial\"" ) && query_has( NULL, "\"size\":12" ) &&
                     query_has( NULL, "\"valid\":false" ) && query_has( NULL, "\"effects\":true" );
    inject( NULL, "{\"t\":\"font\",\"s\":\"Menlo\",\"v\":14}" );
    font_native_ok = font_native_ok && query_has( NULL, "\"valid\":true" );
    inject( NULL, "{\"t\":\"underline\",\"v\":1}" );
    inject( NULL, "{\"t\":\"pick\",\"a\":[0,0,255]}" );
    inject( NULL, "{\"t\":\"ok\"}" );
}

static void CALLBACK print_panel_ok( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    print_native_ok = query_has( NULL, "\"open\":true" ) && query_has( NULL, "\"copies\":1" );
    inject( NULL, "{\"t\":\"print\",\"copies\":2,\"from\":2,\"to\":3}" );
}

static int CALLBACK font_exists_proc( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lparam )
{
    *(BOOL *)lparam = TRUE;
    return 0;
}

static BOOL font_exists( const WCHAR *face )
{
    LOGFONTW lf = { 0 };
    HDC hdc = GetDC( NULL );
    BOOL found = FALSE;

    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy( lf.lfFaceName, face );
    EnumFontFamiliesExW( hdc, &lf, font_exists_proc, (LPARAM)&found, 0 );
    ReleaseDC( NULL, hdc );
    return found;
}

static BOOL near_rgb( COLORREF a, COLORREF b )
{
    return abs( GetRValue( a ) - GetRValue( b ) ) <= 1 && abs( GetGValue( a ) - GetGValue( b ) ) <= 1 &&
           abs( GetBValue( a ) - GetBValue( b ) ) <= 1;
}

static BOOL is_dark( COLORREF c )
{
    return 0.2126 * GetRValue( c ) + 0.7152 * GetGValue( c ) + 0.0722 * GetBValue( c ) < 0.5 * 255;
}

/* Light and Dark mode: wine's colours from NSColor, each control light or dark as what's behind it */
static void selftest_look(void)
{
    char want[96];
    COLORREF face;
    int sys_before, theme_before;

    pump( 300 );
    face = GetSysColor( COLOR_BTNFACE );
    snprintf( want, sizeof(want), "\"btnFace\":%lu", face );
    check( query_has( ctl[ID_PUSH], want ), "wine's COLOR_BTNFACE is the native table's (NSColor.windowBackgroundColor)" );
    snprintf( want, sizeof(want), "\"appearance\":\"%s\"", is_dark( face ) ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua" );
    check( query_has( ctl[ID_PUSH], want ), "a control on wine's dialog background is as light or dark as it" );
    check( query_has( ctl[ID_WHITECHECK], "\"backdrop\":16777215" ) &&
           query_has( ctl[ID_WHITECHECK], "\"appearance\":\"NSAppearanceNameAqua\"" ),
           "a control on a page the app paints white stays light" );

    /* as if macOS switched to Dark Mode, then back: from Light Mode, whatever the
     * Mac shows now (an automatic appearance is dark at night) */
    inject( ctl[ID_PUSH], "{\"t\":\"lookOverride\",\"s\":\"light\"}" );
    pump( 800 );
    sys_before = got_syscolorchange;
    theme_before = got_themechanged;
    inject( ctl[ID_PUSH], "{\"t\":\"lookOverride\",\"s\":\"dark\"}" );
    pump( 800 );
    printf( "      dark: COLOR_BTNFACE %06lx, COLOR_WINDOW %06lx, COLOR_WINDOWTEXT %06lx, COLOR_HIGHLIGHT %06lx;"
            " WM_SYSCOLORCHANGE %d -> %d, WM_THEMECHANGED %d -> %d\n",
            GetSysColor( COLOR_BTNFACE ), GetSysColor( COLOR_WINDOW ), GetSysColor( COLOR_WINDOWTEXT ),
            GetSysColor( COLOR_HIGHLIGHT ), sys_before, got_syscolorchange, theme_before, got_themechanged );
    check( got_syscolorchange > sys_before && got_themechanged > theme_before && is_dark( GetSysColor( COLOR_BTNFACE ) ) &&
           is_dark( GetSysColor( COLOR_WINDOW ) ) && !is_dark( GetSysColor( COLOR_WINDOWTEXT ) ),
           "Dark Mode -> dark system colours, WM_SYSCOLORCHANGE and WM_THEMECHANGED" );
    check( query_has( ctl[ID_PUSH], "\"appearance\":\"NSAppearanceNameDarkAqua\"" ) &&
           query_has( ctl[ID_WHITECHECK], "\"appearance\":\"NSAppearanceNameAqua\"" ),
           "then controls on the dialog go dark, the one on the white page stays light" );
    inject( ctl[ID_PUSH], "{\"t\":\"lookOverride\",\"s\":\"light\"}" );
    pump( 800 );
    check( !is_dark( GetSysColor( COLOR_BTNFACE ) ) && is_dark( GetSysColor( COLOR_WINDOWTEXT ) ) &&
           query_has( ctl[ID_PUSH], "\"appearance\":\"NSAppearanceNameAqua\"" ),
           "Light Mode -> light colours and light controls again" );
    inject( ctl[ID_PUSH], "{\"t\":\"lookOverride\",\"s\":\"\"}" );
    pump( 500 );

    /* the white page's check box, both ways */
    inject( ctl[ID_WHITECHECK], "{\"t\":\"click\"}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_WHITECHECK], BM_GETCHECK, 0, 0 ) == BST_CHECKED &&
           got_command[ID_WHITECHECK][BN_CLICKED] == 1, "native click on the white page's check box -> BN_CLICKED" );
    SendMessageW( ctl[ID_WHITECHECK], BM_SETCHECK, BST_UNCHECKED, 0 );
    pump( 200 );
    check( query_has( ctl[ID_WHITECHECK], "\"checked\":0" ), "BM_SETCHECK -> the white page's native check box" );
}

/* FindText and ReplaceText as AppKit's Find panel */
static void selftest_find(void)
{
    static WCHAR what[64], with[64];
    static FINDREPLACEW fr;     /* the app's, alive while the dialog is */
    HWND dlg;
    char *q;

    find_msg = RegisterWindowMessageW( FINDMSGSTRINGW );
    wcscpy( what, L"needle" );
    memset( &fr, 0, sizeof(fr) );
    fr.lStructSize = sizeof(fr);
    fr.hwndOwner = main_window;
    fr.lpstrFindWhat = what;
    fr.wFindWhatLen = ARRAYSIZE(what);
    fr.Flags = FR_DOWN | FR_MATCHCASE;
    dlg = FindTextW( &fr );
    pump( 400 );
    check( dlg && IsWindow( dlg ) && !IsWindowVisible( dlg ) && query_has( NULL, "\"visible\":true" ) &&
           query_has( NULL, "\"find\":\"needle\"" ) && query_has( NULL, "\"matchCase\":true" ) &&
           query_has( NULL, "\"replaceEnabled\":false" ) && query_has( NULL, "\"previousEnabled\":true" ),
           "FindText -> AppKit's Find panel with the app's text and options, and a window standing for the dialog" );
    inject( NULL, "{\"t\":\"press\",\"v\":3,\"find\":\"hay\",\"matchCase\":false,\"wholeWord\":true}" );
    pump( 300 );
    check( got_find_next == 1 && !wcscmp( what, L"hay" ) && !(got_find_flags & (FR_DOWN | FR_MATCHCASE)) &&
           (got_find_flags & FR_WHOLEWORD),
           "Previous in the Find panel -> FINDMSGSTRING: FR_FINDNEXT upward, the text and options" );
    ShowWindow( dlg, SW_SHOW );
    check( !IsWindowVisible( dlg ), "showing the dialog window brings the panel forward, the window stays hidden" );
    inject( NULL, "{\"t\":\"close\"}" );
    pump( 300 );
    check( got_find_term == 1 && !IsWindow( dlg ), "closing the Find panel -> FR_DIALOGTERM, the dialog window goes" );

    wcscpy( with, L"pin" );
    fr.lpstrReplaceWith = with;
    fr.wReplaceWithLen = ARRAYSIZE(with);
    fr.Flags = 0;
    dlg = ReplaceTextW( &fr );
    pump( 400 );
    check( dlg && query_has( NULL, "\"replaceEnabled\":true" ) && query_has( NULL, "\"replace\":\"pin\"" ) &&
           query_has( NULL, "\"previousEnabled\":false" ),
           "ReplaceText -> the Find panel with Replace (and no direction, as the dialog)" );
    inject( NULL, "{\"t\":\"press\",\"v\":4,\"find\":\"a\",\"replace\":\"b\"}" );
    pump( 300 );
    check( got_find_all == 1 && (got_find_flags & FR_DOWN) && !wcscmp( what, L"a" ) && !wcscmp( with, L"b" ),
           "Replace All -> FR_REPLACEALL with both strings" );
    inject( NULL, "{\"t\":\"press\",\"v\":6}" );
    pump( 300 );
    check( got_find_replace == 1, "Replace & Find -> FR_REPLACE" );
    DestroyWindow( dlg );
    pump( 300 );
    q = pQuery( NULL );
    check( got_find_term == 1 && q && strstr( q, "no open request" ),
           "the app destroying its dialog closes the panel, without FR_DIALOGTERM" );
    pFree( q );

    /* an ANSI app: its strings in its code page, both ways */
    {
        static char what_a[64] = "caf\xe9";      /* café in 1252 */
        static FINDREPLACEA fra;

        if (GetACP() != 1252) printf( "      code page %u: the ANSI text checks only see the calls\n", GetACP() );

        memset( &fra, 0, sizeof(fra) );
        fra.lStructSize = sizeof(fra);
        fra.hwndOwner = main_window;
        fra.lpstrFindWhat = what_a;
        fra.wFindWhatLen = sizeof(what_a);
        fra.Flags = FR_DOWN;
        dlg = FindTextA( &fra );
        pump( 400 );
        check( dlg && (GetACP() != 1252 || query_has( NULL, "\"find\":\"caf\u00e9\"" ) ||
                       query_has( NULL, "\"find\":\"caf\xc3\xa9\"" )),
               "FindTextA -> the Find panel with the app's ANSI text" );
        inject( NULL, "{\"t\":\"press\",\"v\":2,\"find\":\"na\u00efve\"}" );
        pump( 300 );
        check( got_find_next == 2 && (fra.Flags & FR_DOWN) &&
               (GetACP() != 1252 || !strcmp( what_a, "na\xefve" )),
               "Next from an ANSI app's panel -> FR_FINDNEXT and the text in its code page" );
        DestroyWindow( dlg );
        pump( 300 );
    }
}

/* PageSetupDlg as the macOS Page Setup sheet */
static BOOL pagesetup_native_ok;

static void CALLBACK pagesetup_landscape( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    pagesetup_native_ok = query_has( NULL, "\"landscape\":false" );
    inject( NULL, "{\"t\":\"pagesetup\",\"landscape\":true}" );
}

static void selftest_pagesetup(void)
{
    PAGESETUPDLGW psd = { sizeof(psd) };
    DWORD needed = 0, count = 0;
    DEVMODEW *dm;
    BOOL ret, landscape = FALSE;

    EnumPrintersW( PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &count );
    if (!needed)
    {
        printf( "      no printer in this wine prefix: the page setup checks are skipped\n" );
        return;
    }
    psd.hwndOwner = main_window;
    psd.Flags = PSD_INHUNDREDTHSOFMILLIMETERS;
    SetTimer( main_window, 32, 1200, pagesetup_landscape );
    ret = PageSetupDlgW( &psd );
    if (psd.hDevMode && (dm = GlobalLock( psd.hDevMode )))
    {
        landscape = (dm->dmFields & DM_ORIENTATION) && dm->dmOrientation == DMORIENT_LANDSCAPE;
        GlobalUnlock( psd.hDevMode );
    }
    printf( "      page setup returned %d: paper %ldx%ld, margins %ld\n", ret, psd.ptPaperSize.x, psd.ptPaperSize.y,
            psd.rtMargin.left );
    check( ret && pagesetup_native_ok && landscape && psd.hDevNames && psd.ptPaperSize.x > psd.ptPaperSize.y &&
           psd.ptPaperSize.y > 10000 && psd.rtMargin.left == 2540,
           "PageSetupDlg -> the macOS Page Setup sheet: landscape into hDevMode, ptPaperSize across in 1/100 mm, "
           "1 inch margins when the app gives none" );
    SetTimer( main_window, 32, 1200, panel_cancel );
    ret = PageSetupDlgW( &psd );
    check( !ret, "Cancel on the Page Setup sheet -> FALSE" );
    if (psd.hDevMode) GlobalFree( psd.hDevMode );
    if (psd.hDevNames) GlobalFree( psd.hDevNames );
}

/* ITaskbarList3 progress on the Dock icon */
static BOOL dock_has( const char *needle )
{
    char *r = pInject( NULL, "{\"t\":\"dock\"}" );
    BOOL ok = r && strstr( r, needle );
    if (!ok) printf( "      dock: %s\n", r ? r : "(null)" );
    pFree( r );
    return ok;
}

/* FlashWindowEx: a Dock bounce (winemac). Nothing to read back: this only goes
 * through the calls (WINEDEBUG=trace+macdrv shows them arrive) */
static void selftest_flash(void)
{
    FLASHWINFO info = { sizeof(info), main_window, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };

    FlashWindowEx( &info );
    info.dwFlags = FLASHW_STOP;
    FlashWindowEx( &info );
    FlashWindow( main_window, TRUE );
    printf( "      FlashWindowEx / FlashWindow called (a Dock bounce, not readable here)\n" );
}

static void selftest_taskbar(void)
{
    ITaskbarList3 *list;

    CoInitialize( NULL );
    if (FAILED( CoCreateInstance( &CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&list ) ))
    {
        check( FALSE, "ITaskbarList3 can be made" );
        return;
    }
    ITaskbarList3_HrInit( list );
    ITaskbarList3_SetProgressValue( list, main_window, 30, 100 );
    pump( 200 );
    check( dock_has( "\"barPercent\":30" ) && dock_has( "\"shown\":true" ) && dock_has( "\"hasIcon\":true" ),
           "SetProgressValue -> a progress bar on the Dock icon" );
    ITaskbarList3_SetProgressState( list, main_window, TBPF_INDETERMINATE );
    pump( 200 );
    check( dock_has( "\"indeterminate\":true" ), "TBPF_INDETERMINATE -> an indeterminate bar on the Dock icon" );
    ITaskbarList3_SetProgressValue( list, main_window, 3, 4 );
    pump( 200 );
    check( dock_has( "\"indeterminate\":false" ) && dock_has( "\"barPercent\":75" ),
           "a value after TBPF_INDETERMINATE -> a normal bar again, as on Windows" );
    ITaskbarList3_SetProgressState( list, main_window, TBPF_NOPROGRESS );
    pump( 200 );
    check( dock_has( "\"shown\":false" ), "TBPF_NOPROGRESS -> the Dock icon as it was" );
    ITaskbarList3_Release( list );
}

/* ShellAbout as the standard About panel */
static BOOL about_native_ok;

static void CALLBACK about_close( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    about_native_ok = query_has( NULL, "\"visible\":true" ) && query_has( NULL, "\"Gallery\"" ) &&
                      query_has( NULL, "Win32-to-SwiftUI gallery\\nEvery control the runtime translates." ) &&
                      query_has( NULL, "1.2.3" ) && query_has( NULL, "\"Copyright 2026 MacNdCheese\"" ) &&
                      query_has( NULL, "\"icon\":\"uttype:com.apple.application-bundle\"" );
    inject( NULL, "{\"t\":\"close\"}" );
}

static void selftest_about(void)
{
    BOOL ret;

    SetTimer( main_window, 30, 1000, about_close );
    ret = ShellAboutW( main_window, L"Gallery#Win32-to-SwiftUI gallery", L"Every control the runtime translates.", NULL );
    check( ret && about_native_ok,
           "ShellAbout -> the About panel: the program's name, version and copyright, the caller's text, "
           "the generic app icon for a program without one; it returns once closed" );
}

/* ChooseColor, ChooseFont and PrintDlg as the macOS panels */
static void selftest_pickers(void)
{
    DWORD needed = 0, count = 0;
    HDC hdc;
    int dpi;

    custom_colors[0] = RGB( 1, 2, 3 );
    SetTimer( main_window, 7, 1000, color_panel_ok );
    inject( ctl[ID_COLOR], "{\"t\":\"click\"}" );
    pump_until( &color_runs, 1, 8000 );
    printf( "      colour panel returned %d: #%06lx\n", color_ok, last_color.rgbResult );
    check( got_command[ID_COLOR][BN_CLICKED] == 1 && color_native_ok,
           "native click on Colour… -> BN_CLICKED; the colour panel starts at the app's colour and custom colours" );
    check( color_ok && near_rgb( last_color.rgbResult, RGB( 200, 100, 50 ) ) && custom_colors[1] == RGB( 200, 0, 0 ),
           "the colour panel's OK -> rgbResult and the edited custom colours" );
    SetTimer( main_window, 7, 1000, panel_cancel );
    choose_color();
    check( color_runs == 2 && !color_ok, "Cancel on the colour panel -> FALSE" );

    SetTimer( main_window, 8, 1000, font_panel_ok );
    inject( ctl[ID_FONT], "{\"t\":\"click\"}" );
    pump_until( &font_runs, 1, 8000 );
    hdc = GetDC( NULL );
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    ReleaseDC( NULL, hdc );
    printf( "      font panel returned %d: %ls, %d, height %ld\n", font_ok, last_font.lfFaceName, last_cf.iPointSize,
            last_font.lfHeight );
    check( got_command[ID_FONT][BN_CLICKED] == 1 && font_native_ok,
           "native click on Font… -> BN_CLICKED; the font panel starts at the app's font, CF_FIXEDPITCHONLY holds OK back" );
    check( font_ok && last_cf.iPointSize == 140 && last_font.lfHeight == -MulDiv( 14, dpi, 72 ) && last_font.lfUnderline &&
           last_cf.rgbColors == RGB( 0, 0, 255 ) &&
           (font_exists( L"Menlo" ) ? !wcscmp( last_font.lfFaceName, L"Menlo" ) : last_font.lfFaceName[0] != 0),
           "the font panel's OK -> a LOGFONT GDI can make, iPointSize, underline and colour" );

    EnumPrintersW( PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &count );
    if (!needed)
    {
        printf( "      no printer in this wine prefix: the print panel checks are skipped\n" );
        return;
    }
    SetTimer( main_window, 9, 1200, print_panel_ok );
    inject( ctl[ID_PRINT], "{\"t\":\"click\"}" );
    pump_until( &print_runs, 1, 10000 );
    check( got_command[ID_PRINT][BN_CLICKED] == 1 && print_native_ok,
           "native click on Print… -> BN_CLICKED; the print panel opens as a sheet with the app's copies" );
    {
        BOOL devices = FALSE;
        if (print_ok && last_print.hDevNames && last_print.hDevMode)
        {
            DEVNAMES *dn = GlobalLock( last_print.hDevNames );
            DEVMODEW *dm = GlobalLock( last_print.hDevMode );
            HANDLE printer;
            if (OpenPrinterW( (WCHAR *)dn + dn->wDeviceOffset, &printer, NULL ))
            {
                devices = !wcsncmp( dm->dmDeviceName, (WCHAR *)dn + dn->wDeviceOffset, CCHDEVICENAME - 1 );
                ClosePrinter( printer );
            }
            printf( "      print panel: %ls on %ls, %d copies, pages %d-%d\n", (WCHAR *)dn + dn->wDeviceOffset,
                    (WCHAR *)dn + dn->wOutputOffset, last_print.nCopies, last_print.nFromPage, last_print.nToPage );
            GlobalUnlock( last_print.hDevMode );
            GlobalUnlock( last_print.hDevNames );
        }
        check( print_ok && devices && (last_print.Flags & PD_PAGENUMS) && last_print.nFromPage == 2 &&
               last_print.nToPage == 3 && last_print.nCopies == 2 && last_print.hDC,
               "Print -> the page range, copies, a wine printer's DEVMODE/DEVNAMES and its DC" );
        if (last_print.hDC) DeleteDC( last_print.hDC );
        if (last_print.hDevMode) GlobalFree( last_print.hDevMode );
        if (last_print.hDevNames) GlobalFree( last_print.hDevNames );
    }
    SetTimer( main_window, 9, 1200, panel_cancel );
    print_dialog();
    check( print_runs == 2 && !print_ok, "Cancel on the print panel -> FALSE" );
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
        check( query_has( tree, "\"img\":0" ) && query_has( tree, "\"0\":\"uttype:public.folder\"" ) &&
               query_has( tree, "\"1\":\"uttype:public." ),   /* shell32's file and document look the same at 16 px */
               "the nodes' icons: shell32's folder and document as the Finder's" );

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

    /* toolbars: native buttons, comctl32's standard images as SF Symbols */
    {
        HWND tb = ctl[ID_TOOLBAR];
        check( pIsTranslated( tb ) && query_has( tb, "\"sym\":\"sf:doc.badge.plus\"" ) &&
               query_has( tb, "\"sym\":\"sf:printer\"" ) && query_has( tb, "\"sep\":true" ),
               "a toolbar's buttons are native, comctl32's standard images as SF Symbols" );
        inject( tb, "{\"t\":\"click\",\"v\":0}" );
        pump( 200 );
        check( got_tb_command[0] == 1, "a native toolbar click -> WM_COMMAND with the button's id" );
        inject( tb, "{\"t\":\"click\",\"v\":3}" );
        pump( 200 );
        check( SendMessageW( tb, TB_ISBUTTONCHECKED, 9103, 0 ) && got_tb_command[2] == 1 &&
               query_object_has( tb, 9103, "\"checked\":true" ),
               "a check button toggles in Win32 and shows checked natively" );
        SendMessageW( tb, TB_ENABLEBUTTON, 9102, FALSE );
        pump( 150 );
        check( query_object_has( tb, 9102, "\"enabled\":false" ), "TB_ENABLEBUTTON reaches the native button" );
        SendMessageW( tb, TB_ENABLEBUTTON, 9102, TRUE );
        inject( tb, "{\"t\":\"dropdown\",\"v\":4}" );
        pump( 200 );
        check( got_tb_dropdown == 1 && got_tb_command[3] == 0, "a native drop-down arrow -> TBN_DROPDOWN, not the command" );
    }

    /* ComboBoxEx: the native editable combo; choices through its own combo box part */
    {
        HWND cbex = ctl[ID_CBEX], edit = (HWND)SendMessageW( ctl[ID_CBEX], CBEM_GETEDITCONTROL, 0, 0 );
        WCHAR text[64];

        check( pIsTranslated( cbex ) && query_has( cbex, "\"items\":[\"Arial\",\"Courier New\",\"Times New Roman\"]" ) &&
               query_has( cbex, "\"selection\":2" ) && query_has( cbex, "\"text\":\"Times New Roman\"" ),
               "a ComboBoxEx is a native combo with its items and selection" );
        inject( cbex, "{\"t\":\"select\",\"v\":1}" );
        pump( 200 );
        GetWindowTextW( edit, text, ARRAYSIZE(text) );
        check( SendMessageW( cbex, CB_GETCURSEL, 0, 0 ) == 1 && !wcscmp( text, L"Courier New" ) &&
               got_command[ID_CBEX][CBN_SELENDOK & 15] == 1,
               "a native choice -> its edit's text and CBN_SELENDOK from the ComboBoxEx" );
        inject( cbex, "{\"t\":\"text\",\"s\":\"Consolas\"}" );
        pump( 200 );
        GetWindowTextW( edit, text, ARRAYSIZE(text) );
        check( !wcscmp( text, L"Consolas" ) && query_has( cbex, "\"text\":\"Consolas\"" ), "native typing goes into its edit" );
    }

    /* EM_SHOWBALLOONTIP (wine's edit has none): a popover on the native field */
    {
        EDITBALLOONTIP tip = { sizeof(tip), L"Caps Lock is on", L"Having Caps Lock on may cause you to enter your "
                               L"password incorrectly.", TTI_WARNING };
        LRESULT shown = SendMessageW( ctl[ID_PASSWORD], EM_SHOWBALLOONTIP, 0, (LPARAM)&tip );

        pump( 300 );
        check( shown && query_has( ctl[ID_PASSWORD], "\"title\":\"Caps Lock is on\"" ) &&
               query_has( ctl[ID_PASSWORD], "\"icon\":\"warning\"" ) &&
               query_has( ctl[ID_PASSWORD], "\"popoverShown\":true" ),
               "EM_SHOWBALLOONTIP -> TRUE and a popover on the native field" );
        shown = SendMessageW( ctl[ID_PASSWORD], EM_HIDEBALLOONTIP, 0, 0 );
        pump( 400 );
        check( shown && query_lacks( ctl[ID_PASSWORD], "\"balloon\"" ) &&
               query_has( ctl[ID_PASSWORD], "\"popoverShown\":false" ),
               "EM_HIDEBALLOONTIP -> the popover goes" );
    }

    /* vertical trackbar: a vertical NSSlider, mirrored (Win32's minimum is at the top) */
    check( pIsTranslated( ctl[ID_VTRACK] ) && query_has( ctl[ID_VTRACK], "\"sliderVertical\":true" ) &&
           query_has( ctl[ID_VTRACK], "\"sliderValue\":8" ) && query_has( ctl[ID_VTRACK], "\"sliderTicks\":11" ),
           "a vertical trackbar is a vertical NSSlider with its ticks, minimum at the top" );
    SendMessageW( ctl[ID_VTRACK], TBM_SETPOS, TRUE, 7 );
    pump( 150 );
    check( query_has( ctl[ID_VTRACK], "\"sliderValue\":3" ), "TBM_SETPOS reaches the vertical slider" );
    inject( ctl[ID_VTRACK], "{\"t\":\"valueEnd\",\"v\":4}" );
    pump( 200 );
    check( SendMessageW( ctl[ID_VTRACK], TBM_GETPOS, 0, 0 ) == 4 && got_vtrack_pos == 4 && got_vtrack_end == 1,
           "the vertical slider -> TBM_GETPOS, WM_VSCROLL TB_THUMBPOSITION and TB_ENDTRACK" );
    inject( ctl[ID_VTRACK], "{\"t\":\"realClick\"}" );
    pump( 300 );
    if (got_vtrack_end != 2)
        printf( "      vertical slider after a real click: position %d, %d TB_ENDTRACK\n",
                (int)SendMessageW( ctl[ID_VTRACK], TBM_GETPOS, 0, 0 ), got_vtrack_end );
    check( SendMessageW( ctl[ID_VTRACK], TBM_GETPOS, 0, 0 ) == 5 && got_vtrack_end == 2,
           "a real click in the vertical slider's middle -> position 5 and TB_ENDTRACK" );

    /* SysLink: its markup as real links; a click -> NM_CLICK with the link */
    check( pIsTranslated( ctl[ID_LINK] ) && query_has( ctl[ID_LINK], "\"links\":[\"Wine site\",\"help\"]" ),
           "a SysLink's text and links reach the native view" );
    inject( ctl[ID_LINK], "{\"t\":\"link\",\"v\":1}" );
    pump( 200 );
    check( got_link_click == 1 && !wcscmp( got_link_id, L"help" ), "a native link click -> NM_CLICK with the link's index and id" );

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
    check( query_has( ctl[ID_LVICON], "\"0\":\"uttype:com.apple.application-bundle\"" ) &&
           query_has( ctl[ID_LVICON], "\"1\":\"sf:info.circle.fill;palette=white,systemBlue\"" ) &&
           query_has( ctl[ID_LVICON], "\"2\":\"nsimage:NSCaution\"" ),
           "wine's stock icons in an image list show as the macOS ones (known by their pixels)" );
    /* stock icons as the macOS ones: known by where they came from, or by their pixels */
    {
        HICON stock = LoadIconW( NULL, (LPCWSTR)IDI_INFORMATION ), copy, own;
        ICONINFO info;
        BYTE and_bits[32 * 32 / 8], xor_bits[32 * 32 * 4];
        int k;

        check( query_has( ctl[ID_ICON], "\"imageSymbol\":\"sf:info.circle.fill;palette=white,systemBlue\"" ),
               "IDI_INFORMATION shows as the macOS info symbol" );
        GetIconInfo( stock, &info );
        copy = CreateIconIndirect( &info );
        DeleteObject( info.hbmColor );
        DeleteObject( info.hbmMask );
        SendMessageW( ctl[ID_ICON], STM_SETICON, (WPARAM)LoadIconW( NULL, (LPCWSTR)IDI_WARNING ), 0 );
        pump( 200 );
        check( query_has( ctl[ID_ICON], "\"imageSymbol\":\"nsimage:NSCaution\"" ), "STM_SETICON IDI_WARNING -> the macOS caution icon" );
        SendMessageW( ctl[ID_ICON], STM_SETICON, (WPARAM)copy, 0 );
        pump( 200 );
        check( query_has( ctl[ID_ICON], "\"imageSymbol\":\"sf:info.circle.fill;palette=white,systemBlue\"" ),
               "a copy of IDI_INFORMATION (no resource behind it) is known by its pixels" );
        /* the app's own artwork stays */
        memset( and_bits, 0, sizeof(and_bits) );
        for (k = 0; k < 32 * 32; k++) *(DWORD *)(xor_bits + k * 4) = 0xff000000 | (k * 2654435761u >> 8);
        own = CreateIcon( NULL, 32, 32, 1, 32, and_bits, xor_bits );
        SendMessageW( ctl[ID_ICON], STM_SETICON, (WPARAM)own, 0 );
        pump( 200 );
        check( query_has( ctl[ID_ICON], "\"imageWidth\":32" ) && query_lacks( ctl[ID_ICON], "\"imageSymbol\":" ),
               "an app's own icon keeps its artwork" );
        SendMessageW( ctl[ID_ICON], STM_SETICON, (WPARAM)stock, 0 );
        pump( 200 );
        DestroyIcon( copy );
        DestroyIcon( own );
    }
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
    /* ...and every DC wine gets to draw them with is empty, including the
     * parent-clipped DCs of CS_PARENTDC classes (Button, Static, Edit, ComboBox) */
    for (i = 0, r = 0; i < ARRAYSIZE(ids); i++)
    {
        HDC hdc = GetDCEx( ctl[ids[i]], 0, DCX_USESTYLE | DCX_CACHE );
        RECT box;
        if (GetClipBox( hdc, &box ) == NULLREGION) r++;
        else printf( "      control %d can still be drawn by wine (%ld,%ld)-(%ld,%ld)\n", ids[i],
                     box.left, box.top, box.right, box.bottom );
        ReleaseDC( ctl[ids[i]], hdc );
    }
    check( r == ARRAYSIZE(ids), "wine can't draw a translated control (its DC's clip box is empty)" );

    /* content the app added after creation, before anything else pushed */
    check( query_has( ctl[ID_MULTI], "Item 6" ), "items added after creation reach a multi-select list" );
    check( query_has( ctl[ID_REPORT], "\"12 KB\"" ), "a report's second column shows its own subitem" );

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
    inject( ctl[ID_PUSH], "{\"t\":\"realClick\"}" );
    pump( 400 );
    check( got_command[ID_PUSH][BN_CLICKED] == 2, "a mouse click on the native button (through AppKit) -> BN_CLICKED" );
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
    check( query_int( ctl[ID_TAB], "tabBarWidth" ) >= 120, "the tab bar has room for its three tabs (not one squeezed segment)" );

    selftest2();

    /* group boxes: a real NSBox, its title above the box when the app leaves room */
    {
        RECT rc;
        int h;

        check( query_has( ctl[ID_GROUP], "\"titleAbove\":false" ),
               "a group box right under a row of buttons keeps its title inside the box" );
        /* a window of its own: a button, and a group box with room above it */
        {
            HWND win = CreateWindowExW( 0, L"W2SWhitePanel", L"Group boxes", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                        200, 200, 320, 220, main_window, NULL, GetModuleHandleW( NULL ), NULL );
            HWND button = CreateWindowExW( 0, L"Button", L"Button", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           12, 10, 100, 24, win, (HMENU)1, NULL, NULL );
            HWND group = CreateWindowExW( 0, L"Button", L"Options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          12, 80, 280, 90, win, (HMENU)2, NULL, NULL );
            pump( 600 );
            h = 90;
            /* the native view reaches above the Win32 rectangle by the title's band */
            check( pIsTranslated( group ) && query_has( group, "\"titleAbove\":true" ) &&
                   query_int( group, "hostHeight" ) > h + 8,
                   "with room above, the title goes above the box, which takes the Win32 rectangle" );
            SetWindowPos( group, 0, 12, 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
            pump( 400 );
            check( query_has( group, "\"titleAbove\":false" ) && query_int( group, "hostHeight" ) == h,
                   "moved up under the button, the title goes inside the box" );
            DestroyWindow( win );
            pump( 200 );
            (void)button; (void)rc;
        }
    }

    /* property sheets: more than 5 pages get the window's native sidebar (an
     * NSSplitViewController, macOS 13+) outside wine's content; fewer keep the strip */
    {
        HWND sheet = property_sheet( 7, TRUE ), tab, page;
        RECT tab_rc, page_rc, sheet_rc, rc;
        int x;

        pump( 800 );
        tab = (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 );
        check( pIsTranslated( tab ) && query_has( tab, "\"mode\":\"window\"" ) &&
               query_has( tab, "\"windowSidebar\":true" ),
               "a 7-page property sheet gets the window's native sidebar (split view)" );
        check( query_int( tab, "sidebarRows" ) == 7 && query_int( tab, "firstRowTop" ) >= 28 &&
               query_int( tab, "firstRowTop" ) >= query_int( tab, "sidebarSafeTop" ) &&
               query_has( tab, "\"firstRowVisible\":true" ),
               "the sidebar lists the pages, starting below the titlebar and toolbar" );
        x = query_int( tab, "wineViewX" );
        check( x >= 150 && query_int( tab, "windowWidth" ) - query_int( tab, "wineViewWidth" ) == x,
               "wine's content sits right of the sidebar, which is outside it" );
        page = (HWND)SendMessageW( sheet, PSM_GETCURRENTPAGEHWND, 0, 0 );
        GetWindowRect( tab, &tab_rc );
        GetWindowRect( page, &page_rc );
        check( page_rc.left - tab_rc.left < 8, "the pages fill the tab control's area (its tabs are in the sidebar)" );
        inject( tab, "{\"t\":\"select\",\"v\":3}" );
        pump( 300 );
        page = (HWND)SendMessageW( sheet, PSM_GETCURRENTPAGEHWND, 0, 0 );
        check( page && page == (HWND)SendMessageW( sheet, PSM_INDEXTOHWND, 3, 0 ) && IsWindowVisible( page ) &&
               query_has( tab, "\"selection\":3" ),
               "a native sidebar choice switches the page" );
        /* the sidebar's toggle: the window grows and shrinks around wine's content */
        GetWindowRect( sheet, &sheet_rc );
        inject( tab, "{\"t\":\"sidebarCollapse\",\"v\":1}" );
        pump( 800 );
        GetWindowRect( sheet, &rc );
        check( query_has( tab, "\"sidebarCollapsed\":true" ) && query_int( tab, "wineViewX" ) == 0 && EqualRect( &rc, &sheet_rc ),
               "hiding the sidebar shrinks the window, wine's content and rect stay" );
        inject( tab, "{\"t\":\"sidebarCollapse\",\"v\":0}" );
        pump( 800 );
        GetWindowRect( sheet, &rc );
        check( query_has( tab, "\"sidebarCollapsed\":false" ) && query_int( tab, "wineViewX" ) == x && EqualRect( &rc, &sheet_rc ),
               "showing it again grows the window back" );
        DestroyWindow( sheet );
        pump( 200 );

        sheet = property_sheet( 3, TRUE );
        pump( 600 );
        tab = (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 );
        check( pIsTranslated( tab ) && query_has( tab, "\"mode\":\"strip\"" ), "a 3-page property sheet keeps the tab strip" );
        {
            HWND page = (HWND)SendMessageW( sheet, PSM_GETCURRENTPAGEHWND, 0, 0 );
            RECT tab_rc, page_rc;
            GetWindowRect( tab, &tab_rc );
            GetWindowRect( page, &page_rc );
            check( page_rc.left - tab_rc.left == 3 && page_rc.top - tab_rc.top == 23 && tab_rc.bottom - page_rc.bottom == 7,
                   "wine puts the page in the tab view's box (TCM_ADJUSTRECT)" );
        }
        DestroyWindow( sheet );
        pump( 200 );

        /* modal, as winecfg runs its sheet: the native OK and Cancel end it, clicked
         * with the mouse (through AppKit) */
        modal_button_id = IDOK;
        modal_button_event = "{\"t\":\"realClick\"}";
        modal_button_stuck = 0;
        SetTimer( NULL, 0, 1200, modal_button_click );
        SetTimer( NULL, 0, 6000, modal_button_watchdog );
        property_sheet( 7, FALSE );
        check( !modal_button_stuck && sheet_result > 0, "a modal property sheet's native OK ends it (PropertySheet > 0)" );
        modal_button_id = IDCANCEL;
        modal_button_stuck = 0;
        SetTimer( NULL, 0, 1200, modal_button_click );
        SetTimer( NULL, 0, 6000, modal_button_watchdog );
        property_sheet( 7, FALSE );
        check( !modal_button_stuck && sheet_result == 0, "a modal property sheet's native Cancel ends it (PropertySheet 0)" );
        pump( 200 );
    }

    /* wizards: the macOS Installer layout, steps and header native, pages placed by wine */
    {
        HWND tab, page, back, next;
        RECT tab_rc, page_rc;

        inject( ctl[ID_WIZARD], "{\"t\":\"click\"}" );
        pump( 600 );
        check( got_command[ID_WIZARD][BN_CLICKED] == 1 && test_wizard, "native click on Wizard… -> BN_CLICKED" );
        tab = (HWND)SendMessageW( test_wizard, PSM_GETTABCONTROL, 0, 0 );
        check( pIsTranslated( tab ) && IsWindowVisible( tab ) && query_has( tab, "\"mode\":\"wizard\"" ) &&
               query_has( tab, "\"items\":[\"Introduction\",\"License\",\"Install\"]" ),
               "a wizard's tab control shows the steps natively" );
        page = (HWND)SendMessageW( test_wizard, PSM_GETCURRENTPAGEHWND, 0, 0 );
        GetWindowRect( tab, &tab_rc );
        GetWindowRect( page, &page_rc );
        check( page_rc.left - tab_rc.left >= 150, "wine lays the wizard's pages out right of the steps (TCM_ADJUSTRECT)" );
        back = GetDlgItem( test_wizard, 12323 );
        next = GetDlgItem( test_wizard, 12324 );
        check( query_has( back, wizard_word( back, "Back", "Go Back" ) ) &&
               query_has( next, wizard_word( next, "Next", "Continue" ) ),
               "wine's Back and Next read Go Back and Continue (in English; other languages keep wine's word)" );
        check( query_has( tab, "\"heading\":\"\"" ), "the welcome page (PSP_HIDEHEADER) has no header" );
        inject( next, "{\"t\":\"click\"}" );
        pump( 300 );
        check( got_wiznext == 1 &&
               (HWND)SendMessageW( test_wizard, PSM_GETCURRENTPAGEHWND, 0, 0 ) == (HWND)SendMessageW( test_wizard, PSM_INDEXTOHWND, 1, 0 ) &&
               query_has( tab, "\"selection\":1" ) && query_has( tab, "\"heading\":\"License agreement\"" ) &&
               query_has( tab, "\"subheading\":\"Please read the terms.\"" ),
               "native Continue -> PSN_WIZNEXT; the next page, its step and its header show" );
        SendMessageW( test_wizard, PSM_SETHEADERTITLEW, 1, (LPARAM)L"Read the license" );
        pump( 200 );
        check( query_has( tab, "\"heading\":\"Read the license\"" ) &&
               query_has( tab, "\"subheading\":\"Please read the terms.\"" ),
               "PSM_SETHEADERTITLE reaches the native header (the subtitle stays)" );
        inject( back, "{\"t\":\"click\"}" );
        pump( 300 );
        check( got_wizback == 1 && query_has( tab, "\"selection\":0" ), "native Go Back -> PSN_WIZBACK, the first step" );
        SendMessageW( test_wizard, PSM_SETCURSEL, 2, 0 );
        pump( 200 );
        check( query_has( tab, "\"selection\":2" ) && query_has( tab, "\"heading\":\"Ready to install\"" ),
               "PSM_SETCURSEL moves the native steps and header" );
        DestroyWindow( test_wizard );
        test_wizard = NULL;
        pump( 200 );
    }

    /* menus: the window's menu is in the Mac menu bar; the window keeps no strip for it */
    {
        RECT wr, cr, adj = { 0, 0, 100, 100 };
        HMENU popup;
        int extra, before;

        GetWindowRect( main_window, &wr );
        GetClientRect( main_window, &cr );
        AdjustWindowRectEx( &adj, GetWindowLongW( main_window, GWL_STYLE ), FALSE, GetWindowLongW( main_window, GWL_EXSTYLE ) );
        extra = (wr.bottom - wr.top - cr.bottom) - (adj.bottom - adj.top - 100);
        if (extra) printf( "      %d px of menu strip left in the window\n", extra );
        check( extra == 0, "the window keeps no menu strip (its menu is in the Mac menu bar)" );
        check( query_has( main_window, "\"titles\":[\"File\",\"Edit\",\"View\"]" ), "the menu bar shows File, Edit, View natively" );
        check( query_has( main_window, "9001:n" ), "Ctrl+N becomes the key equivalent Cmd-N" );
        inject( main_window, "{\"t\":\"menu\",\"v\":9001}" );
        pump( 250 );
        check( got_menu_new == 1, "a native menu choice -> WM_COMMAND" );
        before = got_initmenupopup;
        inject( main_window, "{\"t\":\"open\",\"v\":2}" );
        pump( 500 );
        check( got_initmenupopup > before &&
               query_has( main_window, status_bar_checked ? "\"checked\":[9010]" : "\"checked\":[]" ),
               "opening a native menu sends WM_INITMENUPOPUP, and the check mark the app sets there shows" );

        popup = CreatePopupMenu();
        AppendMenuW( popup, MF_STRING, 9004, L"&Copy" );
        AppendMenuW( popup, MF_STRING, 9005, L"&Paste" );
        SetTimer( main_window, 3, 700, choose_popup );
        r = TrackPopupMenu( popup, TPM_RETURNCMD, wr.left + 60, wr.top + 90, 0, main_window, NULL );
        check( r == 9004, "TrackPopupMenu -> a native popup menu returns the chosen command" );
        DestroyMenu( popup );
    }

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

    selftest_pickers();
    selftest_pagesetup();
    selftest_find();
    selftest_about();
    selftest_taskbar();
    selftest_flash();
    selftest_look();

    printf( "%d passed, %d failed\n", passes, failures );
    return failures;
}

/* gallery.exe /capture <unix dir>: draws the gallery and its sheets into PNGs
 * there (W2SDebugInject "capture"), for looking at them without a screen */
static void capture_window( HWND control, const char *dir, const char *name )
{
    char event[1024];
    snprintf( event, sizeof(event), "{\"t\":\"capture\",\"s\":\"%s/%s.png\"}", dir, name );
    inject( control, event );
    printf( "captured %s/%s.png\n", dir, name );
}

static const char *about_dir, *about_name;

static void CALLBACK about_capture( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    KillTimer( hwnd, id );
    capture_window( NULL, about_dir, about_name );
    inject( NULL, "{\"t\":\"close\"}" );
}

static int capture( const WCHAR *args )
{
    HMODULE w2s = GetModuleHandleW( L"win32swiftui.dll" );
    char dir[512];
    HWND sheet;

    if (!w2s) return 1;
    pQuery = (void *)GetProcAddress( w2s, "W2SDebugQuery" );
    pInject = (void *)GetProcAddress( w2s, "W2SDebugInject" );
    pFree = (void *)GetProcAddress( w2s, "W2SDebugFree" );
    pIsTranslated = (void *)GetProcAddress( w2s, "W2SIsTranslated" );
    while (*args == ' ') args++;
    WideCharToMultiByte( CP_UTF8, 0, args, -1, dir, sizeof(dir), NULL, NULL );
    in_selftest = TRUE;
    pump( 1500 );
    capture_window( ctl[ID_PUSH], dir, "gallery" );
    sheet = property_sheet( 7, TRUE );
    pump( 4000 );
    {
        HWND tab = (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 );
        char *q = pQuery( tab );
        printf( "sheet7 query: %s\n", q ? q : "(null)" );
        pFree( q );
        capture_window( tab, dir, "sheet7" );
    }
    DestroyWindow( sheet );
    sheet = property_sheet( 3, TRUE );
    pump( 1500 );
    capture_window( (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 ), dir, "sheet3" );
    DestroyWindow( sheet );
    sheet = wizard_sheet( TRUE );
    pump( 1500 );
    capture_window( (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 ), dir, "wizard" );
    DestroyWindow( sheet );
    {
        ITaskbarList3 *list;
        char event[600];

        CoInitialize( NULL );
        if (SUCCEEDED( CoCreateInstance( &CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
                                         (void **)&list ) ))
        {
            ITaskbarList3_HrInit( list );
            ITaskbarList3_SetProgressValue( list, main_window, 40, 100 );
            pump( 300 );
            snprintf( event, sizeof(event), "{\"t\":\"dock\",\"s\":\"%s/dock.png\"}", dir );
            pFree( pInject( NULL, event ) );
            printf( "captured %s/dock.png\n", dir );
            ITaskbarList3_SetProgressState( list, main_window, TBPF_NOPROGRESS );
            ITaskbarList3_Release( list );
        }
    }
    {
        /* AppKit's Find panel for ReplaceText */
        static WCHAR what[64] = L"needle", with[64] = L"pin";
        static FINDREPLACEW fr;
        char event[600];
        HWND dlg;

        fr.lStructSize = sizeof(fr);
        fr.hwndOwner = main_window;
        fr.lpstrFindWhat = what;
        fr.wFindWhatLen = ARRAYSIZE(what);
        fr.lpstrReplaceWith = with;
        fr.wReplaceWithLen = ARRAYSIZE(with);
        if ((dlg = ReplaceTextW( &fr )))
        {
            pump( 800 );
            snprintf( event, sizeof(event), "{\"t\":\"capture\",\"s\":\"%s/find.png\"}", dir );
            pFree( pInject( NULL, event ) );
            printf( "captured %s/find.png\n", dir );
            DestroyWindow( dlg );
        }
    }
    about_dir = dir;
    about_name = "about";
    SetTimer( main_window, 31, 1500, about_capture );
    ShellAboutW( main_window, L"Gallery#Win32-to-SwiftUI gallery", L"Every control the runtime translates.", NULL );
    {
        /* an app's own icon, as pixels */
        HMODULE notepad = LoadLibraryExW( L"notepad.exe", NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE );
        HICON icon = notepad ? LoadImageW( notepad, MAKEINTRESOURCEW( 0x300 ), IMAGE_ICON, 128, 128, 0 ) : NULL;

        printf( "notepad icon: %p\n", icon );
        about_name = "about-icon";
        SetTimer( main_window, 31, 1500, about_capture );
        ShellAboutW( main_window, L"Notepad", L"Wine Notepad", icon );
        if (icon) DestroyIcon( icon );
    }
    pump( 300 );
    return 0;
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
    wc.lpfnWndProc = white_panel_proc;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"W2SWhitePanel";
    RegisterClassW( &wc );
    main_window = CreateWindowExW( 0, L"W2SGallery", L"Win32-to-SwiftUI gallery", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1150, 720, NULL, NULL, inst, NULL );
    {
        /* a menu bar: it goes to the Mac menu bar (menus.c), the window keeps no strip for it */
        HMENU bar = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(), view = CreatePopupMenu();
        AppendMenuW( file, MF_STRING, 9001, L"&New\tCtrl+N" );
        AppendMenuW( file, MF_STRING, 9002, L"&Open...\tCtrl+O" );
        AppendMenuW( file, MF_SEPARATOR, 0, NULL );
        AppendMenuW( file, MF_STRING, 9003, L"E&xit" );
        AppendMenuW( edit, MF_STRING, 9004, L"&Copy\tCtrl+C" );
        AppendMenuW( edit, MF_STRING, 9005, L"&Paste\tCtrl+V" );
        AppendMenuW( view, MF_STRING | MF_CHECKED, 9010, L"&Status bar" );
        AppendMenuW( bar, MF_POPUP, (UINT_PTR)file, L"&File" );
        AppendMenuW( bar, MF_POPUP, (UINT_PTR)edit, L"&Edit" );
        AppendMenuW( bar, MF_POPUP, (UINT_PTR)view, L"&View" );
        SetMenu( main_window, bar );
    }
    create_controls();
    ShowWindow( main_window, SW_SHOW );
    UpdateWindow( main_window );

    if (wcsstr( cmdline, L"/capture" ))
    {
        ret = capture( wcsstr( cmdline, L"/capture" ) + 8 );
        DestroyWindow( main_window );
        return ret;
    }
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
