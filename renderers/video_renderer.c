/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/videooverlay.h>
#include <gst/video/video.h>
#include <cairo.h>
#include "video_renderer.h"
#include "virtual_camera_writer.h"
#include "virtual_camera_frame.h"

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#endif

#define SECOND_IN_NSECS 1000000000UL
#define SECOND_IN_MICROSECS 1000000
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts = 0;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static gchar *hud_last_text = NULL;
static int hud_last_right = -1;
static int hud_last_bottom = -1;
static int hud_last_xpad = -1;
static int hud_last_ypad = -1;
G_LOCK_DEFINE_STATIC(hud);
static volatile uint64_t presented_frame_count = 0;
static volatile uint64_t presented_frame_last_ns = 0;
static volatile uint64_t pushed_frame_count = 0;
static volatile uint64_t pushed_frame_last_ns = 0;
static volatile uint64_t decoded_frame_count = 0;
static volatile uint64_t decoded_frame_last_ns = 0;
static volatile uint64_t parser_out_count = 0;
static volatile uint64_t parser_in_count = 0;
static volatile uint64_t decoder_in_count = 0;
static char selected_decoder_name[128] = "decodebin (auto)";
static volatile uint64_t overlay_in_count = 0;
static volatile uint64_t overlay_out_count = 0;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink = true;
static bool hls_video = false;

typedef enum {
    VIDEO_PACING_ARRIVAL = 0,
    VIDEO_PACING_LOCAL = 1,
    VIDEO_PACING_AIRPLAY = 2
} VideoPacing;
static VideoPacing video_pacing = VIDEO_PACING_ARRIVAL;
static GstClockTime local_pacing_pts = 0;
static guint local_pacing_fps = 60;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration = 0;
static gboolean hls_seek_enabled = FALSE;
static gboolean hls_playing = FALSE;
static gboolean hls_buffer_empty = FALSE;
static gboolean hls_buffer_full = FALSE;
static int type_264 = 0;
static int type_265 = 0;
static int type_hls = 0;
static int type_jpeg = 0;

typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  //GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  //GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

#define NCODECS  3   /* renderers for h264,h265, and jpeg images */

struct video_renderer_s {
    GstElement *appsrc, *pipeline, *textsrc;
    GstElement *hud_overlay;
    GstElement *vcam_sink;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    char *uri;
    gboolean eos;
    gint64 duration;
    gint buffering_level;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static bool virtual_camera_enabled = false;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[]  = "hls";
static char jpeg[] = "jpeg";

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char jpeg_caps[]="image/jpeg";
static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

static GstPadProbeReturn frame_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                        gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        static int sink_pts_logs = 0;
        static GstClockTime last_sink_pts = GST_CLOCK_TIME_NONE;
        if (buffer && sink_pts_logs < 20) {
            const GstClockTime pts = GST_BUFFER_PTS(buffer);
            const GstClockTime duration = GST_BUFFER_DURATION(buffer);
            logger_log(logger, LOGGER_INFO,
                       "video sink input pts=%" GST_TIME_FORMAT
                       " duration=%" GST_TIME_FORMAT " delta=%" GST_TIME_FORMAT,
                       GST_TIME_ARGS(pts), GST_TIME_ARGS(duration),
                       GST_TIME_ARGS(last_sink_pts == GST_CLOCK_TIME_NONE
                                         ? GST_CLOCK_TIME_NONE
                                         : pts - last_sink_pts));
            last_sink_pts = pts;
            ++sink_pts_logs;
        }
        presented_frame_count++;
        presented_frame_last_ns =
            (uint64_t) g_get_monotonic_time() * 1000ULL;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn vcam_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                    gpointer user_data) {
    (void) user_data;
    if (!virtual_camera_writer_connected()) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == NULL) {
        return GST_PAD_PROBE_OK;
    }

    static uint8_t *repacked = NULL;
    if (repacked == NULL) {
        repacked = (uint8_t *) g_malloc(UXVC_NV12_SIZE);
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == NULL) {
        return GST_PAD_PROBE_OK;
    }
    GstVideoInfo vinfo;
    if (!gst_video_info_from_caps(&vinfo, caps) ||
        GST_VIDEO_INFO_FORMAT(&vinfo) != GST_VIDEO_FORMAT_NV12 ||
        GST_VIDEO_INFO_WIDTH(&vinfo) != UXVC_WIDTH ||
        GST_VIDEO_INFO_HEIGHT(&vinfo) != UXVC_HEIGHT) {
        gst_caps_unref(caps);
        return GST_PAD_PROBE_OK;
    }

    GstVideoFrame frame;
    if (gst_video_frame_map(&frame, &vinfo, buffer, GST_MAP_READ)) {
        const uint8_t *y = (const uint8_t *) GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        const int y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        const uint8_t *uv = (const uint8_t *) GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
        const int uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        for (guint row = 0; row < UXVC_HEIGHT; row++) {
            memcpy(repacked + row * UXVC_WIDTH,
                   y + row * y_stride, UXVC_WIDTH);
        }
        for (guint row = 0; row < UXVC_HEIGHT / 2; row++) {
            memcpy(repacked + UXVC_WIDTH * UXVC_HEIGHT + row * UXVC_WIDTH,
                   uv + row * uv_stride, UXVC_WIDTH);
        }
        gst_video_frame_unmap(&frame);
        virtual_camera_writer_write(repacked, UXVC_NV12_SIZE);
    }
    gst_caps_unref(caps);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn decoder_in_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                             gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        decoder_in_count++;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn parser_in_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                            gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        parser_in_count++;
        static unsigned int logged = 0;
        if (logged < 8) {
            GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            if (buffer != NULL) {
                logger_log(logger, LOGGER_INFO,
                           "h264parser input pts=%" GST_TIME_FORMAT
                           " duration=%" GST_TIME_FORMAT
                           " flags=0x%x size=%zu",
                           GST_TIME_ARGS(GST_BUFFER_PTS(buffer)),
                           GST_TIME_ARGS(GST_BUFFER_DURATION(buffer)),
                           GST_BUFFER_FLAGS(buffer), gst_buffer_get_size(buffer));
                ++logged;
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn parser_out_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                             gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        parser_out_count++;
        if (video_pacing == VIDEO_PACING_LOCAL) {
            GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            if (buffer != NULL) {
                const GstClockTime step = GST_SECOND / local_pacing_fps;
                GST_BUFFER_PTS(buffer) = local_pacing_pts;
                GST_BUFFER_DTS(buffer) = local_pacing_pts;
                GST_BUFFER_DURATION(buffer) = step;
                local_pacing_pts += step;
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn decoded_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                          gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        decoded_frame_count++;
        decoded_frame_last_ns =
            (uint64_t) g_get_monotonic_time() * 1000ULL;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn overlay_in_probe_cb(GstPad *pad,
                                             GstPadProbeInfo *info,
                                             gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        overlay_in_count++;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn overlay_out_probe_cb(GstPad *pad,
                                              GstPadProbeInfo *info,
                                              gpointer user_data) {
    (void) pad;
    (void) user_data;
    if (info && (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        overlay_out_count++;
    }
    return GST_PAD_PROBE_OK;
}

void video_renderer_set_virtual_camera_enabled(bool enabled) {
    virtual_camera_enabled = enabled;
}

static GstFlowReturn virtual_camera_new_sample(GstAppSink *sink, gpointer user_data) {
    (void) user_data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == NULL) {
        return GST_FLOW_ERROR;
    }

    if (!virtual_camera_writer_connected()) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer != NULL && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        virtual_camera_writer_write(map.data, map.size);
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

typedef struct {
    GstPad *pad;
} decode_probe_request_t;

static gboolean add_decode_probe_idle(gpointer data) {
    decode_probe_request_t *request = (decode_probe_request_t *) data;
    if (request) {
        if (request->pad) {
            gst_pad_add_probe(request->pad, GST_PAD_PROBE_TYPE_BUFFER,
                              decoded_probe_cb, NULL, NULL);
            gst_object_unref(request->pad);
        }
        g_free(request);
    }
    return G_SOURCE_REMOVE;
}

static void decodebin_pad_added(GstElement *bin, GstPad *pad,
                                gpointer user_data) {
    (void) bin;
    (void) user_data;
    /* Adding a pad probe directly from pad-added can deadlock decodebin's
     * state change; defer it to the main loop instead. */
    decode_probe_request_t *request =
        (decode_probe_request_t *) g_malloc0(sizeof(*request));
    request->pad = (GstPad *) gst_object_ref(pad);
    g_idle_add(add_decode_probe_idle, request);
}

static void decodebin_child_added(GstChildProxy *proxy, GObject *child,
                                  gpointer user_data) {
    (void) proxy;
    (void) user_data;
    if (!GST_IS_ELEMENT(child)) {
        return;
    }
    GstElement *element = GST_ELEMENT(child);
    GstElementFactory *factory = gst_element_get_factory(element);
    if (factory == NULL) {
        return;
    }
    const gchar *klass = gst_element_factory_get_klass(factory);
    const gchar *name = gst_plugin_feature_get_name(
        GST_PLUGIN_FEATURE(factory));
    if (klass != NULL && name != NULL && g_strrstr(klass, "Decoder") != NULL &&
        g_strrstr(klass, "Parser") == NULL &&
        g_strrstr(klass, "Demux") == NULL) {
        g_strlcpy(selected_decoder_name, name,
                  sizeof(selected_decoder_name));
    }
}

const char *video_renderer_get_decoder_name(void) {
    return selected_decoder_name;
}

/* decodebin creates its source pad dynamically; attach a buffer probe to the
 * video output so a stall can be attributed to the decoder or the sink. */
/* Attach a probe to an explicitly configured decoder's output.  decodebin
 * creates its decoder internally, but with -vd the decoder is a named
 * factory and would otherwise have no decoded-frame counter. */
static void attach_explicit_decoder_probe(GstElement *pipeline,
                                          const char *decoder_name) {
    if (decoder_name == NULL || strcmp(decoder_name, "decodebin") == 0) {
        return;
    }
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            const gchar *name = factory
                ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
                : NULL;
            if (name != NULL && strcmp(name, decoder_name) == 0) {
                GstPad *srcpad = gst_element_get_static_pad(element, "src");
                if (srcpad) {
                    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
                                      decoded_probe_cb, NULL, NULL);
                    gst_object_unref(srcpad);
                }
                GstPad *sinkpad = gst_element_get_static_pad(element, "sink");
                if (sinkpad) {
                    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER,
                                      decoder_in_probe_cb, NULL, NULL);
                    gst_object_unref(sinkpad);
                }
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    gst_iterator_free(it);
}

static void attach_parser_probe(GstElement *pipeline,
                                const char *parser_name) {
    if (parser_name == NULL || *parser_name == '\0') {
        return;
    }
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            const gchar *name = factory
                ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
                : NULL;
            if (name != NULL && strcmp(name, parser_name) == 0) {
                GstPad *srcpad = gst_element_get_static_pad(element, "src");
                if (srcpad) {
                    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
                                      parser_out_probe_cb, NULL, NULL);
                    gst_object_unref(srcpad);
                }
                GstPad *sinkpad = gst_element_get_static_pad(element, "sink");
                if (sinkpad) {
                    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER,
                                      parser_in_probe_cb, NULL, NULL);
                    gst_object_unref(sinkpad);
                }
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    gst_iterator_free(it);
}

static void attach_decodebin_probe(GstElement *pipeline) {
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            const gchar *name = factory
                ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
                : NULL;
            if (name && g_str_has_prefix(name, "decodebin")) {
                g_signal_connect(element, "pad-added",
                                 G_CALLBACK(decodebin_pad_added), NULL);
                g_signal_connect(element, "child-added",
                                 G_CALLBACK(decodebin_child_added), NULL);
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

void video_renderer_get_frame_stats(uint64_t *frames, double *age_ms) {
    if (frames) {
        *frames = presented_frame_count;
    }
    if (age_ms) {
        const uint64_t now = (uint64_t) g_get_monotonic_time() * 1000ULL;
        *age_ms = presented_frame_last_ns && now > presented_frame_last_ns
                      ? (double) (now - presented_frame_last_ns) / 1000000.0
                      : -1.0;
    }
}

void video_renderer_get_input_stats(uint64_t *frames, double *age_ms) {
    if (frames) {
        *frames = pushed_frame_count;
    }
    if (age_ms) {
        const uint64_t now = (uint64_t) g_get_monotonic_time() * 1000ULL;
        *age_ms = pushed_frame_last_ns && now > pushed_frame_last_ns
                      ? (double) (now - pushed_frame_last_ns) / 1000000.0
                      : -1.0;
    }
}

void video_renderer_get_decode_stats(uint64_t *frames, double *age_ms) {
    if (frames) {
        *frames = decoded_frame_count;
    }
    if (age_ms) {
        const uint64_t now = (uint64_t) g_get_monotonic_time() * 1000ULL;
        *age_ms = decoded_frame_last_ns && now > decoded_frame_last_ns
                      ? (double) (now - decoded_frame_last_ns) / 1000000.0
                      : -1.0;
    }
}

void video_renderer_get_pipeline_stats(uint64_t *parser_out,
                                       uint64_t *decoder_in,
                                       uint64_t *decoder_out) {
    if (parser_out) {
        *parser_out = parser_out_count;
    }
    if (decoder_in) {
        *decoder_in = decoder_in_count;
    }
    if (decoder_out) {
        *decoder_out = decoded_frame_count;
    }
}

void video_renderer_get_parser_stats(uint64_t *parser_in,
                                     uint64_t *parser_out) {
    if (parser_in) {
        *parser_in = parser_in_count;
    }
    if (parser_out) {
        *parser_out = parser_out_count;
    }
}

void video_renderer_get_overlay_stats(uint64_t *in_frames,
                                      uint64_t *out_frames) {
    if (in_frames) {
        *in_frames = overlay_in_count;
    }
    if (out_frames) {
        *out_frames = overlay_out_count;
    }
}

static void hud_plain_text(const char *in, char *out, size_t capacity) {
    size_t used = 0;
    int in_tag = 0;
    if (!out || capacity == 0) {
        return;
    }
    for (; in && *in && used + 1 < capacity; in++) {
        if (*in == '<') {
            in_tag = 1;
            continue;
        }
        if (*in == '>' && in_tag) {
            in_tag = 0;
            continue;
        }
        if (!in_tag) {
            out[used++] = *in;
        }
    }
    out[used] = '\0';
}

static void hud_draw_cb(GstElement *overlay, cairo_t *cr, guint64 timestamp,
                        guint64 duration, gpointer user_data) {
    (void) overlay;
    (void) timestamp;
    (void) duration;
    (void) user_data;

    G_LOCK(hud);
    gchar *text = g_strdup(hud_last_text ? hud_last_text : "");
    const int right = hud_last_right == 1;
    const int bottom = hud_last_bottom == 1;
    const int xpad = hud_last_xpad >= 0 ? hud_last_xpad : 8;
    const int ypad = hud_last_ypad >= 0 ? hud_last_ypad : 72;
    G_UNLOCK(hud);

    if (!text || text[0] == '\0') {
        g_free(text);
        return;
    }

    char plain[4096];
    hud_plain_text(text, plain, sizeof(plain));
    g_free(text);
    gchar **lines = g_strsplit(plain, "\n", -1);
    int count = 0;
    while (lines && lines[count]) {
        count++;
    }
    if (count == 0) {
        g_strfreev(lines);
        return;
    }

    double clip_x1 = 0, clip_y1 = 0, clip_x2 = 0, clip_y2 = 0;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
    const double canvas_w = clip_x2 - clip_x1;
    const double canvas_h = clip_y2 - clip_y1;
    const double font_size = 14.0;
    const double line_h = 18.0;
    const double pad = 8.0;

    cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    double text_w = 0;
    for (int i = 0; i < count; i++) {
        cairo_text_extents_t extents;
        cairo_text_extents(cr, lines[i], &extents);
        if (extents.width > text_w) {
            text_w = extents.width;
        }
    }

    const double box_w = text_w + pad * 2;
    const double box_h = count * line_h + pad * 2;
    const double box_x = right ? canvas_w - xpad - box_w : xpad;
    const double box_y = bottom ? canvas_h - ypad - box_h : ypad;

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
    cairo_rectangle(cr, box_x, box_y, box_w, box_h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.92, 0.96, 1.0);
    for (int i = 0; i < count; i++) {
        cairo_move_to(cr, box_x + pad, box_y + pad + font_size + i * line_h);
        cairo_show_text(cr, lines[i]);
    }
    cairo_restore(cr);
    g_strfreev(lines);
}

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
    /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token = NULL;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
            g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
            gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

#if defined(_WIN32) || defined(WIN32)
/* First-use initialization of the D3D11 video sink has been observed to
 * leave the first mirror session stuck after one frame. Run a one-frame
 * hidden pipeline once per process so the real session uses a warmed device. */
static void warm_up_d3d11_sink(void) {
    static bool warmed = false;
    if (warmed) {
        return;
    }
    warmed = true;

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 2, 2,
                                nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (!hwnd) {
        return;
    }

    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *conv = gst_element_factory_make("videoconvert", nullptr);
    GstElement *sink = gst_element_factory_make("d3d11videosink", nullptr);
    if (!pipeline || !src || !conv || !sink) {
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (conv) gst_object_unref(conv);
        if (sink) gst_object_unref(sink);
        DestroyWindow(hwnd);
        return;
    }

    g_object_set(src, "num-buffers", 1, "is-live", FALSE, NULL);
    gst_bin_add_many(GST_BIN(pipeline), src, conv, sink, NULL);
    gst_element_link_many(src, conv, sink, NULL);
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),
                                        (guintptr) hwnd);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    if (bus) {
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 2 * GST_SECOND,
            GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message) {
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    DestroyWindow(hwnd);
}
#endif

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char * rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool rtp = (bool) strlen(rtp_pipeline);
    hls_video = (uri != NULL);
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    const char *pacing_env = g_getenv("UXPLAY_VIDEO_PACING");
    if (pacing_env != NULL && strcmp(pacing_env, "local") == 0) {
        video_pacing = VIDEO_PACING_LOCAL;
    } else if (pacing_env != NULL && strcmp(pacing_env, "airplay") == 0) {
        video_pacing = VIDEO_PACING_AIRPLAY;
    } else {
        video_pacing = VIDEO_PACING_ARRIVAL;
    }
    local_pacing_pts = 0;
    local_pacing_fps = 60;
    logger_log(logger, LOGGER_INFO, "Video pacing mode: %s",
               video_pacing == VIDEO_PACING_LOCAL ? "local"
               : video_pacing == VIDEO_PACING_AIRPLAY ? "airplay"
               : "arrival");
#if defined(_WIN32) || defined(WIN32)
    warm_up_d3d11_sink();
#endif
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_empty = FALSE;
    type_hls = -1;
    type_264 = -1;
    type_265 = -1;
    type_jpeg = -1;
    presented_frame_count = 0;
    presented_frame_last_ns = 0;
    pushed_frame_count = 0;
    pushed_frame_last_ns = 0;
    decoded_frame_count = 0;
    decoded_frame_last_ns = 0;
    parser_out_count = 0;
    parser_in_count = 0;
    decoder_in_count = 0;
    if (decoder != NULL && strcmp(decoder, "decodebin") != 0) {
        g_strlcpy(selected_decoder_name, decoder,
                  sizeof(selected_decoder_name));
    } else {
        g_strlcpy(selected_decoder_name, "decodebin (auto)",
                  sizeof(selected_decoder_name));
    }
    overlay_in_count = 0;
    overlay_out_count = 0;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;
    n_renderers = 1;
    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264/h265 mirror-mode and jpeg
     * audio-mode renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265, jpeg and hls  */  
    if (hls_video) {
        type_hls = 0;
    } else {
        type_264 = 0;
        if (h265_support) {
            type_265 = n_renderers++;
        }
        if (coverart_support) {
            type_jpeg = n_renderers++;
        }
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 3);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        renderer_type[i]->appsrc = NULL;
        renderer_type[i]->textsrc = NULL;
        renderer_type[i]->uri = NULL;
        renderer_type[i]->eos = FALSE;
        if (hls_video) {
            renderer_type[i]->uri = (char *) calloc(strlen(uri) + 1, sizeof(char));
            memcpy(renderer_type[i]->uri, uri, strlen(uri));
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);	    
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->codec = hls;
            /* if we are not using an autovideosink, build a videosink based on the string "videosink" */
            if (!auto_videosink) { 
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);  
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }
            gint flags = 0;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags |= GST_PLAY_FLAG_DOWNLOAD;
            flags |= GST_PLAY_FLAG_BUFFERING;    // set by default in playbin3, but not in playbin2; is it needed?
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            //g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            bool jpeg_pipeline = false;
            if (i == type_264) {
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
            } else if (i == type_265) {
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
            } else if (i == type_jpeg) {
                jpeg_pipeline = true;
                renderer_type[i]->codec = jpeg;
                caps = gst_caps_from_string(jpeg_caps);
            } else {
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            if (jpeg_pipeline) {
                g_string_append(launch, "jpegdec ");
            } else {
                g_string_append(launch, "queue ! ");
                g_string_append(launch, parser);
                g_string_append(launch, " ! ");
                if (!rtp) {
                    g_string_append(launch, decoder);
                } else {
                    g_string_append(launch, "rtph264pay ");
                    g_string_append(launch, rtp_pipeline);
                }
            }
            if (!rtp || jpeg_pipeline) {
                g_string_append(launch, " ! ");
                append_videoflip(launch, &videoflip[0], &videoflip[1]);
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                g_string_append(launch, "videoscale ! ");
                if (jpeg_pipeline) {
                    g_string_append(launch, " imagefreeze allow-replace=TRUE ! textoverlay name=metadata_overlay ! ");
                } else if (virtual_camera_enabled) {
                    /* Keep the virtual camera on its own tee branch. The main
                     * preview branch and the virtual-camera branch each have
                     * their own queue, so a slow camera consumer can drop its
                     * own frames without stalling the d3d11 sink. */
                    g_string_append(launch, "tee name=vcam_tee vcam_tee. ! queue ! ");
                } else {
                    /* HUD is being moved out of the video pipeline. cairooverlay
                     * keeps the video moving but renders every frame and forces a
                     * BGRA copy, which is too expensive at 2360x1640. Keep the
                     * mirror pipeline free of per-frame overlays. */
                }
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
                if (video_pacing == VIDEO_PACING_LOCAL && !jpeg_pipeline) {
                    /* Local mode: the sink uses a synthetic 60 Hz timeline; the
                     * pad probe stamps decoded frames on arrival. */
                    g_string_append(launch, " sync=true");
                    sync = false;
                } else if (video_pacing == VIDEO_PACING_AIRPLAY
                           && video_sync && !jpeg_pipeline) {
                    g_string_append(launch, " sync=true");
                    sync = true;
                } else {
                    g_string_append(launch, " sync=false");
                    sync = false;
                }
                if (!jpeg_pipeline && virtual_camera_enabled) {
                    g_string_append(launch,
                        " vcam_tee. ! queue leaky=downstream "
                        "max-size-buffers=4 max-size-time=100000000 "
                        "max-size-bytes=0 ! "
                        "videoconvert ! videoscale ! ");
                    g_string_append_printf(launch,
                        "video/x-raw(memory:SystemMemory),format=NV12,"
                        "width=%u,height=%u ! "
                        "fakesink name=vcam_sink sync=false async=false",
                        UXVC_WIDTH, UXVC_HEIGHT);
                }
            }
            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
                if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);
            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);
            if (!hls_video && !jpeg_pipeline) {
                attach_decodebin_probe(renderer_type[i]->pipeline);
                attach_explicit_decoder_probe(renderer_type[i]->pipeline,
                                              decoder);
                attach_parser_probe(renderer_type[i]->pipeline, parser);
            }
            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            if (!hls_video && !jpeg_pipeline) {
                GString *sink_name = g_string_new(videosink);
                g_string_append(sink_name, "_");
                g_string_append(sink_name, renderer_type[i]->codec);
                GstElement *sink = gst_bin_get_by_name(
                    GST_BIN(renderer_type[i]->pipeline), sink_name->str);
                g_string_free(sink_name, TRUE);
                if (sink) {
                    GstPad *sinkpad = gst_element_get_static_pad(sink, "sink");
                    if (sinkpad) {
                        gst_pad_add_probe(sinkpad,
                                          GST_PAD_PROBE_TYPE_BUFFER,
                                          frame_probe_cb, NULL, NULL);
                        gst_object_unref(sinkpad);
                    }
                    gst_object_unref(sink);
                }
            }
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
            gst_object_unref(clock);
            if (jpeg_pipeline) {
                 renderer_type[i]->textsrc = gst_bin_get_by_name(GST_BIN(renderer_type[i]->pipeline), "metadata_overlay");
                 g_object_set(G_OBJECT(renderer_type[i]->textsrc), "text", "", "shaded-background", TRUE, "font-desc", "Sans, 16",  NULL);
            } else {
                 renderer_type[i]->vcam_sink = gst_bin_get_by_name(
                     GST_BIN(renderer_type[i]->pipeline), "vcam_sink");
                 if (renderer_type[i]->vcam_sink) {
                     GstPad *vcampad = gst_element_get_static_pad(
                         renderer_type[i]->vcam_sink, "sink");
                     if (vcampad) {
                         gst_pad_add_probe(vcampad,
                                           GST_PAD_PROBE_TYPE_BUFFER,
                                           vcam_probe_cb, NULL, NULL);
                         gst_object_unref(vcampad);
                     }
                 }
                 if (virtual_camera_enabled) {
                     virtual_camera_writer_start();
                     logger_log(logger, LOGGER_INFO,
                                "Virtual camera output enabled (%ux%u NV12, source frame rate)",
                                UXVC_WIDTH, UXVC_HEIGHT);
                 }
                 renderer_type[i]->hud_overlay = gst_bin_get_by_name(
                     GST_BIN(renderer_type[i]->pipeline), "hud_overlay");
                 if (renderer_type[i]->hud_overlay) {
                     g_signal_connect(renderer_type[i]->hud_overlay, "draw",
                                      G_CALLBACK(hud_draw_cb), NULL);
                     GstPad *overlay_in = gst_element_get_static_pad(
                         renderer_type[i]->hud_overlay, "sink");
                     GstPad *overlay_out = gst_element_get_static_pad(
                         renderer_type[i]->hud_overlay, "src");
                     if (overlay_in) {
                         gst_pad_add_probe(overlay_in,
                                           GST_PAD_PROBE_TYPE_BUFFER,
                                           overlay_in_probe_cb, NULL, NULL);
                         gst_object_unref(overlay_in);
                     }
                     if (overlay_out) {
                         gst_pad_add_probe(overlay_out,
                                           GST_PAD_PROBE_TYPE_BUFFER,
                                           overlay_out_probe_cb, NULL, NULL);
                         gst_object_unref(overlay_out);
                     }
                 }
                 G_LOCK(hud);
                 g_free(hud_last_text);
                 hud_last_text = NULL;
                 hud_last_right = -1;
                 hud_last_bottom = -1;
                 hud_last_xpad = -1;
                 hud_last_ypad = -1;
                 G_UNLOCK(hud);
             }
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
        /* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
        char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[0]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);	
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
            if (hls_video && i == 0) {
                renderer = renderer_type[i];
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            logger_log(logger, LOGGER_INFO, "\nPerhaps your GStreamer installation is missing some required plugins,"
                       "\nor your choices of video options (-vs -vd -vc -fs etc.) are incompatible on"
                       "\nthis computer architecture.  (An example: kmssink with fullscreen option -fs"
                       "\nmay work on some systems, but fail on others)");
            exit(1);
        }
    }
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
    logger_log(logger, LOGGER_DEBUG, "video renderer pause: %s", gst_element_state_change_return_get_name(ret));
}

void video_renderer_resume() {
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

void video_renderer_start() {
    GstState state;
    const gchar *state_name = NULL;
    if (hls_video) {
        g_object_set (G_OBJECT (renderer->pipeline), "uri", renderer->uri, NULL);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name = gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
    /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    /* Keep the inactive codec pipeline out of PAUSED: entering PAUSED creates
     * a video window for both codecs, and the inactive window can steal the
     * handle/z-order from the codec the client actually selected. Only the
     * selected pipeline is promoted to PLAYING when the codec is known. */
    for (int i = 0; i < n_renderers; i++) {
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name = gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d %p state %s", i, renderer_type[i], state_name);
    }
    renderer = NULL;
    first_packet = true;
    local_pacing_pts = 0;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
}

/* use this to cycle the jpeg renderer to remove expired coverart when no new coverart has replaced it */
int video_renderer_cycle() {
    if (!renderer || !strstr(renderer->codec, jpeg)) {
        return -1;
    }
    GstState state, pending_state, target_state;
    GstStateChangeReturn ret;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    logger_log(logger, LOGGER_DEBUG, "renderer_cycle renderer %p: initial pipeline state is %s", renderer,
               gst_element_state_get_name(state));

    for (int i = 0 ; i < 2; i++) {
        int count = 0;
        if (i == 0 ) {
            target_state = GST_STATE_NULL;
            video_renderer_stop();
        } else {
            target_state = GST_STATE_PLAYING;
            gst_element_set_state (renderer->pipeline, target_state);
        }
        while (state != target_state) {
            ret = gst_element_get_state(renderer->pipeline, &state, &pending_state, 1000 * GST_MSECOND);
            if (ret == GST_STATE_CHANGE_SUCCESS) {
                logger_log(logger, LOGGER_DEBUG, "current pipeline state is %s", gst_element_state_get_name(state));
                if (pending_state != GST_STATE_VOID_PENDING) {
                    logger_log(logger, LOGGER_DEBUG, "pending pipeline state is %s", gst_element_state_get_name(pending_state));
                }
            } else if (ret == GST_STATE_CHANGE_FAILURE) {
                logger_log(logger, LOGGER_ERR, "pipeline %s: state change to %s failed", renderer->codec, gst_element_state_get_name(target_state));
                count++;
                if (count > 10) {
                    return -1;
                }
            } else if (ret == GST_STATE_CHANGE_ASYNC) {
                logger_log(logger, LOGGER_DEBUG, "state change to %s is asynchronous, waiting for completion ...",
                           gst_element_state_get_name(target_state));
            }
        }
    }
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (type_jpeg == -1) {
        return;
    }
    if (renderer && !strcmp(renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
	g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
    }  
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (gst_video_pipeline_base_time == GST_CLOCK_TIME_NONE ||
            gst_video_pipeline_base_time == 0) {
            GstClockTime pipeline_base =
                renderer && renderer->appsrc
                    ? gst_element_get_base_time(renderer->appsrc)
                    : GST_CLOCK_TIME_NONE;
            if (pipeline_base != GST_CLOCK_TIME_NONE && pipeline_base != 0) {
                gst_video_pipeline_base_time = pipeline_base;
            } else {
                /* A missing pipeline base time is not a reason to use the
                 * first video PTS as the origin: the first AirPlay frame can
                 * carry an absolute NTP-like timestamp while later frames are
                 * already relative.  Subtracting that first PTS makes every
                 * later frame look older than the base and feeds the sink an
                 * enormous PTS, which makes d3d11/d3d12 backpressure the whole
                 * pipeline.  Keep relative PTS values and clamp only an
                 * obviously absolute first timestamp below. */
                gst_video_pipeline_base_time = 0;
                logger_log(logger, LOGGER_INFO,
                           "no valid video pipeline base time; using relative PTS");
            }
        }
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_DEBUG,
                       "video PTS older than pipeline base; clamping to 0\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS,
                       ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            pts = 0;
        }
        /* The first AirPlay buffer is sometimes stamped with absolute NTP
         * time; do not let that single value make the sink wait for hours. */
        if (pts > 3600 * GST_SECOND) {
            logger_log(logger, LOGGER_DEBUG,
                       "clamping absolute first video PTS to 0");
            pts = 0;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        if (!renderer || !(renderer->appsrc)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        const GstFlowReturn push_result =
            gst_app_src_push_buffer(GST_APP_SRC(renderer->appsrc), buffer);
        if (push_result == GST_FLOW_OK) {
            pushed_frame_count++;
            pushed_frame_last_ns =
                (uint64_t) g_get_monotonic_time() * 1000ULL;
        } else {
            static unsigned int push_error_logs = 0;
            if (push_error_logs < 10) {
                logger_log(logger, LOGGER_WARNING,
                           "appsrc push_buffer returned %s",
                           gst_flow_get_name(push_result));
                ++push_error_logs;
            }
        }
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
            if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
    return 0;
}

void video_renderer_flush() {
}

void video_renderer_hls_ready() {
    GstState state;
    GstStateChangeReturn ret;
    if (renderer && hls_video) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_hls_ready");
        ret = gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                   gst_element_state_change_return_get_name(ret));
        gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
    }
}

void video_renderer_set_hud_text(const char *text, bool right, bool bottom,
                                 int xpad, int ypad) {
    if (!text) {
        text = "";
    }
    G_LOCK(hud);
    if (hud_last_text && !strcmp(text, hud_last_text)
        && hud_last_right == (right ? 1 : 0)
        && hud_last_bottom == (bottom ? 1 : 0)
        && hud_last_xpad == xpad && hud_last_ypad == ypad) {
        G_UNLOCK(hud);
        return;
    }
    g_free(hud_last_text);
    hud_last_text = g_strdup(text);
    hud_last_right = right ? 1 : 0;
    hud_last_bottom = bottom ? 1 : 0;
    hud_last_xpad = xpad;
    hud_last_ypad = ypad;
    G_UNLOCK(hud);
}

void video_renderer_stop() {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
}

void video_renderer_set_device_model(const char *model, const char *name) {
    // Device frame not supported in GStreamer renderer
    (void)model;
    (void)name;
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    // Track metadata display superimposed on coverart is now supported in GStreamer renderer
    GString *metadata = g_string_new("");
    if (artist) {
        g_string_append(metadata, artist);
    }
    if (artist && title) {
        g_string_append(metadata, ": ");
    }
    if (title) {
        g_string_append(metadata, "\"");
        g_string_append(metadata, title);
        g_string_append(metadata, "\"");
    }
    
    g_string_replace (metadata, "&", "&amp;", 0);   //fix pango problem with "&" in text
    if (renderer && renderer->textsrc && (artist || title)) {
        g_object_set(G_OBJECT(renderer->textsrc), "text", metadata->str, NULL);
    }
    g_string_free(metadata, TRUE);
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p codec=%s ", renderer, renderer->codec);
        GstState state;
        GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                       gst_element_state_change_return_get_name(ret));
            gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
            logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        }
        if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
            renderer->appsrc = NULL;
        }
        if (renderer->textsrc) {
            gst_object_unref (renderer->textsrc);
            renderer->textsrc = NULL;
        }
        if (renderer->hud_overlay) {
            gst_object_unref (renderer->hud_overlay);
            renderer->hud_overlay = NULL;
        }
        if (renderer->vcam_sink) {
            gst_object_unref (renderer->vcam_sink);
            renderer->vcam_sink = NULL;
        }
        gst_object_unref(renderer->bus);
        gst_object_unref(renderer->pipeline);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window){
	  // free_X11_Display(renderer->gst_window);   without this, a memory leak; with it, a coredump
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        if (renderer->uri) {
            free(renderer->uri);
        }
        free (renderer);
        renderer = NULL;
        logger_log(logger, LOGGER_DEBUG,"renderer destroyed\n");	
    }
}

void video_renderer_destroy() {
    G_LOCK(hud);
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
            renderer_type[i] = NULL;
        }
    }
    n_renderers = 0;
    virtual_camera_writer_shutdown();
    presented_frame_count = 0;
    presented_frame_last_ns = 0;
    pushed_frame_count = 0;
    pushed_frame_last_ns = 0;
    decoded_frame_count = 0;
    decoded_frame_last_ns = 0;
    overlay_in_count = 0;
    overlay_out_count = 0;
    g_free(hud_last_text);
    hud_last_text = NULL;
    hud_last_right = -1;
    hud_last_bottom = -1;
    hud_last_xpad = -1;
    hud_last_ypad = -1;
    G_UNLOCK(hud);
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
    switch (type) {
    case GST_STREAM_STATUS_TYPE_CREATE:
        strncpy(name, "CREATE", len);
        return;
    case GST_STREAM_STATUS_TYPE_ENTER:
        strncpy(name, "ENTER", len);
        return;
    case GST_STREAM_STATUS_TYPE_LEAVE:
        strncpy(name, "LEAVE", len);
        return;
    case GST_STREAM_STATUS_TYPE_DESTROY:
        strncpy(name, "DESTROY", len);
        return;
    case GST_STREAM_STATUS_TYPE_START:
        strncpy(name, "START", len);
        return;
    case GST_STREAM_STATUS_TYPE_PAUSE:
        strncpy(name, "PAUSE", len);
        return;
    case GST_STREAM_STATUS_TYPE_STOP:
        strncpy(name, "STOP", len);
        return;
    default:
        strncpy(name, "", len);
        return;
    }
}

static void hls_video_seek_to_start_position(GstElement *pipeline) {
    if (hls_requested_start_position && hls_seek_enabled && hls_requested_start_position >= hls_seek_start
        && hls_requested_start_position  <= hls_seek_end) {
        g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
        if (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
				 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
            hls_requested_start_position = 0;
        } else {
            g_print("*** seek to requested_start_position failed\n"); 
        }
    } 
}

static gboolean gstreamer_video_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    video_renderer_t *renderer = renderer_type[type];

    gint64 pos = -1;
    if (hls_video) {
        gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos);
        if (hls_requested_start_position && pos >= hls_requested_start_position) {
            hls_requested_start_position = 0;
        }
    }

    if (logger_debug) {
        gchar *name = NULL;
        GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
        if (name) {
            g_free(name);
        }
    }

    if (hls_video && !GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }

    if (hls_requested_start_position && hls_seek_enabled) {
        hls_video_seek_to_start_position(renderer->pipeline);
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            hls_buffer_empty = TRUE;
            hls_buffer_full = FALSE;
            if (percent > 0) {
                hls_buffer_empty = FALSE;
                renderer->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
                if (percent < 100) {
                    gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
                } else {
                    hls_buffer_full = TRUE;
                    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gboolean flushing = FALSE;
        gboolean closed_window = FALSE;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (video): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (strstr(err->message, "Output window was closed")) {
            closed_window = TRUE;
        }
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
        g_error_free (err);
        g_free (debug);
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        if (!hls_video || closed_window) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (video)");
        if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            renderer->eos = TRUE;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if (logger_debug) {
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            } 
            if (new_state != GST_STATE_PLAYING) {
                hls_playing = FALSE;
                break;
            } 
            hls_playing = TRUE;
            GstQuery *query = NULL;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);

            if (hls_requested_start_position && hls_seek_enabled) {
                hls_video_seek_to_start_position(renderer->pipeline);
            }

        }
        if (renderer->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message), renderer->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer->id + 1,
                               renderer->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer->autovideo = false;
                }
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer->gst_window && renderer->gst_window->window) {
            GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event (message, &event)) {
                    GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                    const gchar *key = NULL;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;
                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref (event);
                }
            }
        }
        break;
#endif
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

static gboolean log_pipeline_state_cb(gpointer data) {
    GstElement *pipeline = GST_ELEMENT(data);
    GstState state = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn ret =
        gst_element_get_state(pipeline, &state, &pending, 0);
    logger_log(logger, LOGGER_INFO,
               "deferred video pipeline state: %s pending=%s ret=%s",
               gst_element_state_get_name(state),
               gst_element_state_get_name(pending),
               gst_element_state_change_return_get_name(ret));
    if (state != GST_STATE_PLAYING) {
        logger_log(logger, LOGGER_INFO,
                   "deferred video pipeline still not PLAYING; retrying");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    } else if (renderer && renderer->pipeline == pipeline && renderer->appsrc) {
        /* The initial get_state() may return ASYNC while the sink is still
         * prerolling.  Only use the base time once the pipeline is really
         * PLAYING; otherwise the first session receives a stale base time and
         * the sink can run unthrottled. */
        if (gst_video_pipeline_base_time == GST_CLOCK_TIME_NONE ||
            gst_video_pipeline_base_time == 0) {
            gst_video_pipeline_base_time =
                gst_element_get_base_time(renderer->appsrc);
            logger_log(logger, LOGGER_DEBUG,
                       "deferred video pipeline base time: %llu",
                       (unsigned long long) gst_video_pipeline_base_time);
        }
    }
    gst_object_unref(pipeline);
    return G_SOURCE_REMOVE;
}

int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    g_assert(!hls_video);
    if (video_is_jpeg) {
        g_assert(type_jpeg != -1);
        renderer_used = renderer_type[type_jpeg];
    } else {
        if (video_is_h265) {
            if (type_265 == -1) {
                logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
                return -1;
            }
            renderer_used = renderer_type[type_265];
        } else {
            g_assert(type_264 != -1);
            renderer_used = renderer_type[type_264];
        }
    }
    if (renderer_used == NULL) {
        return -1;
    } else if (renderer_used == renderer) {
        return 0;
    } else if (renderer) {
        return -1;
    }
    renderer = renderer_used;
    /* Tear down the inactive codec pipeline before starting the active one.
     * Starting both windows first and destroying one during the first frame
     * can leave the D3D11 sink stuck after a single presented frame. */
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i] == renderer) {
            continue;
        }
        if (renderer_type[i]) {
            video_renderer_t *renderer_unused = renderer_type[i];
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    /* Recreate the active pipeline from NULL so the first session does not
     * inherit the PAUSED/ASYNC state of the renderer created at engine start. */
    gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    GstState reset_state;
    gst_element_get_state(renderer->pipeline, &reset_state, NULL,
                          100 * GST_MSECOND);
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    GstState pending_state = GST_STATE_VOID_PENDING;
    GstStateChangeReturn state_ret = gst_element_get_state(
        renderer->pipeline, &new_state, &pending_state, 100 * GST_MSECOND);
    logger_log(logger, LOGGER_INFO,
               "active video pipeline state: %s pending=%s ret=%s",
               gst_element_state_get_name(new_state),
               gst_element_state_get_name(pending_state),
               gst_element_state_change_return_get_name(state_ret));
    g_timeout_add(1000, log_pipeline_state_cb,
                  gst_object_ref(renderer->pipeline));
    g_timeout_add(5000, log_pipeline_state_cb,
                  gst_object_ref(renderer->pipeline));
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        g_error("video pipeline failed to go into playing state");
        return -1;
    }
    old_state = new_state;
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    if (new_state == GST_STATE_PLAYING) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
    if (strstr(renderer->codec, h265)) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    return 0;
}
    

bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    if (!renderer) {
        return true;
    }

    if (hls_seek_enabled) {
        *seek_start = ((double) hls_seek_start) / GST_SECOND;
        *seek_duration = ((double) (hls_seek_end - hls_seek_start)) / GST_SECOND;     
    }

    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    *rate = 0.0f;
    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        if (!gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration)) {
            return true;
        }
    }
    *duration = ((double) hls_duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos) &&
                                        GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double) pos) / GST_SECOND;
        }
    }

    logger_log(logger, LOGGER_DEBUG, "******* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s rate %f *****",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state), *rate);

    return true;
}

void video_renderer_set_start(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    hls_requested_start_position = (gint64) (pos_in_micros * GST_USECOND);
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
               hls_requested_start_position);    
}

void video_renderer_seek(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    gint64 seek_position = (gint64) (pos_in_micros * GST_USECOND);
    /* don't seek to within 1  microsecond  of beginning or end of video */
    if (hls_duration < 2000) return;
    seek_position =  seek_position < 1000 ? 1000 : seek_position;
    seek_position =  seek_position > hls_duration  - 1000 ? hls_duration - 1000 : seek_position;
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);	
    } else {
        g_print("seek failed\n");
    }
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    g_assert (renderer_type[id] && renderer_type[id]->bus);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_video_pipeline_bus_callback, (gpointer) loop);    
}

bool video_renderer_eos_watch() {
    if (hls_video && renderer->eos) {
        renderer->eos = FALSE;
	return true;
    }
    return false; 
}
