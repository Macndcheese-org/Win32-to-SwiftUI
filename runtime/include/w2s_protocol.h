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
 */
#ifndef W2S_PROTOCOL_H
#define W2S_PROTOCOL_H

#include <stdint.h>

#define W2S_PROTOCOL_VERSION 2

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
    const char *entry;
    const char *json;
    uint32_t json_len;
    uint64_t handle;            /* out */
};

struct w2s_control_update_params
{
    uint64_t handle;
    const char *json;
    uint32_t json_len;
};

struct w2s_control_destroy_params
{
    uint64_t handle;
};

struct w2s_control_focus_params
{
    uint64_t handle;
    uint32_t focused;
};

struct w2s_pop_events_params
{
    uint64_t handle;
    char *buffer;
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
    char *buffer;
    uint32_t size;
    uint32_t len;               /* out: needed size, including the terminator */
};

/* alerts and panels: started, then polled from a Win32 modal loop */
struct w2s_request_start_params
{
    const char *kind;           /* "alert", "open", "save" */
    uint64_t window;            /* owner NSWindow for a sheet, or 0 */
    const char *json;
    uint32_t json_len;
    uint64_t id;                /* out */
};

/* done: buffer has the result. Not done and len > 0: buffer has the events
 * the open panel raised meanwhile (a JSON array, like a control's). */
struct w2s_request_poll_params
{
    uint64_t id;
    char *buffer;
    uint32_t size;
    uint32_t len;               /* out */
    uint32_t done;              /* out */
};

/* changes an open alert or panel (a task dialog's progress bar, its buttons) */
struct w2s_request_update_params
{
    uint64_t id;
    const char *json;
    uint32_t json_len;
};

/* test hooks for tests/gallery: {"op":"query"} or {"op":"inject","event":{...}} */
struct w2s_debug_params
{
    uint64_t handle;
    const char *json;
    uint32_t json_len;
    char *buffer;
    uint32_t size;
    uint32_t len;               /* out */
};

#endif /* W2S_PROTOCOL_H */
