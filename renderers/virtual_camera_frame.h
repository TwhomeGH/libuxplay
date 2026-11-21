/**
 * UxPlay virtual-camera frame protocol (C side).
 *
 * Keep this in sync with virtualcam/protocol/SharedFrame.h.  The media
 * source lives in a separate Windows Frame Server process, so frames cross a
 * named pipe as one message: this header followed by the NV12 payload.
 */

#ifndef UXPLAY_VIRTUAL_CAMERA_FRAME_H
#define UXPLAY_VIRTUAL_CAMERA_FRAME_H

#include <stdint.h>

#define UXVC_MAGIC 0x43565855u /* "UXVC" */
#define UXVC_VERSION 1u
#define UXVC_HEADER_SIZE 48u
#define UXVC_WIDTH 2360u
#define UXVC_HEIGHT 1640u
#define UXVC_NV12_SIZE (UXVC_WIDTH * UXVC_HEIGHT * 3u / 2u)
#define UXVC_PIPE_NAME L"\\\\.\\pipe\\uxplay-vcam-frame-v1"

typedef struct __attribute__((packed)) uxvc_frame_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format; /* 2 = NV12 */
    uint32_t data_size;
    uint64_t frame_id;
    uint64_t timestamp_ms;
} uxvc_frame_header;

#endif /* UXPLAY_VIRTUAL_CAMERA_FRAME_H */
