// uxplay_api.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uxplay_media_info_callback_t)(const char *key, const char *value);
typedef void (*uxplay_log_callback_t)(int level, const char *message);
typedef void (*uxplay_client_callback_t)(const char *event,
                                         const char *name,
                                         const char *model,
                                         const char *deviceid,
                                         int open_connections);
typedef void (*uxplay_ready_callback_t)(void);

void set_uxplay_media_info_callback(uxplay_media_info_callback_t callback);
void set_uxplay_hud_text(const char *text, int right, int bottom);
void set_uxplay_log_callback(uxplay_log_callback_t callback);
void set_uxplay_client_callback(uxplay_client_callback_t callback);
void set_uxplay_ready_callback(uxplay_ready_callback_t callback);
int start_uxplay(int argc, char *argv[]);
void stop_uxplay();
void reset_uxplay_state();

#ifdef __cplusplus
}
#endif
