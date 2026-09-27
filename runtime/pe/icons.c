/*
 * win32swiftui.dll: stock icons as macOS images (map: 75-icons.yaml).
 *
 * wine's own icons (user32's IDI_* and shell32's files, folders and drives)
 * are Windows artwork. A native view shows the macOS image with the same
 * meaning instead: an SF Symbol, Finder's icon for a kind of file, an AppKit
 * image. The PE side recognises the icon and sends the image's spec, which the
 * native side resolves (Icons.swift).
 *
 * An icon loaded from a resource says where it came from (GetIconInfoExW:
 * module and resource id). One copied into an image list doesn't: the image
 * list keeps pixels only. So each stock icon also goes through a duplicate of
 * the same image list (same colour depth, mask and background) at the same
 * size, and an image whose pixels match one of those exactly is that icon.
 * The app's own icons match nothing and keep their artwork.
 */
#include "w2s_pe.h"
#include "w2s_map_tables.h"

static UINT64 fnv1a( const BYTE *bits, size_t size )
{
    UINT64 h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < size; i++) h = (h ^ bits[i]) * 0x100000001b3ull;
    return h;
}

static const WCHAR *basename_w( const WCHAR *path )
{
    const WCHAR *p = wcsrchr( path, '\\' );
    return p ? p + 1 : path;
}

/* 1: the spec; 0: an icon from a resource that isn't a stock one; -1: no origin */
static int spec_from_origin( HICON icon, const char **spec )
{
    ICONINFOEXW info;
    const WCHAR *module;
    unsigned int i;

    memset( &info, 0, sizeof(info) );
    info.cbSize = sizeof(info);
    if (!GetIconInfoExW( icon, &info )) return -1;
    if (info.hbmColor) DeleteObject( info.hbmColor );
    if (info.hbmMask) DeleteObject( info.hbmMask );
    if (!info.szModName[0]) return -1;
    if (!info.wResID) return 0;
    module = basename_w( info.szModName );
    for (i = 0; i < ARRAYSIZE(w2s_icon_entries); i++)
    {
        if (w2s_icon_entries[i].id != info.wResID || _wcsicmp( w2s_icon_entries[i].module, module )) continue;
        *spec = w2s_icon_entries[i].spec;
        return 1;
    }
    return 0;
}

/* ---------- fingerprints ---------- */

/* the stock icons as one image list (or none: a plain icon) holds them at one size */
struct fingerprints
{
    HIMAGELIST himl;
    int cx, cy;
    unsigned int count;
    UINT64 hash[ARRAYSIZE(w2s_icon_entries) * 3];
    const char *spec[ARRAYSIZE(w2s_icon_entries) * 3];
};

#define FINGERPRINT_SETS 4
static struct fingerprints *sets[FINGERPRINT_SETS];
static unsigned int next_set;
static SRWLOCK sets_lock = SRWLOCK_INIT;     /* each UI thread snapshots its own controls */

static void add_fingerprint( struct fingerprints *fp, HIMAGELIST dup, HICON icon, const char *spec )
{
    HICON copy = NULL;
    BYTE *bits;
    int index;

    if (dup)
    {
        /* through the app's image list, as its own images went */
        if ((index = ImageList_ReplaceIcon( dup, -1, icon )) < 0) return;
        if (!(copy = ImageList_GetIcon( dup, index, ILD_NORMAL ))) return;
        icon = copy;
    }
    if ((bits = w2s_image_bgra( icon, NULL, fp->cx, fp->cy )))
    {
        fp->hash[fp->count] = fnv1a( bits, (size_t)fp->cx * fp->cy * 4 );
        fp->spec[fp->count++] = spec;
        HeapFree( GetProcessHeap(), 0, bits );
    }
    if (copy) DestroyIcon( copy );
}

static struct fingerprints *make_fingerprints( HIMAGELIST himl, int cx, int cy )
{
    /* LoadIcon gives the SM_CXICON size, which an image list of another size scales */
    const int sizes[3] = { cx, GetSystemMetrics( SM_CXICON ), GetSystemMetrics( SM_CXSMICON ) };
    struct fingerprints *fp;
    HIMAGELIST dup = NULL;
    HMODULE module;
    unsigned int i, s, k;

    if (!(fp = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*fp) ))) return NULL;
    fp->himl = himl;
    fp->cx = cx;
    fp->cy = cy;
    if (himl && !(dup = ImageList_Duplicate( himl )))
    {
        HeapFree( GetProcessHeap(), 0, fp );
        return NULL;
    }
    for (i = 0; i < ARRAYSIZE(w2s_icon_entries); i++)
    {
        /* shell32's icons come from shell32 only: an app without it has none of them */
        if (!(module = GetModuleHandleW( w2s_icon_entries[i].module ))) continue;
        for (s = 0; s < 3; s++)
        {
            HICON icon;

            for (k = 0; k < s; k++) if (sizes[k] == sizes[s]) break;
            if (k < s) continue;
            icon = LoadImageW( module, MAKEINTRESOURCEW( w2s_icon_entries[i].id ), IMAGE_ICON,
                               sizes[s], sizes[s] * cy / max( cx, 1 ), 0 );
            if (!icon) continue;
            add_fingerprint( fp, dup, icon, w2s_icon_entries[i].spec );
            DestroyIcon( icon );
        }
    }
    if (dup) ImageList_Destroy( dup );
    TRACE( "stock icon fingerprints for %p at %dx%d: %u\n", himl, cx, cy, fp->count );
    return fp;
}

static const char *spec_from_pixels( const BYTE *bits, int cx, int cy, HIMAGELIST himl )
{
    struct fingerprints *fp = NULL;
    const char *spec = NULL;
    UINT64 hash;
    unsigned int i;

    AcquireSRWLockExclusive( &sets_lock );
    for (i = 0; i < FINGERPRINT_SETS; i++)
        if (sets[i] && sets[i]->himl == himl && sets[i]->cx == cx && sets[i]->cy == cy) fp = sets[i];
    if (!fp && (fp = make_fingerprints( himl, cx, cy )))
    {
        /* the oldest set goes (an image list can be destroyed and its handle reused:
         * then it only costs a set made again) */
        HeapFree( GetProcessHeap(), 0, sets[next_set] );
        sets[next_set] = fp;
        next_set = (next_set + 1) % FINGERPRINT_SETS;
    }
    if (fp)
    {
        hash = fnv1a( bits, (size_t)cx * cy * 4 );
        for (i = 0; i < fp->count && !spec; i++) if (fp->hash[i] == hash) spec = fp->spec[i];
    }
    ReleaseSRWLockExclusive( &sets_lock );
    return spec;
}

/***********************************************************************
 *      w2s_stock_icon
 *
 * The macOS image for a stock icon, as a spec for the native side, or NULL
 * for the app's own artwork. icon: the icon, when there is one to ask where it
 * came from; bits: its pixels at cx x cy (w2s_image_bgra); himl: the image list
 * they came from, or NULL.
 */
const char *w2s_stock_icon( HICON icon, const BYTE *bits, int cx, int cy, HIMAGELIST himl )
{
    const char *spec = NULL;

    if (icon)
    {
        switch (spec_from_origin( icon, &spec ))
        {
        case 1: return spec;
        case 0: return NULL;    /* the app's own resource */
        }
    }
    if (!bits || cx <= 0 || cy <= 0) return NULL;
    return spec_from_pixels( bits, cx, cy, himl );
}
