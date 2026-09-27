/*
 * Win32-to-SwiftUI: what win32swiftui.dll (PE) and win32swiftui.so (unix) share.
 *
 * The escape layouts mirror dlls/winemac.drv/macdrv.h in the MNC wine tree
 * (MACDRV_ESCAPE_W2S_*); keep them in sync. Snapshots and events travel as
 * UTF-8 JSON.
 */
#ifndef W2S_PROTOCOL_H
#define W2S_PROTOCOL_H

#include <stdint.h>

#define W2S_PROTOCOL_VERSION 1

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
    unix_w2s_func_count,
};

struct w2s_init_params
{
    uint32_t version;
    uint32_t ok;
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

/* alerts and panels: started, then polled from a Win32 modal loop */
struct w2s_request_start_params
{
    const char *kind;           /* "alert", "open", "save" */
    uint64_t window;            /* owner NSWindow for a sheet, or 0 */
    const char *json;
    uint32_t json_len;
    uint64_t id;                /* out */
};

struct w2s_request_poll_params
{
    uint64_t id;
    char *buffer;
    uint32_t size;
    uint32_t len;               /* out */
    uint32_t done;              /* out */
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
