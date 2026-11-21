#ifndef UXPLAY_VIRTUAL_CAMERA_WRITER_H
#define UXPLAY_VIRTUAL_CAMERA_WRITER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GstBuffer GstBuffer;

void virtual_camera_writer_write(const uint8_t *data, size_t size);
void virtual_camera_writer_push_buffer(GstBuffer *buffer);
void virtual_camera_writer_shutdown(void);
int virtual_camera_writer_start(void);
int virtual_camera_writer_connected(void);

#ifdef __cplusplus
}
#endif

#endif
