/* The Swift functions unixlib.c calls (implemented with @_cdecl in W2SKit). */
#ifndef W2S_UNIX_H
#define W2S_UNIX_H
#include <stdint.h>

int32_t w2s_swift_init(uint32_t version, uint32_t *os_major, uint32_t *os_minor);
uint64_t w2s_swift_control_create(uint64_t host_view, uint64_t window, uint64_t post_wake, uint64_t hwnd,
                                  const char *entry, const char *json, uint32_t json_len);
void w2s_swift_control_update(uint64_t handle, const char *json, uint32_t json_len);
void w2s_swift_control_destroy(uint64_t handle);
void w2s_swift_control_focus(uint64_t handle, uint32_t focused);
uint32_t w2s_swift_pop_events(uint64_t handle, char *buffer, uint32_t size);
uint64_t w2s_swift_request_start(const char *kind, uint64_t window, const char *json, uint32_t json_len);
uint32_t w2s_swift_request_poll(uint64_t id, char *buffer, uint32_t size, uint32_t *done);
uint32_t w2s_swift_control_state(uint64_t handle, uint64_t *version, char *buffer, uint32_t size);
void w2s_swift_request_update(uint64_t id, const char *json, uint32_t json_len);
uint32_t w2s_swift_debug(uint64_t handle, const char *json, uint32_t json_len, char *buffer, uint32_t size);
uint32_t w2s_swift_system_colors(uint64_t *version, uint32_t *colors, uint32_t size, uint32_t *dark);
void w2s_swift_app_state(const char *json, uint32_t len);

#endif
