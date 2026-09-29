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
 *
 * An image list made from one of wine's toolbar strips (ImageList_LoadImage, or
 * LoadBitmap and ImageList_Add: Internet Explorer's toolbar, the help viewer's
 * contents, cryptui's certificates) doesn't say so either, and the colour the
 * app masked out isn't known. The strip goes through the same duplicate, and an
 * image is the strip's when each pixel it shows is the strip's there and each it
 * leaves out is one colour (the mask) or clear.
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
    if (!info.wResID && !info.szResName[0]) return 0;
    module = basename_w( info.szModName );
    for (i = 0; i < ARRAYSIZE(w2s_icon_entries); i++)
    {
        const struct w2s_icon_entry *e = &w2s_icon_entries[i];

        if (info.wResID ? e->id != info.wResID : !e->name || _wcsicmp( e->name, info.szResName )) continue;
        if (_wcsicmp( e->module, module )) continue;
        *spec = e->spec;
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
    unsigned int strips;            /* strip images through the same image list */
    BYTE *strip_bits[ARRAYSIZE(w2s_toolbar_images) * 2];
    const char *strip_spec[ARRAYSIZE(w2s_toolbar_images) * 2];
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

static void free_fingerprints( struct fingerprints *fp )
{
    unsigned int i;

    if (!fp) return;
    for (i = 0; i < fp->strips; i++) HeapFree( GetProcessHeap(), 0, fp->strip_bits[i] );
    HeapFree( GetProcessHeap(), 0, fp );
}

/* a strip's mapped images (those of entry first on), as the duplicate makes them */
static void add_strip( struct fingerprints *fp, HIMAGELIST dup, unsigned int first, HBITMAP bitmap )
{
    const struct w2s_toolbar_image *e = &w2s_toolbar_images[first];
    int base = ImageList_GetImageCount( dup ), added;
    unsigned int k;
    BITMAP bm;

    if (!GetObjectW( bitmap, sizeof(bm), &bm ) || bm.bmHeight != fp->cy || bm.bmWidth < fp->cx ||
        ImageList_Add( dup, bitmap, NULL ) < 0)
        return;
    added = ImageList_GetImageCount( dup ) - base;
    for (k = first; k < ARRAYSIZE(w2s_toolbar_images) && fp->strips < ARRAYSIZE(fp->strip_bits); k++)
    {
        const struct w2s_toolbar_image *img = &w2s_toolbar_images[k];
        HICON icon;

        if (img->bitmap != e->bitmap || _wcsicmp( img->module, e->module ) || (int)img->index >= added) continue;
        if (!(icon = ImageList_GetIcon( dup, base + img->index, ILD_NORMAL ))) continue;
        if ((fp->strip_bits[fp->strips] = w2s_image_bgra( icon, NULL, fp->cx, fp->cy )))
            fp->strip_spec[fp->strips++] = img->spec;
        DestroyIcon( icon );
    }
}

/* wine's strips whose module the app has, at the image list's height, through it: loaded
 * both ways apps load them (LoadBitmap's device bitmap, ImageList_LoadImage's DIB section),
 * which a 32-bit strip's alpha goes through differently */
static void add_strips( struct fingerprints *fp, HIMAGELIST dup )
{
    unsigned int i, k;

    for (i = 0; i < ARRAYSIZE(w2s_toolbar_images); i++)
    {
        const struct w2s_toolbar_image *e = &w2s_toolbar_images[i];
        HMODULE module;
        HBITMAP bitmap;

        /* each strip once (its images are together); comctl32's standard ones come by TB_LOADIMAGES */
        for (k = 0; k < i; k++)
            if (w2s_toolbar_images[k].bitmap == e->bitmap && !_wcsicmp( w2s_toolbar_images[k].module, e->module )) break;
        if (k < i || !_wcsicmp( e->module, L"comctl32.dll" ) || !(module = GetModuleHandleW( e->module ))) continue;
        if ((bitmap = LoadBitmapW( module, MAKEINTRESOURCEW( e->bitmap ) )))
        {
            add_strip( fp, dup, i, bitmap );
            DeleteObject( bitmap );
        }
        if ((bitmap = LoadImageW( module, MAKEINTRESOURCEW( e->bitmap ), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION )))
        {
            add_strip( fp, dup, i, bitmap );
            DeleteObject( bitmap );
        }
    }
}

/* whether an image (bits) is a strip's image (strip), as add_strips made it */
static BOOL is_strip_image( const BYTE *bits, const BYTE *strip, int cx, int cy )
{
    const BYTE *key = NULL;
    int i, c, shown = 0, n = cx * cy;

    for (i = 0; i < n; i++)
    {
        const BYTE *p = bits + i * 4, *q = strip + i * 4;

        if (!p[3])
        {
            /* left out: clear there too, or the one colour the app masked out */
            if (!q[3]) continue;
            if (!key) key = q;
            else if (memcmp( key, q, 3 )) return FALSE;
            continue;
        }
        for (c = 0; c < 4; c++) if (abs( p[c] - q[c] ) > 2) return FALSE;
        shown++;
    }
    return shown >= n / 8;  /* not an empty image */
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
            icon = LoadImageW( module, w2s_icon_entries[i].name ? w2s_icon_entries[i].name :
                               MAKEINTRESOURCEW( w2s_icon_entries[i].id ), IMAGE_ICON,
                               sizes[s], sizes[s] * cy / max( cx, 1 ), 0 );
            if (!icon) continue;
            add_fingerprint( fp, dup, icon, w2s_icon_entries[i].spec );
            DestroyIcon( icon );
        }
    }
    if (dup)
    {
        add_strips( fp, dup );
        ImageList_Destroy( dup );
    }
    TRACE( "stock icon fingerprints for %p at %dx%d: %u, strip images %u\n", himl, cx, cy, fp->count, fp->strips );
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
        free_fingerprints( sets[next_set] );
        sets[next_set] = fp;
        next_set = (next_set + 1) % FINGERPRINT_SETS;
    }
    if (fp)
    {
        hash = fnv1a( bits, (size_t)cx * cy * 4 );
        for (i = 0; i < fp->count && !spec; i++) if (fp->hash[i] == hash) spec = fp->spec[i];
        for (i = 0; i < fp->strips && !spec; i++)
            if (is_strip_image( bits, fp->strip_bits[i], cx, cy )) spec = fp->strip_spec[i];
    }
    ReleaseSRWLockExclusive( &sets_lock );
    return spec;
}

/***********************************************************************
 *      w2s_forget_image_list
 *
 * A view has another image list (or the same handle for another one, which
 * an image list destroyed and made again can get): what was made for it goes.
 */
void w2s_forget_image_list( HIMAGELIST himl )
{
    unsigned int i;

    AcquireSRWLockExclusive( &sets_lock );
    for (i = 0; i < FINGERPRINT_SETS; i++)
    {
        if (!sets[i] || sets[i]->himl != himl) continue;
        free_fingerprints( sets[i] );
        sets[i] = NULL;
    }
    ReleaseSRWLockExclusive( &sets_lock );
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
