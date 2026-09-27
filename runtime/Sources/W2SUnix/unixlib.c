/*
 * win32swiftui.so entry points: wine's unix-call tables for win32swiftui.dll.
 * Each call unpacks its parameter block and hands over to W2SKit (Swift).
 *
 * 64-bit apps call through __wine_unix_call_funcs, 32-bit ones (an i386
 * win32swiftui.dll under WoW64) through __wine_unix_call_wow64_funcs. The
 * parameter blocks have the same layout for both (w2s_protocol.h: pointers
 * are 64-bit integers, 64-bit fields 8-aligned, the offsets asserted), and a
 * 32-bit pointer is a valid address here, so the same functions serve both.
 */
#include <stdint.h>
#include "w2s_protocol.h"
#include "W2SUnix.h"

typedef int32_t NTSTATUS;   /* LONG in wine's unix build: 32 bits */
typedef NTSTATUS (*unixlib_entry_t)(void *args);

static NTSTATUS w2s_init(void *args)
{
    struct w2s_init_params *params = args;
    params->ok = w2s_swift_init(params->version, &params->os_major, &params->os_minor) == 0;
    return 0;
}

static NTSTATUS w2s_control_create(void *args)
{
    struct w2s_control_create_params *params = args;
    params->handle = w2s_swift_control_create(params->host_view, params->window, params->post_wake, params->hwnd,
                                              W2S_UNPTR(params->entry), W2S_UNPTR(params->json), params->json_len);
    return 0;
}

static NTSTATUS w2s_control_update(void *args)
{
    struct w2s_control_update_params *params = args;
    w2s_swift_control_update(params->handle, W2S_UNPTR(params->json), params->json_len);
    return 0;
}

static NTSTATUS w2s_control_destroy(void *args)
{
    struct w2s_control_destroy_params *params = args;
    w2s_swift_control_destroy(params->handle);
    return 0;
}

static NTSTATUS w2s_control_focus(void *args)
{
    struct w2s_control_focus_params *params = args;
    w2s_swift_control_focus(params->handle, params->focused);
    return 0;
}

static NTSTATUS w2s_pop_events(void *args)
{
    struct w2s_pop_events_params *params = args;
    params->len = w2s_swift_pop_events(params->handle, W2S_UNPTR(params->buffer), params->size);
    return 0;
}

static NTSTATUS w2s_request_start(void *args)
{
    struct w2s_request_start_params *params = args;
    params->id = w2s_swift_request_start(W2S_UNPTR(params->kind), params->window, W2S_UNPTR(params->json),
                                         params->json_len);
    return 0;
}

static NTSTATUS w2s_request_poll(void *args)
{
    struct w2s_request_poll_params *params = args;
    params->len = w2s_swift_request_poll(params->id, W2S_UNPTR(params->buffer), params->size, &params->done);
    return 0;
}

static NTSTATUS w2s_debug(void *args)
{
    struct w2s_debug_params *params = args;
    params->len = w2s_swift_debug(params->handle, W2S_UNPTR(params->json), params->json_len,
                                  W2S_UNPTR(params->buffer), params->size);
    return 0;
}

static NTSTATUS w2s_control_state(void *args)
{
    struct w2s_control_state_params *params = args;
    params->len = w2s_swift_control_state(params->handle, &params->version, W2S_UNPTR(params->buffer), params->size);
    return 0;
}

static NTSTATUS w2s_request_update(void *args)
{
    struct w2s_request_update_params *params = args;
    w2s_swift_request_update(params->id, W2S_UNPTR(params->json), params->json_len);
    return 0;
}

__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] =
{
    w2s_init,
    w2s_control_create,
    w2s_control_update,
    w2s_control_destroy,
    w2s_control_focus,
    w2s_pop_events,
    w2s_request_start,
    w2s_request_poll,
    w2s_debug,
    w2s_control_state,
    w2s_request_update,
};

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == unix_w2s_func_count,
               "unix call table out of sync with w2s_protocol.h");

/* 32-bit callers: the same layouts, so the same functions */
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    w2s_init,
    w2s_control_create,
    w2s_control_update,
    w2s_control_destroy,
    w2s_control_focus,
    w2s_pop_events,
    w2s_request_start,
    w2s_request_poll,
    w2s_debug,
    w2s_control_state,
    w2s_request_update,
};

_Static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0]) == unix_w2s_func_count,
               "wow64 call table out of sync with w2s_protocol.h");
