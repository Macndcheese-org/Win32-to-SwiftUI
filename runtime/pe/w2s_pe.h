/*
 * win32swiftui.dll internals.
 */
#ifndef W2S_PE_H
#define W2S_PE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "w2s_protocol.h"

/* wine's unix-call glue, from libwinecrt0.a */
typedef UINT64 unixlib_handle_t;
extern unixlib_handle_t __wine_unixlib_handle;
extern NTSTATUS (WINAPI *__wine_unix_call_dispatcher)( unixlib_handle_t, unsigned int, void * );
extern NTSTATUS WINAPI __wine_init_unix_call(void);

static inline NTSTATUS w2s_call( enum w2s_unix_func code, void *args )
{
    return __wine_unix_call_dispatcher( __wine_unixlib_handle, code, args );
}

/* debug output: WINEDEBUG isn't ours to parse; W2S_DEBUG=1 turns this on */
extern BOOL w2s_debug;
#define TRACE(...) do { if (w2s_debug) { char _b[512]; snprintf( _b, sizeof(_b), "w2s: " __VA_ARGS__ ); OutputDebugStringA( _b ); fputs( _b, stderr ); } } while (0)

/* json.c: a small UTF-8 JSON writer and an event reader */
struct json
{
    char *buf;
    size_t len, cap;
    int depth;
    BOOL first[16];
};

void json_init( struct json *j );
void json_free( struct json *j );
void json_obj_begin( struct json *j );
void json_obj_end( struct json *j );
void json_arr_begin( struct json *j, const char *key );
void json_arr_end( struct json *j );
void json_key_obj_begin( struct json *j, const char *key );
void json_str( struct json *j, const char *key, const WCHAR *value );
void json_str_a( struct json *j, const char *key, const char *value );
void json_int( struct json *j, const char *key, INT64 value );
void json_num( struct json *j, const char *key, double value );
void json_bool( struct json *j, const char *key, BOOL value );
void json_raw( struct json *j, const char *key, const char *raw );

struct w2s_event
{
    char type[32];
    BOOL has_value;
    double value;
    WCHAR *string;          /* HeapAlloc'd, or NULL */
    int *array;             /* HeapAlloc'd, or NULL */
    int array_count;
};

/* parses [{"t":..,"v":..,"s":..,"a":[..]}, ...]; returns the count, events HeapAlloc'd */
int json_parse_events( const char *text, struct w2s_event **events );
void json_free_events( struct w2s_event *events, int count );
/* reads one field of a flat JSON object (for results); NULL/FALSE when missing */
BOOL json_get_num( const char *text, const char *key, double *value );
WCHAR *json_get_str( const char *text, const char *key );          /* HeapAlloc'd */
int json_get_str_array( const char *text, const char *key, WCHAR ***strings ); /* HeapAlloc'd */

WCHAR *strdupW( const WCHAR *s );
char *utf8_from_wide( const WCHAR *s, int len );                   /* HeapAlloc'd */
WCHAR *wide_from_utf8( const char *s, int len );                   /* HeapAlloc'd */

/* main.c */
extern UINT w2s_wake_message;
BOOL w2s_get_host( HWND hwnd, struct w2s_host *host );
void w2s_release_host( HWND hwnd, const struct w2s_host *host );

/* controls.c: one kind per map entry the runtime implements */
struct w2s_control;

struct w2s_kind
{
    const char *entry;      /* map entry id */
    void (*snapshot)( struct w2s_control *ctl, struct json *j );
    void (*apply)( struct w2s_control *ctl, const struct w2s_event *ev );
};

struct w2s_control
{
    HWND hwnd;
    const struct w2s_kind *kind;
    const UINT *state_in;
    unsigned int state_in_count;
    struct w2s_host host;
    UINT64 handle;
    WNDPROC orig;
    char *last;             /* last snapshot sent */
    int applying;
};

const struct w2s_kind *w2s_select_kind( HWND hwnd );
void w2s_common_snapshot( struct w2s_control *ctl, struct json *j );
void w2s_push( struct w2s_control *ctl, BOOL force );
void w2s_notify_parent_command( HWND hwnd, UINT code );
LRESULT w2s_notify_parent( HWND hwnd, UINT code, NMHDR *hdr );

/* dialogs.c */
BOOL w2s_run_request( const char *kind, HWND owner, const char *json, char **result );

#endif
