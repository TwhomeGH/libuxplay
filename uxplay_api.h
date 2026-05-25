// uxplay_api.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uxplay_media_info_callback_t)(const char *key, const char *value);

void set_uxplay_media_info_callback(uxplay_media_info_callback_t callback);
int start_uxplay(int argc, char *argv[]);
void stop_uxplay();

#ifdef __cplusplus
}
#endif
