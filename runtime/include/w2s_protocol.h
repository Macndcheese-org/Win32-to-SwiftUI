/*
 * Win32-to-SwiftUI: what win32swiftui.dll (PE) and win32swiftui.so (unix) share.
 *
 * The escape layouts mirror dlls/winemac.drv/macdrv.h in the MNC wine tree
 * (MACDRV_ESCAPE_W2S_*); keep them in sync. Snapshots and events travel as
 * UTF-8 JSON.
 *
 * Every native event carries a sequence number "n"; every snapshot carries
 * "ack", the highest one the PE side has taken. A snapshot whose ack is below
 * the last event the native view sent is stale (the user did more since) and
 * the native view ignores it: the PE sends a fresh one once it has applied the
 * events.
 *
 * Every parameter block has the same layout in a 32-bit PE (an i386 app under
 * WoW64) as on the 64-bit unix side: pointers travel as 64-bit integers
 * (W2S_PTR, W2S_UNPTR), and every 64-bit field sits at a multiple of 8, so
 * no alignment rule can move it. The offsets are checked at the end of this
 * file, on both sides, and the unix side serves 32-bit callers with the same
 * functions (__wine_unix_call_wow64_funcs).
 */
#ifndef W2S_PROTOCOL_H
#define W2S_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define W2S_PROTOCOL_VERSION 3

/* a pointer in a parameter block, and back */
#define W2S_PTR(p)      ((uint64_t)(uintptr_t)(p))
#define W2S_UNPTR(v)    ((void *)(uintptr_t)(v))

/* winemac escapes */
#define W2S_ESCAPE_GET_HOST     6792
#define W2S_ESCAPE_RELEASE_HOST 6793
#define W2S_MACDRV_VERSION      1

struct w2s_host_request
{
    uint32_t message;
    uint32_t version;
};

struct w2s_host
{
    uint64_t surface;
    uint64_t view;
    uint64_t window;
    uint64_t post_wake;
};

/* unix calls */
enum w2s_unix_func
{
    unix_w2s_init,
    unix_w2s_control_create,
    unix_w2s_control_update,
    unix_w2s_control_destroy,
    unix_w2s_control_focus,
    unix_w2s_pop_events,
    unix_w2s_request_start,
    unix_w2s_request_poll,
    unix_w2s_debug,
    unix_w2s_control_state,
    unix_w2s_request_update,
    unix_w2s_system_colors,
    unix_w2s_func_count,
};

struct w2s_init_params
{
    uint32_t version;
    uint32_t ok;                /* out */
    uint32_t os_major;          /* out: the running macOS, for layout choices made */
    uint32_t os_minor;          /* out: on the PE side (a property sheet's sidebar) */
};

struct w2s_control_create_params
{
    uint64_t host_view;
    uint64_t window;
    uint64_t post_wake;
    uint64_t hwnd;
    uint64_t entry;             /* const char *: the map entry */
    uint64_t json;              /* const char * */
    uint32_t json_len;
    uint32_t pad;
    uint64_t handle;            /* out */
};

struct w2s_control_update_params
{
    uint64_t handle;
    uint64_t json;              /* const char * */
    uint32_t json_len;
    uint32_t pad;
};

struct w2s_control_destroy_params
{
    uint64_t handle;
};

struct w2s_control_focus_params
{
    uint64_t handle;
    uint32_t focused;
    uint32_t pad;
};

struct w2s_pop_events_params
{
    uint64_t handle;
    uint64_t buffer;            /* char * */
    uint32_t size;
    uint32_t len;               /* out: needed size, including the terminator */
};

/* What a native view publishes for the queries its Win32 control answers from
 * it (the map's `answers`): a JSON object the native side rewrites on the
 * main thread whenever it changes, read here without waiting for the main
 * thread. len is 0 when the caller's version is still current. */
struct w2s_control_state_params
{
    uint64_t handle;
    uint64_t version;           /* in: the version the caller has; out: the current one */
    uint64_t buffer;            /* char * */
    uint32_t size;
    uint32_t len;               /* out: needed size, including the terminator */
};

/* alerts and panels: started, then polled from a Win32 modal loop */
struct w2s_request_start_params
{
    uint64_t kind;              /* const char *: "alert", "open", "save", "color", ... */
    uint64_t window;            /* owner NSWindow for a sheet, or 0 */
    uint64_t json;              /* const char * */
    uint32_t json_len;
    uint32_t pad;
    uint64_t id;                /* out */
};

/* done: buffer has the result. Not done and len > 0: buffer has the events
 * the open panel raised meanwhile (a JSON array, like a control's). */
struct w2s_request_poll_params
{
    uint64_t id;
    uint64_t buffer;            /* char * */
    uint32_t size;
    uint32_t len;               /* out */
    uint32_t done;              /* out */
    uint32_t pad;
};

/* changes an open alert or panel (a task dialog's progress bar, its buttons) */
struct w2s_request_update_params
{
    uint64_t id;
    uint64_t json;              /* const char * */
    uint32_t json_len;
    uint32_t pad;
};

/* wine's system colours from the macOS appearance (map: 70-look.yaml): the
 * table the native side keeps current, read without waiting for the main
 * thread. count is 0 while the caller's version is still current. */
#define W2S_SYSTEM_COLORS 32

struct w2s_system_colors_params
{
    uint64_t version;           /* in: the version the caller has; out: the current one */
    uint32_t colors[W2S_SYSTEM_COLORS]; /* out: COLORREF by COLOR_* index, 0xffffffff: leave it */
    uint32_t count;             /* out: entries in colors, 0 when unchanged */
    uint32_t dark;              /* out: the appearance is a dark one */
};

/* test hooks for tests/gallery: {"op":"query"} or {"op":"inject","event":{...}} */
struct w2s_debug_params
{
    uint64_t handle;
    uint64_t json;              /* const char * */
    uint64_t buffer;            /* char * */
    uint32_t json_len;
    uint32_t size;
    uint32_t len;               /* out */
    uint32_t pad;
};

/* the same offsets for a 32-bit PE and the 64-bit unix side */
#define W2S_CHECK_OFFSET(type, field, offset) \
    _Static_assert(offsetof(struct type, field) == (offset), #type "." #field " moved")
W2S_CHECK_OFFSET(w2s_host, post_wake, 24);
W2S_CHECK_OFFSET(w2s_control_create_params, entry, 32);
W2S_CHECK_OFFSET(w2s_control_create_params, json_len, 48);
W2S_CHECK_OFFSET(w2s_control_create_params, handle, 56);
W2S_CHECK_OFFSET(w2s_control_update_params, json_len, 16);
W2S_CHECK_OFFSET(w2s_control_focus_params, focused, 8);
W2S_CHECK_OFFSET(w2s_pop_events_params, size, 16);
W2S_CHECK_OFFSET(w2s_control_state_params, buffer, 16);
W2S_CHECK_OFFSET(w2s_control_state_params, len, 28);
W2S_CHECK_OFFSET(w2s_request_start_params, json, 16);
W2S_CHECK_OFFSET(w2s_request_start_params, id, 32);
W2S_CHECK_OFFSET(w2s_request_poll_params, done, 24);
W2S_CHECK_OFFSET(w2s_request_update_params, json_len, 16);
W2S_CHECK_OFFSET(w2s_debug_params, buffer, 16);
W2S_CHECK_OFFSET(w2s_debug_params, len, 32);
W2S_CHECK_OFFSET(w2s_system_colors_params, count, 136);

#endif /* W2S_PROTOCOL_H */
