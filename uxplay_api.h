// uxplay_api.h
#pragma once

#include <stdint.h>

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
void set_uxplay_hud_text(const char *text, int right, int bottom,
                         int xpad, int ypad);
int get_uxplay_frame_stats(uint64_t *frames, double *age_ms);
int get_uxplay_video_input_stats(uint64_t *frames, double *age_ms);
int get_uxplay_video_decode_stats(uint64_t *frames, double *age_ms);
int get_uxplay_video_pipeline_stats(uint64_t *parser_out,
                                    uint64_t *decoder_in,
                                    uint64_t *decoder_out);
int get_uxplay_video_parser_stats(uint64_t *parser_in,
                                   uint64_t *parser_out);
const char *get_uxplay_video_decoder(void);
const char *get_uxplay_lib_build_info(void);
int get_uxplay_video_overlay_stats(uint64_t *in_frames,
                                   uint64_t *out_frames);
int get_uxplay_audio_stats(uint64_t *received, uint64_t *decoded,
                           uint64_t *resendRequests, uint64_t *emptyResends,
                           uint64_t *bytes);
int get_uxplay_audio_recovery_count(uint64_t *recoveries);
int get_uxplay_audio_pipeline_restarts(uint64_t *restarts);
int get_uxplay_audio_last_gap_ms(uint64_t *gap_ms);
int get_uxplay_audio_output_stats(double *peak, uint64_t *age_ms, uint64_t *pushed);
int get_uxplay_audio_age_ms(uint64_t *age_ms);
void set_uxplay_log_callback(uxplay_log_callback_t callback);
void set_uxplay_client_callback(uxplay_client_callback_t callback);
void set_uxplay_ready_callback(uxplay_ready_callback_t callback);
int start_uxplay(int argc, char *argv[]);
void stop_uxplay();
void reset_uxplay_state();

#ifdef __cplusplus
}
#endif
