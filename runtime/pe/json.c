/*
 * win32swiftui.dll: just enough JSON for snapshots, events and results.
 */
#include "w2s_pe.h"

WCHAR *strdupW( const WCHAR *s )
{
    size_t n = (wcslen( s ) + 1) * sizeof(WCHAR);
    WCHAR *r = HeapAlloc( GetProcessHeap(), 0, n );
    if (r) memcpy( r, s, n );
    return r;
}

char *utf8_from_wide( const WCHAR *s, int len )
{
    int n = WideCharToMultiByte( CP_UTF8, 0, s, len, NULL, 0, NULL, NULL );
    char *r = HeapAlloc( GetProcessHeap(), 0, n + 1 );
    if (!r) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, s, len, r, n, NULL, NULL );
    r[n] = 0;
    return r;
}

WCHAR *wide_from_utf8( const char *s, int len )
{
    int n = MultiByteToWideChar( CP_UTF8, 0, s, len, NULL, 0 );
    WCHAR *r = HeapAlloc( GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR) );
    if (!r) return NULL;
    MultiByteToWideChar( CP_UTF8, 0, s, len, r, n );
    r[n] = 0;
    return r;
}

/* ---------- writer ---------- */

static void put( struct json *j, const char *s, size_t n )
{
    if (j->len + n + 1 > j->cap)
    {
        size_t cap = max( j->cap * 2, j->len + n + 256 );
        char *buf = j->buf ? HeapReAlloc( GetProcessHeap(), 0, j->buf, cap ) : HeapAlloc( GetProcessHeap(), 0, cap );
        if (!buf) return;
        j->buf = buf;
        j->cap = cap;
    }
    memcpy( j->buf + j->len, s, n );
    j->len += n;
    j->buf[j->len] = 0;
}

static void puts_( struct json *j, const char *s )
{
    put( j, s, strlen( s ) );
}

static void escaped( struct json *j, const char *s )
{
    put( j, "\"", 1 );
    for (; *s; s++)
    {
        unsigned char c = *s;
        char tmp[8];
        if (c == '"' || c == '\\') { put( j, "\\", 1 ); put( j, s, 1 ); }
        else if (c == '\n') puts_( j, "\\n" );
        else if (c == '\r') puts_( j, "\\r" );
        else if (c == '\t') puts_( j, "\\t" );
        else if (c < 0x20) { snprintf( tmp, sizeof(tmp), "\\u%04x", c ); puts_( j, tmp ); }
        else put( j, s, 1 );
    }
    put( j, "\"", 1 );
}

static void key( struct json *j, const char *k )
{
    if (!j->first[j->depth]) put( j, ",", 1 );
    j->first[j->depth] = FALSE;
    if (k)
    {
        escaped( j, k );
        put( j, ":", 1 );
    }
}

void json_init( struct json *j )
{
    memset( j, 0, sizeof(*j) );
    j->first[0] = TRUE;
}

void json_free( struct json *j )
{
    if (j->buf) HeapFree( GetProcessHeap(), 0, j->buf );
    j->buf = NULL;
}

void json_obj_begin( struct json *j )
{
    if (j->depth) key( j, NULL );
    put( j, "{", 1 );
    j->first[++j->depth] = TRUE;
}

void json_key_obj_begin( struct json *j, const char *k )
{
    key( j, k );
    put( j, "{", 1 );
    j->first[++j->depth] = TRUE;
}

void json_obj_end( struct json *j )
{
    put( j, "}", 1 );
    j->depth--;
}

void json_arr_begin( struct json *j, const char *k )
{
    key( j, k );
    put( j, "[", 1 );
    j->first[++j->depth] = TRUE;
}

void json_arr_end( struct json *j )
{
    put( j, "]", 1 );
    j->depth--;
}

void json_str( struct json *j, const char *k, const WCHAR *value )
{
    char *utf8 = utf8_from_wide( value ? value : L"", -1 );
    key( j, k );
    escaped( j, utf8 ? utf8 : "" );
    if (utf8) HeapFree( GetProcessHeap(), 0, utf8 );
}

void json_str_a( struct json *j, const char *k, const char *value )
{
    key( j, k );
    escaped( j, value ? value : "" );
}

void json_int( struct json *j, const char *k, INT64 value )
{
    char tmp[32];
    key( j, k );
    snprintf( tmp, sizeof(tmp), "%lld", (long long)value );
    puts_( j, tmp );
}

void json_num( struct json *j, const char *k, double value )
{
    char tmp[64];
    key( j, k );
    snprintf( tmp, sizeof(tmp), "%.17g", value );
    puts_( j, tmp );
}

void json_bool( struct json *j, const char *k, BOOL value )
{
    key( j, k );
    puts_( j, value ? "true" : "false" );
}

void json_raw( struct json *j, const char *k, const char *raw )
{
    key( j, k );
    puts_( j, raw );
}

/* ---------- reader ---------- */

static const char *skip_ws( const char *p )
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

/* parses a JSON string at p (pointing at the quote) into UTF-16 */
static const char *parse_string( const char *p, WCHAR **out )
{
    WCHAR *buf;
    size_t n = 0, cap = 64;

    if (*p != '"') return NULL;
    p++;
    buf = HeapAlloc( GetProcessHeap(), 0, cap * sizeof(WCHAR) );
    while (*p && *p != '"')
    {
        WCHAR w[2];
        int nw = 0;

        if (*p == '\\')
        {
            p++;
            switch (*p)
            {
            case 'n': w[0] = '\n'; nw = 1; p++; break;
            case 'r': w[0] = '\r'; nw = 1; p++; break;
            case 't': w[0] = '\t'; nw = 1; p++; break;
            case 'b': w[0] = '\b'; nw = 1; p++; break;
            case 'f': w[0] = '\f'; nw = 1; p++; break;
            case 'u':
            {
                char hex[5] = {0};
                memcpy( hex, p + 1, 4 );
                w[0] = (WCHAR)strtoul( hex, NULL, 16 );
                nw = 1;
                p += 5;
                break;
            }
            default: w[0] = (unsigned char)*p; nw = 1; p++; break;
            }
        }
        else
        {
            /* one UTF-8 sequence */
            const char *start = p;
            unsigned char c = *p;
            int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            while (len-- && *p) p++;
            nw = MultiByteToWideChar( CP_UTF8, 0, start, (int)(p - start), w, 2 );
        }
        if (n + nw + 1 > cap)
        {
            cap *= 2;
            buf = HeapReAlloc( GetProcessHeap(), 0, buf, cap * sizeof(WCHAR) );
        }
        memcpy( buf + n, w, nw * sizeof(WCHAR) );
        n += nw;
    }
    buf[n] = 0;
    *out = buf;
    return *p == '"' ? p + 1 : p;
}

static const char *skip_value( const char *p )
{
    int depth = 0;
    p = skip_ws( p );
    if (*p == '"')
    {
        WCHAR *tmp;
        p = parse_string( p, &tmp );
        HeapFree( GetProcessHeap(), 0, tmp );
        return p;
    }
    do
    {
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        else if (*p == '"')
        {
            WCHAR *tmp;
            p = parse_string( p, &tmp );
            HeapFree( GetProcessHeap(), 0, tmp );
            continue;
        }
        if (depth <= 0 && (*p == ',' || *p == '}' || *p == ']')) break;
        p++;
    } while (*p);
    if (depth < 0) return p;
    return p;
}

int json_parse_events( const char *text, struct w2s_event **events )
{
    const char *p = skip_ws( text );
    int count = 0, cap = 8;
    struct w2s_event *list;

    *events = NULL;
    if (*p != '[') return 0;
    list = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(*list) );
    p++;
    for (;;)
    {
        struct w2s_event ev;

        p = skip_ws( p );
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        memset( &ev, 0, sizeof(ev) );
        p++;
        for (;;)
        {
            WCHAR *name;
            char k[16];

            p = skip_ws( p );
            if (*p == ',') { p++; continue; }
            if (*p != '"') break;
            p = parse_string( p, &name );
            WideCharToMultiByte( CP_UTF8, 0, name, -1, k, sizeof(k), NULL, NULL );
            HeapFree( GetProcessHeap(), 0, name );
            p = skip_ws( p );
            if (*p == ':') p++;
            p = skip_ws( p );
            if (!strcmp( k, "t" ) && *p == '"')
            {
                WCHAR *t;
                p = parse_string( p, &t );
                WideCharToMultiByte( CP_UTF8, 0, t, -1, ev.type, sizeof(ev.type), NULL, NULL );
                HeapFree( GetProcessHeap(), 0, t );
            }
            else if (!strcmp( k, "s" ) && *p == '"') p = parse_string( p, &ev.string );
            else if (!strcmp( k, "v" ) && (*p == '-' || (*p >= '0' && *p <= '9')))
            {
                char *end;
                ev.value = strtod( p, &end );
                ev.has_value = TRUE;
                p = end;
            }
            else if (!strcmp( k, "n" ) && *p >= '0' && *p <= '9')
            {
                char *end;
                ev.seq = _strtoui64( p, &end, 10 );
                p = end;
            }
            else if (!strcmp( k, "a" ) && *p == '[')
            {
                int acap = 8;
                ev.array = HeapAlloc( GetProcessHeap(), 0, acap * sizeof(int) );
                p++;
                for (;;)
                {
                    char *end;
                    p = skip_ws( p );
                    if (*p == ',') { p++; continue; }
                    if (*p == ']') { p++; break; }
                    if (ev.array_count == acap)
                    {
                        acap *= 2;
                        ev.array = HeapReAlloc( GetProcessHeap(), 0, ev.array, acap * sizeof(int) );
                    }
                    ev.array[ev.array_count++] = (int)strtol( p, &end, 10 );
                    if (end == p) break;
                    p = end;
                }
            }
            else p = skip_value( p );
        }
        if (*p == '}') p++;
        if (count == cap)
        {
            cap *= 2;
            list = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, list, cap * sizeof(*list) );
        }
        list[count++] = ev;
    }
    *events = list;
    return count;
}

void json_free_events( struct w2s_event *events, int count )
{
    int i;
    for (i = 0; i < count; i++)
    {
        if (events[i].string) HeapFree( GetProcessHeap(), 0, events[i].string );
        if (events[i].array) HeapFree( GetProcessHeap(), 0, events[i].array );
    }
    if (events) HeapFree( GetProcessHeap(), 0, events );
}

/* finds "key": at the top level of a flat object */
static const char *find_key( const char *text, const char *k )
{
    char pat[64];
    const char *p;
    snprintf( pat, sizeof(pat), "\"%s\"", k );
    for (p = strstr( text, pat ); p; p = strstr( p + 1, pat ))
    {
        const char *q = skip_ws( p + strlen( pat ) );
        if (*q == ':') return skip_ws( q + 1 );
    }
    return NULL;
}

BOOL json_get_num( const char *text, const char *k, double *value )
{
    const char *p = find_key( text, k );
    char *end;
    if (!p) return FALSE;
    if (!strncmp( p, "true", 4 )) { *value = 1; return TRUE; }
    if (!strncmp( p, "false", 5 )) { *value = 0; return TRUE; }
    *value = strtod( p, &end );
    return end != p;
}

WCHAR *json_get_str( const char *text, const char *k )
{
    const char *p = find_key( text, k );
    WCHAR *s;
    if (!p || *p != '"') return NULL;
    parse_string( p, &s );
    return s;
}

int json_get_str_array( const char *text, const char *k, WCHAR ***strings )
{
    const char *p = find_key( text, k );
    int count = 0, cap = 8;
    WCHAR **list;

    *strings = NULL;
    if (!p || *p != '[') return 0;
    list = HeapAlloc( GetProcessHeap(), 0, cap * sizeof(*list) );
    p++;
    for (;;)
    {
        p = skip_ws( p );
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        if (count == cap)
        {
            cap *= 2;
            list = HeapReAlloc( GetProcessHeap(), 0, list, cap * sizeof(*list) );
        }
        p = parse_string( p, &list[count++] );
    }
    *strings = list;
    return count;
}

/* "key":[1,2,3] at the top level of a flat object */
int json_get_int_array( const char *text, const char *k, int **values )
{
    const char *p = find_key( text, k );
    int count = 0, cap = 16;
    int *list;

    *values = NULL;
    if (!p || *p != '[') return 0;
    list = HeapAlloc( GetProcessHeap(), 0, cap * sizeof(*list) );
    p++;
    for (;;)
    {
        char *end;
        double v;

        p = skip_ws( p );
        if (*p == ',') { p++; continue; }
        if (*p == ']' || !*p) break;
        v = strtod( p, &end );
        if (end == p) break;
        if (count == cap)
        {
            cap *= 2;
            list = HeapReAlloc( GetProcessHeap(), 0, list, cap * sizeof(*list) );
        }
        list[count++] = (int)v;
        p = end;
    }
    *values = list;
    return count;
}
