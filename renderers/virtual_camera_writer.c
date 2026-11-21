/**
 * Non-blocking virtual-camera frame writer.
 *
 * The AirPlay video pipeline must never wait for a camera consumer.  The
 * appsink callback hands the latest complete NV12 frame to this module, and a
 * small worker thread owns the named pipe.  If no consumer is connected (or
 * the consumer disappears), the frame is dropped and the pipeline continues.
 */

#include "virtual_camera_frame.h"

#if defined(_WIN32) || defined(WIN32)

#include <windows.h>
#include "virtual_camera_writer.h"
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CRITICAL_SECTION lock;
static HANDLE wake_event = NULL;
static HANDLE pipe = NULL;
static HANDLE worker = NULL;
static uint8_t *pending = NULL;
static GstBuffer *pending_buffer = NULL;
static unsigned char *message = NULL;
static size_t pending_size = 0;
static volatile LONG running = 0;
static volatile LONG initialized = 0;
static volatile LONG consumer_connected = 0;
static uint64_t frame_counter = 0;
static int dump_done = 0;

static void connect_pipe_once(void)
{
    if (pipe != NULL) {
        return;
    }
    pipe = CreateFileW(
        UXVC_PIPE_NAME,
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        pipe = NULL;
    } else {
        InterlockedExchange(&consumer_connected, 1);
    }
}

static void disconnect_pipe(void)
{
    if (pipe != NULL) {
        CloseHandle(pipe);
        pipe = NULL;
    }
    InterlockedExchange(&consumer_connected, 0);
}

static DWORD WINAPI writer_thread(LPVOID unused)
{
    (void)unused;
    uint8_t *frame = (uint8_t *)malloc(UXVC_NV12_SIZE);
    if (frame == NULL) {
        return 0;
    }

    while (InterlockedCompareExchange(&running, 1, 1) == 1) {
        WaitForSingleObject(wake_event, 100);

        connect_pipe_once();

        size_t size = 0;
        GstBuffer *buffer = NULL;
        EnterCriticalSection(&lock);
        if (pending_buffer != NULL) {
            buffer = pending_buffer;
            pending_buffer = NULL;
            pending_size = 0;
        } else if (pending_size != 0) {
            memcpy(frame, pending, pending_size);
            size = pending_size;
            pending_size = 0;
        }
        LeaveCriticalSection(&lock);

        if (buffer != NULL) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                if (map.size >= UXVC_NV12_SIZE) {
                    memcpy(frame, map.data, UXVC_NV12_SIZE);
                    size = UXVC_NV12_SIZE;
                }
                gst_buffer_unmap(buffer, &map);
            }
            gst_buffer_unref(buffer);
        }

        if (size == 0) {
            continue;
        }

        if (!dump_done && getenv("UXPLAY_VCAM_DUMP") != NULL) {
            FILE *dump = fopen("F:\\uxplay-windows\\build\\vcam-frame.raw", "wb");
            if (dump != NULL) {
                fwrite(frame, 1, size, dump);
                fclose(dump);
                dump_done = 1;
            }
        }

        if (pipe == NULL) {
            continue;
        }

        uxvc_frame_header *header = (uxvc_frame_header *)message;
        header->magic = UXVC_MAGIC;
        header->version = UXVC_VERSION;
        header->header_size = UXVC_HEADER_SIZE;
        header->width = UXVC_WIDTH;
        header->height = UXVC_HEIGHT;
        header->stride = UXVC_WIDTH;
        header->format = 2;
        header->data_size = (uint32_t)size;
        header->frame_id = ++frame_counter;
        header->timestamp_ms = GetTickCount64();
        memcpy(message + UXVC_HEADER_SIZE, frame, size);

        DWORD written = 0;
        if (!WriteFile(pipe, message, (DWORD)(UXVC_HEADER_SIZE + size), &written, NULL) ||
            written != (DWORD)(UXVC_HEADER_SIZE + size)) {
            disconnect_pipe();
        }
    }

    free(frame);
    return 0;
}

static int ensure_initialized(void)
{
    if (InterlockedCompareExchange(&initialized, 1, 1) == 1) {
        return 1;
    }

    InitializeCriticalSection(&lock);
    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    pending = (uint8_t *)malloc(UXVC_NV12_SIZE);
    message = (unsigned char *)malloc(UXVC_HEADER_SIZE + UXVC_NV12_SIZE);
    if (wake_event == NULL || pending == NULL || message == NULL) {
        free(pending);
        free(message);
        pending = NULL;
        message = NULL;
        if (wake_event != NULL) {
            CloseHandle(wake_event);
            wake_event = NULL;
        }
        DeleteCriticalSection(&lock);
        return 0;
    }

    InterlockedExchange(&running, 1);
    worker = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
    if (worker == NULL) {
        InterlockedExchange(&running, 0);
        free(pending);
        free(message);
        pending = NULL;
        message = NULL;
        CloseHandle(wake_event);
        wake_event = NULL;
        DeleteCriticalSection(&lock);
        return 0;
    }

    InterlockedExchange(&initialized, 1);
    return 1;
}

int virtual_camera_writer_start(void)
{
    return ensure_initialized();
}

int virtual_camera_writer_connected(void)
{
    return InterlockedCompareExchange(&consumer_connected, 1, 1) == 1;
}

void virtual_camera_writer_push_buffer(GstBuffer *buffer)
{
    if (buffer == NULL || !ensure_initialized()) {
        return;
    }
    GstBuffer *copy = gst_buffer_ref(buffer);
    EnterCriticalSection(&lock);
    if (pending_buffer != NULL) {
        gst_buffer_unref(pending_buffer);
    }
    pending_buffer = copy;
    LeaveCriticalSection(&lock);
    SetEvent(wake_event);
}

void virtual_camera_writer_write(const uint8_t *data, size_t size)
{
    if (data == NULL || size < UXVC_NV12_SIZE) {
        return;
    }
    if (!ensure_initialized()) {
        return;
    }

    EnterCriticalSection(&lock);
    memcpy(pending, data, UXVC_NV12_SIZE);
    pending_size = UXVC_NV12_SIZE;
    LeaveCriticalSection(&lock);
    SetEvent(wake_event);
}

void virtual_camera_writer_shutdown(void)
{
    if (InterlockedCompareExchange(&initialized, 1, 1) != 1) {
        return;
    }

    InterlockedExchange(&running, 0);
    if (wake_event != NULL) {
        SetEvent(wake_event);
    }
    if (worker != NULL) {
        CancelSynchronousIo(worker);
    }
    disconnect_pipe();
    if (worker != NULL) {
        DWORD wait = WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
        worker = NULL;
        if (wait != WAIT_OBJECT_0) {
            /* The worker may still be unwinding; do not free its buffers. */
            return;
        }
    }
    if (wake_event != NULL) {
        CloseHandle(wake_event);
        wake_event = NULL;
    }
    EnterCriticalSection(&lock);
    if (pending_buffer != NULL) {
        gst_buffer_unref(pending_buffer);
        pending_buffer = NULL;
    }
    LeaveCriticalSection(&lock);
    free(pending);
    free(message);
    pending = NULL;
    message = NULL;
    DeleteCriticalSection(&lock);
    InterlockedExchange(&initialized, 0);
}

#else

int virtual_camera_writer_start(void)
{
    return 0;
}

int virtual_camera_writer_connected(void)
{
    return 0;
}

void virtual_camera_writer_write(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
}

void virtual_camera_writer_push_buffer(GstBuffer *buffer)
{
    (void)buffer;
}

void virtual_camera_writer_shutdown(void)
{
}

#endif
