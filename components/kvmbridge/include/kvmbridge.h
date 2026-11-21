// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "live_message.h"

typedef enum { KVB_UNAVAILABLE, KVB_VALID, KVB_STALE, KVB_ERROR } kvmbridge_quality_t;
typedef struct { double value; uint32_t age_ms; kvmbridge_quality_t quality; } kvmbridge_sample_t;
typedef enum { KVB_DISPLAY_NONE, KVB_DISPLAY_REVIEW, KVB_DISPLAY_SHARING, KVB_DISPLAY_CLOSING } kvmbridge_display_state_t;
typedef enum { KVB_LINK_IDLE, KVB_LINK_CONNECTING, KVB_LINK_CONNECTED } kvmbridge_link_t;
// Capture status strings are fixed firmware literals, including after the image worker exits.
typedef struct {
    kvmbridge_link_t link;
    bool sharing;
    const char *capture_code, *capture_text;
} kvmbridge_activity_t;
enum { KVB_STREAM_TIMEOUT_US = 6000000 };

// Physical virtual-desktop coordinates; pad is measured in source pixels before scaling.
typedef struct {
    int32_t cursor_x, cursor_y, left, top;
    uint16_t width, height, pad_x, pad_y;
    bool clamped;
} kvmbridge_crop_t;
typedef bool (*kvmbridge_crop_push_t)(uint32_t epoch,const uint8_t *message,size_t length,
                                      const kvmbridge_crop_t *crop);
// Used only with negotiated presentation credit. The presenter owns a full
// media packet buffer until end(epoch); the bridge writes only while writable.
typedef struct {
    uint8_t *(*start)(uint32_t epoch,uint32_t id,kvm_live_codec_t codec);
    bool (*writable)(uint32_t epoch);
} kvmbridge_receive_buffer_t;
void kvmbridge_media_with_buffer(bool (*start)(uint32_t epoch,uint32_t id,kvm_live_codec_t codec),
                                 kvmbridge_crop_push_t push,void (*end)(uint32_t epoch),
                                 kvmbridge_receive_buffer_t buffer);
// crop is borrowed until push returns. Legacy sessions supply NULL.
void kvmbridge_media_with_crop(bool (*start)(uint32_t epoch,uint32_t id,kvm_live_codec_t codec),
                               kvmbridge_crop_push_t push,void (*end)(uint32_t epoch));

// Local UI entry points. Grants expire on navigation, panel Stop, card loss and disconnect.
void kvmbridge_media(bool (*start)(uint32_t epoch,uint32_t id,kvm_live_codec_t codec),
                     bool (*push)(uint32_t epoch,const uint8_t *message,size_t length),
                     void (*end)(uint32_t epoch));
bool kvmbridge_capture(const char *profile_id,uint32_t epoch,kvm_live_codec_t preferred,const char *mode);
// Opt in only when the presenter reports each completed or discarded picture.
bool kvmbridge_capture_paced(const char *profile_id,uint32_t epoch,kvm_live_codec_t preferred,const char *mode,unsigned max_fps);
void kvmbridge_frame_done(uint32_t epoch,uint32_t sequence,bool shown);
void kvmbridge_capture_decoder_failed(uint32_t epoch);
void kvmbridge_capture_frame_timeout(uint32_t epoch);
kvmbridge_activity_t kvmbridge_activity(void);
// During pointer entry, NONE may return a monitor hint for this epoch; only REVIEW enables approval.
kvmbridge_display_state_t kvmbridge_display(uint32_t epoch, char resource[64]);
void kvmbridge_allow_display(uint32_t epoch);
bool kvmbridge_freeze(uint32_t epoch,bool paused);
bool kvmbridge_instruments(const char *profile_id,bool xplane,unsigned channel_mask);
bool kvmbridge_instruments_for_profile(const char *profile_id,bool xplane,unsigned channel_mask,bool composite);
// A present ERROR describes a provider fault and has no numeric reading. UNAVAILABLE returns false.
bool kvmbridge_sample(unsigned channel,kvmbridge_sample_t *out);
bool kvmbridge_action(bool obs,bool xplane,const char *fixed_action);
void kvmbridge_stop(void);
// Epoch zero addresses the current display of the named profile.
void kvmbridge_stop_display(const char *profile,uint32_t epoch);
bool kvmbridge_composite_telemetry(const char *profile);
// Transfer the selected profile to its full-screen magnifier during or after HELLO.
bool kvmbridge_transfer_telemetry(const char *profile);
void kvmbridge_status(char text[96]);

// Existing paired CLIENT control session. Its authenticated transport supplies the peer address.
bool kvmbridge_host_attach(uint64_t session,const char *address,uint16_t port,const uint8_t *leaf,size_t length);
void kvmbridge_host_detach(uint64_t session);
bool kvmbridge_host_endpoint(char endpoint[24]);
