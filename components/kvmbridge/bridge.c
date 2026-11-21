// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvmbridge.h"
#include "crop_wire.h"
#include "pack_scene.h"
#include "peer.h"
#include "remote_json.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kind { IMAGE, INSTRUMENTS, ACTION };
typedef enum {
    CAP_NONE, CAP_CONNECTING, CAP_CONTROL_FAILED, CAP_REQUEST_FAILED, CAP_WAIT_HOST,
    CAP_DENIED, CAP_NO_REPLY, CAP_INVALID, CAP_REVIEW, CAP_ALLOW_TIMEOUT, CAP_CONTROL_LOST,
    CAP_STARTING, CAP_RESOURCES, CAP_DECODER, CAP_BINDING, CAP_BIND_FAILED,
    CAP_STREAMING, CAP_STREAM_ENDED, CAP_FRAME_TIMEOUT, CAP_STOPPED, CAP_BUSY, CAP_CURSOR_WAIT, CAP_CURSOR_LEFT,
    CAP_PICKED_WINDOW, CAP_HOST_FRAME_TIMEOUT, CAP_APPROVAL_TIMEOUT, CAP_PICKER_TIMEOUT, CAP_PICKER_CANCEL,
    CAP_WAIT_PICKER, CAP_WAIT_POINTER, CAP_DISPLAY_MAPPING, CAP_POINTER_POSITION
} capture_state_t;
static const struct { const char *code,*text; } capture_status[]={
    [CAP_NONE]={"none","No capture requested"},
    [CAP_CONNECTING]={"connecting","Connecting to paired HOST"},
    [CAP_CONTROL_FAILED]={"control_failed","Could not open the HOST control session"},
    [CAP_REQUEST_FAILED]={"request_failed","Could not send the capture request"},
    [CAP_WAIT_HOST]={"awaiting_host","Approve sharing and select a display on the HOST"},
    [CAP_WAIT_PICKER]={"awaiting_picker","Choose a whole monitor in the Windows picker"},
    [CAP_WAIT_POINTER]={"awaiting_pointer","Move the pointer onto the monitor you selected"},
    [CAP_DISPLAY_MAPPING]={"display_mapping","Cannot map this display. Select the monitor again"},
    [CAP_POINTER_POSITION]={"pointer_position","Windows could not locate the pointer. Try again"},
    [CAP_DENIED]={"host_denied","The HOST declined or could not start capture"},
    [CAP_NO_REPLY]={"host_timeout","Timed out waiting for the HOST capture reply"},
    [CAP_INVALID]={"invalid_reply","Invalid capture reply from the HOST"},
    [CAP_REVIEW]={"awaiting_allow","Confirm the selected display on KVMpad"},
    [CAP_ALLOW_TIMEOUT]={"allow_timeout","Time expired waiting for Allow display"},
    [CAP_CONTROL_LOST]={"control_lost","HOST control connection closed before sharing"},
    [CAP_STARTING]={"starting_receiver","Preparing the display receiver"},
    [CAP_RESOURCES]={"resources","KVMpad could not allocate the display receiver"},
    [CAP_DECODER]={"decoder_failed","KVMpad could not open the selected decoder"},
    [CAP_BINDING]={"binding","Opening the HOST image channel"},
    [CAP_BIND_FAILED]={"bind_failed","Could not open or bind the HOST image channel"},
    [CAP_STREAMING]={"streaming","Sharing display"},
    [CAP_STREAM_ENDED]={"stream_ended","HOST image stream ended"},
    [CAP_FRAME_TIMEOUT]={"frame_timeout","Screen sharing stopped (no new frame)"},
    [CAP_STOPPED]={"stopped","Screen sharing stopped"},
    [CAP_BUSY]={"busy","Another HOST operation is in progress"},
    [CAP_CURSOR_WAIT]={"cursorwait","The pointer did not reach the selected display in time"},
    [CAP_CURSOR_LEFT]={"cursor","Pointer left the selected display. Tap Start sharing to retry."},
    [CAP_PICKED_WINDOW]={"pickedwindow","Select a whole monitor in the Windows picker and try again"},
    [CAP_HOST_FRAME_TIMEOUT]={"host_frame_timeout","Windows timed out waiting for an image from the selected monitor"},
    [CAP_APPROVAL_TIMEOUT]={"approval_timeout","Time expired waiting for Allow once on the HOST"},
    [CAP_PICKER_TIMEOUT]={"selection_timeout","Time expired while choosing a monitor in Windows"},
    [CAP_PICKER_CANCEL]={"cancel","Monitor selection was canceled in Windows"},
};
typedef struct { esp_transport_handle_t tls, ws; } channel_t;
typedef struct {
    enum kind kind;
    bool xplane,obs,composite,wait_for_telemetry;
    uint32_t epoch;
    kvm_live_codec_t codec;
    char mode[12],action[32];
    char profile[101];
    unsigned channel_mask, max_fps;
    bool (*start)(uint32_t,uint32_t,kvm_live_codec_t);
    bool (*push)(uint32_t,const uint8_t*,size_t);
    kvmbridge_crop_push_t push_crop;
    kvmbridge_receive_buffer_t buffer;
    void (*end)(uint32_t);
} request_t;
typedef struct {
    request_t request;
    capture_state_t capture_failure;
    kvb_peer_t peer;
    kvm_srt_identity_t id;
    channel_t control,data;
    unsigned sequence;
    int64_t last_wire;
    uint8_t chunk[1024];
    char text[1025],token[33];
    kvm_json_doc_t doc;
    uint8_t *picture;
    bool crop_v, present_v, progress_v, pointer_label_v, quality_v, pending_crop, fence_active, borrowed;
    uint32_t fence_seq;
    unsigned crop_w, crop_h;
    int64_t pair_deadline;
    kvb_crop_record_t crop;
    kvm_live_receiver_t crop_rx;
} worker_t;
static const char *const topics[]={"sim.airspeed_kt","sim.altitude_ft","sim.heading_deg",
    "sim.vertical_speed_fpm","sim.attitude.pitch_deg","sim.attitude.roll_deg"};
static const char *const units[]={"kt","ft","deg","ft/min","deg","deg"};
static const char *const formats[]={NULL,"jpeg","h264-intra","cinepak-intra"};
static const char *const ids[]={"org.kvmpad.utility.cursor-magnifier","org.kvmpad.flight.instruments",
                               "org.kvmpad.studio.obs","org.kvmpad.studio.sim"};
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
// Keep the slot reserved until the worker has closed both transports.
typedef struct {
    bool busy, composite_requested, composite_ready, handoff;
    _Atomic bool cancelled;
    kvmbridge_link_t link;
    char profile[101];
} session_slot_t;
static session_slot_t sessions[ACTION+1];
static enum kind foreground;
static bool confirm,presenting,paused;
typedef enum { CREDIT_IDLE, CREDIT_WAITING, CREDIT_SHOWN, CREDIT_DROPPED } credit_state_t;
static credit_state_t credit_state;
static uint32_t credit_frame;
static uint32_t display_epoch;
static char resource[64],status[96]="Disconnected";
// Advisory pointer-entry hint only. An epoch can be approved only via resource from STARTED.
static char pointer_hint[sizeof("\\\\.\\DISPLAY99999")];
static struct { kvmbridge_sample_t sample; int64_t measured_us; uint32_t source_ms; } samples[6];
static request_t callbacks;
static capture_state_t capture_state;

kvmbridge_activity_t kvmbridge_activity(void)
{
    portENTER_CRITICAL(&mux);
    kvmbridge_activity_t activity={.sharing=presenting,.capture_code=capture_status[capture_state].code,
                                  .capture_text=capture_status[capture_state].text};
    for (unsigned i=0;i<=ACTION;i++)
        if (sessions[i].link>activity.link) activity.link=sessions[i].link;
    portEXIT_CRITICAL(&mux);
    return activity;
}

static void capture_progress(capture_state_t step)
{
    portENTER_CRITICAL(&mux);
    capture_state=step;
    portEXIT_CRITICAL(&mux);
}

void kvmbridge_capture_decoder_failed(uint32_t epoch)
{
    portENTER_CRITICAL(&mux);
    if (epoch && epoch==display_epoch && capture_state==CAP_STARTING) capture_state=CAP_DECODER;
    portEXIT_CRITICAL(&mux);
}

void kvmbridge_capture_frame_timeout(uint32_t epoch)
{
    portENTER_CRITICAL(&mux);
    if (epoch && epoch==display_epoch && capture_state==CAP_STREAMING) capture_state=CAP_FRAME_TIMEOUT;
    portEXIT_CRITICAL(&mux);
}

void kvmbridge_frame_done(uint32_t epoch,uint32_t sequence,bool shown)
{
    portENTER_CRITICAL(&mux);
    if (epoch && display_epoch==epoch && credit_state==CREDIT_WAITING && credit_frame==sequence)
        credit_state=shown ? CREDIT_SHOWN : CREDIT_DROPPED;
    portEXIT_CRITICAL(&mux);
}

static bool stopped(const worker_t *w)
{
    return atomic_load(&sessions[w->request.kind].cancelled);
}
static void state(worker_t *w,const char *message)
{
    portENTER_CRITICAL(&mux);
    if (foreground==w->request.kind && !stopped(w)) strlcpy(status,message,sizeof(status));
    portEXIT_CRITICAL(&mux);
}
void kvmbridge_status(char text[96])
{
    portENTER_CRITICAL(&mux);
    memcpy(text,status,96);
    portEXIT_CRITICAL(&mux);
}

void kvmbridge_media(bool (*start)(uint32_t,uint32_t,kvm_live_codec_t),
                     bool (*push)(uint32_t,const uint8_t*,size_t),void (*end)(uint32_t))
{
    portENTER_CRITICAL(&mux);
    callbacks.start=start; callbacks.push=push; callbacks.push_crop=NULL; callbacks.end=end;
    callbacks.buffer=(kvmbridge_receive_buffer_t){0};
    portEXIT_CRITICAL(&mux);
}
void kvmbridge_media_with_crop(bool (*start)(uint32_t,uint32_t,kvm_live_codec_t),
                               kvmbridge_crop_push_t push,void (*end)(uint32_t))
{
    kvmbridge_media_with_buffer(start,push,end,(kvmbridge_receive_buffer_t){0});
}
void kvmbridge_media_with_buffer(bool (*start)(uint32_t,uint32_t,kvm_live_codec_t),
                                 kvmbridge_crop_push_t push,void (*end)(uint32_t),kvmbridge_receive_buffer_t buffer)
{
    portENTER_CRITICAL(&mux);
    callbacks.start=start; callbacks.push=NULL; callbacks.push_crop=push; callbacks.end=end;
    callbacks.buffer=buffer;
    portEXIT_CRITICAL(&mux);
}
static void cancel_locked(enum kind kind)
{
    atomic_store(&sessions[kind].cancelled,true);
    sessions[kind].link=KVB_LINK_IDLE;
}
void kvmbridge_stop_display(const char *profile,uint32_t epoch)
{
    portENTER_CRITICAL(&mux);
    if (profile && sessions[IMAGE].busy && !strcmp(sessions[IMAGE].profile,profile) && (!epoch || display_epoch==epoch)) {
        cancel_locked(IMAGE);
        credit_state=CREDIT_IDLE; confirm=false; presenting=false; resource[0]=0;
    }
    portEXIT_CRITICAL(&mux);
}
bool kvmbridge_composite_telemetry(const char *profile)
{
    portENTER_CRITICAL(&mux);
    session_slot_t *s=&sessions[INSTRUMENTS];
    bool active=profile && s->busy && s->composite_ready && !atomic_load(&s->cancelled) && !strcmp(profile,s->profile);
    portEXIT_CRITICAL(&mux);
    return active;
}
bool kvmbridge_transfer_telemetry(const char *profile)
{
    portENTER_CRITICAL(&mux);
    session_slot_t *s=&sessions[INSTRUMENTS];
    bool owned=profile && s->busy && !atomic_load(&s->cancelled) && !strcmp(profile,s->profile);
    if (owned) {
        s->handoff=true;
        if (!s->composite_requested || (s->link==KVB_LINK_CONNECTED && !s->composite_ready))
            cancel_locked(INSTRUMENTS);
    }
    portEXIT_CRITICAL(&mux);
    return owned;
}
void kvmbridge_stop(void)
{
    portENTER_CRITICAL(&mux);
    for (unsigned i=0;i<=ACTION;i++) {
        cancel_locked((enum kind)i);
        sessions[i].handoff=false;
    }
    credit_state=CREDIT_IDLE;
    confirm=false; presenting=false; resource[0]=0;
    memset(samples,0,sizeof(samples));
    portEXIT_CRITICAL(&mux);
}
kvmbridge_display_state_t kvmbridge_display(uint32_t epoch, char label[64])
{
    kvmbridge_display_state_t state=KVB_DISPLAY_NONE;
    label[0]=0;
    portENTER_CRITICAL(&mux);
    if (epoch && display_epoch==epoch && sessions[IMAGE].busy) {
        if (atomic_load(&sessions[IMAGE].cancelled)) state=KVB_DISPLAY_CLOSING;
        else if (resource[0]) {
            if (presenting) state=KVB_DISPLAY_SHARING;
            else if (!confirm) state=KVB_DISPLAY_REVIEW;
            if (state!=KVB_DISPLAY_NONE) memcpy(label,resource,64);
        } else if (capture_state==CAP_WAIT_POINTER) memcpy(label,pointer_hint,sizeof(pointer_hint));
    }
    portEXIT_CRITICAL(&mux);
    return state;
}
void kvmbridge_allow_display(uint32_t epoch)
{
    portENTER_CRITICAL(&mux);
    if (epoch && display_epoch==epoch && resource[0] && !atomic_load(&sessions[IMAGE].cancelled)) confirm=true;
    portEXIT_CRITICAL(&mux);
}
bool kvmbridge_freeze(uint32_t epoch,bool freeze)
{
    portENTER_CRITICAL(&mux);
    bool ok=presenting && epoch && epoch==display_epoch && !atomic_load(&sessions[IMAGE].cancelled);
    if (ok) paused=freeze;
    portEXIT_CRITICAL(&mux);
    return ok;
}
bool kvmbridge_sample(unsigned channel,kvmbridge_sample_t *out)
{
    if (channel>=6 || !out) return false;
    portENTER_CRITICAL(&mux);
    *out=samples[channel].sample;
    int64_t elapsed=esp_timer_get_time()-samples[channel].measured_us;
    portEXIT_CRITICAL(&mux);
    if (out->quality==KVB_UNAVAILABLE) return false;
    out->age_ms=elapsed>=3000000 ? 3000u : (uint32_t)(elapsed/1000);
    if (out->age_ms>=3000) { out->quality=KVB_UNAVAILABLE; return false; }
    if (out->age_ms>=1000 && out->quality==KVB_VALID) out->quality=KVB_STALE;
    return true;
}

static bool open_channel(worker_t *w,channel_t *c,const char *path)
{
    if (stopped(w)) return false;
    c->tls=esp_transport_ssl_init();
    if (!c->tls) return false;
    // Only the inspected self-issued, non-CA end-entity leaf is installed. mbedTLS compares DER exactly.
    esp_transport_ssl_set_cert_data_der(c->tls,(char*)w->peer.leaf,w->peer.length);
    esp_transport_ssl_set_client_cert_data_der(c->tls,(char*)w->id.cert_der,w->id.cert_len);
    esp_transport_ssl_set_client_key_data_der(c->tls,(char*)w->id.key_der,w->id.key_len);
    esp_transport_ssl_skip_common_name_check(c->tls);
    c->ws=esp_transport_ws_init(c->tls);
    esp_transport_ws_config_t config={.ws_path=path,.propagate_control_frames=true};
    return c->ws && esp_transport_ws_set_config(c->ws,&config)==ESP_OK &&
           esp_transport_connect(c->ws,w->peer.address,w->peer.port,3000)==0 && !stopped(w);
}
static void close_channel(channel_t *c)
{
    if (c->ws) { esp_transport_close(c->ws); esp_transport_destroy(c->ws); }
    if (c->tls) esp_transport_destroy(c->tls);
}
static bool send_text(worker_t *w,channel_t *c,const char *text)
{
    if (stopped(w)) return false;
    size_t size=strlen(text);
    return esp_transport_ws_send_raw(c->ws,WS_TRANSPORT_OPCODES_FIN|WS_TRANSPORT_OPCODES_TEXT,
                                     text,size,500)==(int)size;
}

// Distinguish the strict v1 HOST's clean policy close from a corrupt or lost response.
enum { MESSAGE_POLICY_CLOSE = -2 };
// Return 0 for idle/heartbeat, a negative result on closure, or a complete message size.
static int message(worker_t *w,channel_t *c,int opcode,uint8_t *out,int cap,int wait_ms)
{
    int ready=esp_transport_poll_read(c->ws,wait_ms);
    if (ready<=0) return ready;
    int used=0;
    bool first=true;
    int64_t until=esp_timer_get_time()+2000000;
    for (unsigned parts=0;parts<64 && !stopped(w) && esp_timer_get_time()<until;parts++) {
        int got=esp_transport_read(c->ws,(char*)w->chunk,sizeof(w->chunk),250);
        if (got<0) return -1;
        if (got>0) w->last_wire=esp_timer_get_time();
        int op=esp_transport_ws_get_read_opcode(c->ws);
        int length=esp_transport_ws_get_read_payload_len(c->ws);
        if (op==WS_TRANSPORT_OPCODES_CLOSE) {
            if (length==2 && got==1 && esp_transport_read(c->ws,(char*)w->chunk+1,1,250)==1)
                got=2;
            // The HOST reports fixed capture reasons on either authenticated image-session socket.
            if (w->request.kind==IMAGE &&
                (w->capture_failure==CAP_CONTROL_LOST || w->capture_failure==CAP_BIND_FAILED ||
                 w->capture_failure==CAP_STREAM_ENDED) && esp_transport_ws_get_fin_flag(c->ws) &&
                got==2 && length==2 && w->chunk[0]==0x0f) {
                if (w->chunk[1]==0xa1) w->capture_failure=CAP_CURSOR_LEFT; // 4001
                if (w->chunk[1]==0xa2 && w->capture_failure==CAP_STREAM_ENDED)
                    w->capture_failure=CAP_HOST_FRAME_TIMEOUT; // 4002, after capture started
                if (w->chunk[1]==0xa3) w->capture_failure=CAP_DISPLAY_MAPPING; // 4003
                if (w->chunk[1]==0xa4) w->capture_failure=CAP_POINTER_POSITION; // 4004
            }
            bool policy_close=first && esp_transport_ws_get_fin_flag(c->ws) && got==length &&
                              got>=2 && got<=125 && w->chunk[0]==0x03 && w->chunk[1]==0xf0; // 1008
            return policy_close ? MESSAGE_POLICY_CLOSE : -1;
        }
        if (op==WS_TRANSPORT_OPCODES_PING || op==WS_TRANSPORT_OPCODES_PONG) {
            if (length!=got || length>125) return -1;
            if (op==WS_TRANSPORT_OPCODES_PING &&
                esp_transport_ws_send_raw(c->ws,WS_TRANSPORT_OPCODES_FIN|WS_TRANSPORT_OPCODES_PONG,
                                          (char*)w->chunk,got,250)!=got) return -1;
            if (first) return 0;
            continue;
        }
        if (op!=(first ? opcode : WS_TRANSPORT_OPCODES_CONT) || length<=0 || length>cap-used ||
            got<=0 || got>length) return -1;
        bool fin=esp_transport_ws_get_fin_flag(c->ws);
        memcpy(out+used,w->chunk,got);
        used+=got;
        int remaining=length-got;
        while (remaining && !stopped(w) && esp_timer_get_time()<until) {
            int piece=esp_transport_read(c->ws,(char*)w->chunk,
                    remaining>(int)sizeof(w->chunk)?(int)sizeof(w->chunk):remaining,250);
            if (piece<=0 || piece>remaining) return -1;
            memcpy(out+used,w->chunk,piece);
            used+=piece; remaining-=piece;
        }
        if (remaining || stopped(w)) return -1;
        if (fin) return used;
        first=false;
    }
    return -1;
}
static int member(worker_t *w,const char *key) { return kvm_json_member(&w->doc,0,key); }
static bool string_eq(worker_t *w,const char *key,const char *value)
{ return kvm_json_str_eq(&w->doc,member(w,key),value); }
static bool parse(worker_t *w,int n)
{ return n>0 && kvm_json_parse(&w->doc,w->text,n)==KVM_JSON_OK; }
static int read_control_until(worker_t *w,int64_t end)
{
    while (!stopped(w) && esp_timer_get_time()<end) {
        int n=message(w,&w->control,WS_TRANSPORT_OPCODES_TEXT,(uint8_t*)w->text,sizeof(w->text)-1,50);
        if (n) return n;
    }
    return 0;
}
static int read_control(worker_t *w,int ms)
{ return read_control_until(w,esp_timer_get_time()+(int64_t)ms*1000); }
static bool reply(worker_t *w,const char *kind,int ms)
{
    return parse(w,read_control(w,ms)) && string_eq(w,"type",kind);
}
static bool hello(worker_t *w,bool *closed_before_session)
{
    request_t *r=&w->request;
    const char *scope=NULL;
    if (r->kind!=ACTION && (r->composite || strcmp(r->profile,ids[r->kind])))
        scope=r->kind==IMAGE ? "host.screen.region" : "host.telemetry.read";
    snprintf(w->text,sizeof(w->text),"{\"type\":\"HELLO\",\"protocol\":\"kvmbridge\",\"version\":1,\"profile\":\"%s\"%s%s%s%s}",
             r->profile,scope ? ",\"scope\":\"" : "",scope ? scope : "",scope ? "\"" : "",
             r->composite ? ",\"composite_v\":1" : "");
    if (!send_text(w,&w->control,w->text)) return false;
    int64_t until=esp_timer_get_time()+5000000;
    int n=0;
    while (!stopped(w) && !n && esp_timer_get_time()<until)
        n=message(w,&w->control,WS_TRANSPORT_OPCODES_TEXT,(uint8_t*)w->text,sizeof(w->text)-1,50);
    *closed_before_session=n==MESSAGE_POLICY_CLOSE;
    if (!parse(w,n) || !string_eq(w,"type","SESSION")) return false;
    int64_t version;
    if (!kvm_json_get_i64(&w->doc,member(w,"version"),&version) || version!=1 ||
        kvm_json_str_copy(&w->doc,member(w,"session"),w->token,sizeof(w->token))!=32) return false;
    if (scope && !string_eq(w,"scope",scope)) return false;
    int ext=member(w,"composite_v");
    if (ext>=0 && (!r->composite || !kvm_json_get_i64(&w->doc,ext,&version) || version!=1)) return false;
    if (r->composite && r->kind==IMAGE && ext<0) return false;
    r->composite=ext>=0;
    if (r->kind==IMAGE && member(w,"progress_v")>=0) {
        if (!kvm_json_get_i64(&w->doc,member(w,"progress_v"),&version) || version!=1) return false;
        w->progress_v=true;
        int hint=member(w,"pointer_label_v");
        if (hint>=0)
            w->pointer_label_v=kvm_json_get_i64(&w->doc,hint,&version) && version==1;
    }
    if (r->kind==INSTRUMENTS && member(w,"quality_v")>=0) {
        int64_t v;
        if (!kvm_json_get_i64(&w->doc,member(w,"quality_v"),&v) || v!=1) return false;
        w->quality_v=true;
    }
    if (r->kind==IMAGE && r->push_crop && member(w,"crop_v")>=0) {
        int64_t v;
        if (!kvm_json_get_i64(&w->doc,member(w,"crop_v"),&v) || v!=1) return false;
        w->crop_v=true;
    }
    if (w->crop_v && r->max_fps && member(w,"present_v")>=0) {
        int64_t v;
        if (!kvm_json_get_i64(&w->doc,member(w,"present_v"),&v) || v!=1) return false;
        w->present_v=true;
    }
    return strspn(w->token,"0123456789abcdef")==32;
}
static bool bind_channel(worker_t *w,const char *path)
{
    if (!open_channel(w,&w->data,path)) return false;
    snprintf(w->text,sizeof(w->text),"{\"type\":\"BIND\",\"session\":\"%s\"}",w->token);
    return send_text(w,&w->data,w->text);
}
static bool read_pointer_hint(worker_t *w,int token,char hint[sizeof(pointer_hint)])
{
    static const char prefix[]="\\\\.\\DISPLAY";
    const size_t prefix_len=sizeof(prefix)-1;
    size_t len=kvm_json_str_copy(&w->doc,token,hint,sizeof(pointer_hint));
    if (len==0) return true;
    if (len<=prefix_len || len>=sizeof(pointer_hint) || memcmp(hint,prefix,prefix_len)) return false;
    return strspn(hint+prefix_len,"0123456789")==len-prefix_len;
}
static bool begin_image(worker_t *w)
{
    request_t *r=&w->request;
    w->capture_failure=CAP_RESOURCES;
    bool in_place=w->present_v && r->buffer.start && r->buffer.writable;
    if (!in_place) {
        w->picture=heap_caps_malloc(KVM_LIVE_HEADER+KVM_LIVE_MAX_PAYLOAD,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!w->picture) return false;
    }
    const char *codec=formats[r->codec];
    w->sequence=1;
    char extension[96]="";
    if (w->crop_v) {
        kvm_live_geometry_t geometry=kvm_live_geometry(r->codec,r->mode);
        w->crop_w=geometry.source_width;
        w->crop_h=geometry.source_height;
        snprintf(extension,sizeof(extension),",\"crop_v\":1,\"source_crop\":[%u,%u]",w->crop_w,w->crop_h);
        if (w->present_v) {
            size_t pos=strlen(extension);
            snprintf(extension+pos,sizeof(extension)-pos,",\"present_v\":1,\"max_fps\":%u",r->max_fps);
        }
    }
    snprintf(w->text,sizeof(w->text),"{\"type\":\"START\",\"seq\":1,\"mode\":\"%s\",\"formats\":[\"%s\"]%s%s%s}",
             r->mode,codec,extension,w->progress_v ? ",\"progress_v\":1" : "",
             w->pointer_label_v ? ",\"pointer_label_v\":1" : "");
    w->capture_failure=CAP_REQUEST_FAILED;
    if (!send_text(w,&w->control,w->text)) return false;
    capture_progress(CAP_WAIT_HOST);
    // HOST allows 45 s for approval, 45 s for selection and 22 s for pointer entry.
    // Its outer deadline is 117 s. Leave time for the final reply over Wi-Fi.
    int64_t reply_deadline=esp_timer_get_time()+125000000;
    int n;
    capture_state_t stage=CAP_WAIT_HOST;
    char hint[sizeof(pointer_hint)]={0};
    for (;;) {
        w->capture_failure=CAP_CONTROL_LOST;
        n=read_control_until(w,reply_deadline);
        if (n<=0) {
            if (!n) w->capture_failure=CAP_NO_REPLY;
            return false;
        }
        w->capture_failure=CAP_INVALID;
        if (!parse(w,n)) return false;
        if (!string_eq(w,"type","START_PROGRESS")) break;
        int64_t seq;
        if (!w->progress_v || !kvm_json_get_i64(&w->doc,member(w,"seq"),&seq) || seq!=w->sequence) return false;
        if (stage==CAP_WAIT_HOST && string_eq(w,"stage","picker")) stage=CAP_WAIT_PICKER;
        else if (stage==CAP_WAIT_PICKER && string_eq(w,"stage","pointer")) stage=CAP_WAIT_POINTER;
        else return false;
        int name=member(w,"resource");
        if (w->pointer_label_v && name>=0 && (stage!=CAP_WAIT_POINTER || !read_pointer_hint(w,name,hint))) return false;
        portENTER_CRITICAL(&mux);
        memcpy(pointer_hint,hint,sizeof(hint));
        capture_state=stage;
        portEXIT_CRITICAL(&mux);
    }
    if (string_eq(w,"type","DENIED")) {
        w->capture_failure=CAP_DENIED;
        if (string_eq(w,"reason","cursorwait")) w->capture_failure=CAP_CURSOR_WAIT;
        if (string_eq(w,"reason","cursor")) w->capture_failure=CAP_CURSOR_LEFT;
        if (string_eq(w,"reason","pickedwindow")) w->capture_failure=CAP_PICKED_WINDOW;
        if (string_eq(w,"reason","geometry") || string_eq(w,"reason","topology"))
            w->capture_failure=CAP_DISPLAY_MAPPING;
        if (string_eq(w,"reason","cursorpos")) w->capture_failure=CAP_POINTER_POSITION;
        if (string_eq(w,"reason","approval_timeout")) w->capture_failure=CAP_APPROVAL_TIMEOUT;
        if (string_eq(w,"reason","selection_timeout")) w->capture_failure=CAP_PICKER_TIMEOUT;
        if (string_eq(w,"reason","cancel")) w->capture_failure=CAP_PICKER_CANCEL;
        return false;
    }
    if (!string_eq(w,"type","STARTED")) return false;
    char label[64]={0};
    int64_t id;
    size_t len=kvm_json_str_copy(&w->doc,member(w,"resource"),label,sizeof(label));
    if (!string_eq(w,"codec",codec) || !string_eq(w,"mode",r->mode) ||
        !kvm_json_get_i64(&w->doc,member(w,"stream"),&id) || id<1 || id>UINT32_MAX ||
        len<1 || len>=sizeof(label) || (hint[0] && strcmp(hint,label))) return false;
    if (w->crop_v) {
        int64_t v,cw,ch;
        int arr=member(w,"source_crop");
        if (!kvm_json_get_i64(&w->doc,member(w,"crop_v"),&v) || v!=1 ||
            kvm_json_type(&w->doc,arr)!=KVM_JSON_ARRAY || kvm_json_count(&w->doc,arr)!=2 ||
            !kvm_json_get_i64(&w->doc,kvm_json_element(&w->doc,arr,0),&cw) || cw!=w->crop_w ||
            !kvm_json_get_i64(&w->doc,kvm_json_element(&w->doc,arr,1),&ch) || ch!=w->crop_h ||
            !kvm_live_start(&w->crop_rx,(uint32_t)id,r->codec)) return false;
    } else if (member(w,"crop_v")>=0 || member(w,"source_crop")>=0) return false;
    if (w->present_v) {
        int64_t v,max_fps,start_fps;
        if (!kvm_json_get_i64(&w->doc,member(w,"present_v"),&v) || v!=1 ||
            !kvm_json_get_i64(&w->doc,member(w,"max_fps"),&max_fps) || max_fps!=r->max_fps ||
            !kvm_json_get_i64(&w->doc,member(w,"start_fps"),&start_fps) || start_fps!=(max_fps<5 ? max_fps : 5)) return false;
    } else if (member(w,"present_v")>=0 || member(w,"max_fps")>=0 || member(w,"start_fps")>=0) return false;
    for (size_t i=0;i<len;i++) if ((unsigned char)label[i]<32 || (unsigned char)label[i]>126) return false;
    portENTER_CRITICAL(&mux);
    memcpy(resource,label,sizeof(resource));
    capture_state=CAP_REVIEW;
    portEXIT_CRITICAL(&mux);
    state(w,"Confirm the selected display");
    w->capture_failure=CAP_CONTROL_LOST;
    int64_t end=esp_timer_get_time()+40000000;
    bool allowed=false;
    while (!stopped(w) && esp_timer_get_time()<end) {
        portENTER_CRITICAL(&mux); allowed=confirm; portEXIT_CRITICAL(&mux);
        if (allowed) break;
        n=message(w,&w->control,WS_TRANSPORT_OPCODES_TEXT,(uint8_t*)w->text,1024,50);
        if (n) {
            if (n>0) w->capture_failure=CAP_INVALID;
            return false;
        }
    }
    if (!allowed || stopped(w)) { w->capture_failure=CAP_ALLOW_TIMEOUT; return false; }
    capture_progress(CAP_STARTING);
    w->capture_failure=CAP_RESOURCES;
    if (in_place) {
        w->picture=r->buffer.start(r->epoch,(uint32_t)id,r->codec);
        if (!w->picture) return false;
        w->borrowed=true;
    } else if (!r->start(r->epoch,(uint32_t)id,r->codec)) return false;
    capture_progress(CAP_BINDING);
    w->capture_failure=CAP_BIND_FAILED;
    if (!bind_channel(w,"/image")) {
        // The picker may close while the panel is opening its second TLS connection.
        if (!stopped(w)) (void)message(w,&w->control,WS_TRANSPORT_OPCODES_TEXT,(uint8_t*)w->text,1024,0);
        return false;
    }
    w->capture_failure=CAP_STREAM_ENDED;
    portENTER_CRITICAL(&mux);
    presenting=!stopped(w);
    if (presenting) capture_state=CAP_STREAMING;
    portEXIT_CRITICAL(&mux);
    return true;
}
static const char *sim(worker_t *w) { return w->request.xplane ? "sim.xplane" : "sim.msfs2024"; }
static bool begin_telemetry(worker_t *w)
{
    w->sequence=1;
    int off=snprintf(w->text,sizeof(w->text),"{\"type\":\"SUBSCRIBE\",\"seq\":1,\"source\":\"%s\",\"topics\":[",sim(w));
    const char *separator="";
    for (unsigned i=0;i<6;i++) {
        if (!(w->request.channel_mask & (1u<<i))) continue;
        off+=snprintf(w->text+off,sizeof(w->text)-off,"%s{\"name\":\"%s\",\"rate_hz\":%u}",separator,topics[i],i>=4?20:10);
        separator=",";
    }
    strlcpy(w->text+off,w->quality_v ? "],\"quality_v\":1}" : "]}",sizeof(w->text)-off);
    if (!send_text(w,&w->control,w->text) || !reply(w,"SUBSCRIBED",50000) || !string_eq(w,"source",sim(w))) return false;
    int v=member(w,"quality_v");
    int64_t accepted;
    if (w->quality_v ? (!kvm_json_get_i64(&w->doc,v,&accepted) || accepted!=1) : v>=0) return false;
    return bind_channel(w,"/telemetry");
}
static bool sample_update(worker_t *w,int n,int64_t *last)
{
    if (!parse(w,n) || !string_eq(w,"type","TELEMETRY") || !string_eq(w,"source",sim(w))) return false;
    int64_t seq,ts;
    if (!kvm_json_get_i64(&w->doc,member(w,"seq"),&seq) || seq<=*last ||
        !kvm_json_get_i64(&w->doc,member(w,"ts_mono_ms"),&ts) || ts<0 || ts>UINT32_MAX) return false;
    unsigned i=0;
    while (i<6 && !string_eq(w,"topic",topics[i])) i++;
    if (i==6 || !(w->request.channel_mask & (1u<<i)) || !string_eq(w,"unit",units[i])) return false;
    kvmbridge_sample_t s={0};
    int val=member(w,"value");
    if (string_eq(w,"quality","VALID") || string_eq(w,"quality","STALE")) {
        if (kvm_json_type(&w->doc,val)!=KVM_JSON_NUMBER) return false;
        const kvm_json_tok_t *t=&kvm_json_tokens(&w->doc)[val];
        char number[40]={0};
        if (t->len>=sizeof(number)) return false;
        memcpy(number,w->text+t->start,t->len);
        char *end=NULL;
        s.value=strtod(number,&end);
        if (!isfinite(s.value) || *end) return false;
        s.quality=string_eq(w,"quality","VALID")?KVB_VALID:KVB_STALE;
    } else {
        if (kvm_json_type(&w->doc,val)!=KVM_JSON_NULL) return false;
        if (w->quality_v && string_eq(w,"quality","ERROR")) s.quality=KVB_ERROR;
        else if (!string_eq(w,"quality","UNAVAILABLE")) return false;
    }
    int age_token=member(w,"age_ms");
    int64_t age=s.quality==KVB_STALE ? 1000 : 0;
    if (age_token>=0 && (!kvm_json_get_i64(&w->doc,age_token,&age) || age<0 || age>3000 ||
        (s.quality==KVB_STALE && age<1000) || (s.quality==KVB_VALID && age>=1000))) return false;
    portENTER_CRITICAL(&mux);
    if (!stopped(w)) {
        int64_t measured=esp_timer_get_time()-age*1000;
        // A repeated HOST timestamp is the same measurement even if a legacy peer omits its age.
        if (samples[i].sample.quality!=KVB_UNAVAILABLE && samples[i].source_ms==(uint32_t)ts &&
            samples[i].measured_us<measured) measured=samples[i].measured_us;
        samples[i].sample=s;
        samples[i].measured_us=measured;
        samples[i].source_ms=(uint32_t)ts;
    }
    portEXIT_CRITICAL(&mux);
    *last=seq;
    return true;
}
static bool action(worker_t *w)
{
    w->sequence=1;
    snprintf(w->text,sizeof(w->text),"{\"type\":\"ACTION\",\"seq\":1,\"source\":\"%s\",\"action\":\"%s\"}",
             w->request.obs?"obs":sim(w),w->request.action);
    int64_t seq;
    if (!send_text(w,&w->control,w->text) || !reply(w,"ACCEPTED",50000) ||
        !kvm_json_get_i64(&w->doc,member(w,"seq"),&seq) || seq!=1 || !reply(w,"RESULT",3000)) return false;
    if (!kvm_json_get_i64(&w->doc,member(w,"seq"),&seq) || seq!=1) return false;
    if (string_eq(w,"status","UNVERIFIED")) { state(w,"Simulator command sent; effect unverified"); return true; }
    if (!string_eq(w,"status","SUCCESS")) return false;
    bool flag;
    if (kvm_json_get_bool(&w->doc,member(w,"recording"),&flag)) state(w,flag?"OBS recording":"OBS recording stopped");
    else if (kvm_json_get_bool(&w->doc,member(w,"muted"),&flag)) state(w,flag?"OBS input muted":"OBS input unmuted");
    else if (kvm_json_get_bool(&w->doc,member(w,"selected"),&flag)) state(w,flag?"OBS scene selected":"OBS scene differs");
    else return false;
    return true;
}
// Only the socket owner sends feedback. LVGL and the decoder publish one terminal result.
static bool send_frame_credit(worker_t *w,int *wait_ms)
{
    portENTER_CRITICAL(&mux);
    credit_state_t result=credit_state;
    uint32_t frame=credit_frame;
    if (result==CREDIT_SHOWN || result==CREDIT_DROPPED) credit_state=CREDIT_IDLE;
    portEXIT_CRITICAL(&mux);
    if (result==CREDIT_WAITING) *wait_ms=5;
    if (result!=CREDIT_SHOWN && result!=CREDIT_DROPPED) return true;
    snprintf(w->text,sizeof(w->text),"{\"type\":\"FRAME_DONE\",\"stream\":%lu,\"frame\":%lu,\"shown\":%s}",
             (unsigned long)w->crop_rx.stream_id,(unsigned long)frame,result==CREDIT_SHOWN ? "true" : "false");
    return send_text(w,&w->data,w->text);
}

// Start can be pressed before the telemetry HELLO completes. Keep the UI responsive
// while the first worker negotiates a second scope or closes the legacy session.
static bool wait_for_telemetry(worker_t *w)
{
    int64_t end=esp_timer_get_time()+20000000;
    while (!stopped(w) && esp_timer_get_time()<end) {
        portENTER_CRITICAL(&mux);
        session_slot_t *s=&sessions[INSTRUMENTS];
        bool ready=s->composite_ready && !atomic_load(&s->cancelled);
        bool released=!s->busy;
        portEXIT_CRITICAL(&mux);
        if (ready || released) {
            w->request.composite=ready;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}
static void worker(void *arg)
{
    worker_t *w=arg;
    request_t *r=&w->request;
    bool result=false;
    w->capture_failure=CAP_CONTROL_FAILED;
    if (r->wait_for_telemetry && !wait_for_telemetry(w)) goto finish;
    if (!kvb_peer_load(&w->peer) || !kvb_identity(&w->id,true) ||
        !open_channel(w,&w->control,"/control")) goto finish;
    bool closed=false;
    if (!hello(w,&closed)) {
        // Older HOSTs close on an unknown HELLO field, before a session or grant exists.
        // Only the first telemetry worker can retry alone with the original HELLO.
        if (!closed || stopped(w) || !r->composite || r->kind!=INSTRUMENTS) goto finish;
        portENTER_CRITICAL(&mux);
        if (sessions[INSTRUMENTS].handoff) cancel_locked(INSTRUMENTS);
        portEXIT_CRITICAL(&mux);
        if (stopped(w)) goto finish;
        close_channel(&w->control);
        w->control=(channel_t){0};
        r->composite=false;
        if (!open_channel(w,&w->control,"/control") || !hello(w,&closed)) goto finish;
    }
    portENTER_CRITICAL(&mux);
    session_slot_t *slot=&sessions[r->kind];
    if (!stopped(w)) {
        slot->link=KVB_LINK_CONNECTED;
        slot->composite_ready=r->composite;
        if (r->kind==INSTRUMENTS && slot->handoff && !r->composite) cancel_locked(INSTRUMENTS);
    }
    portEXIT_CRITICAL(&mux);
    if (r->kind==ACTION) { result=action(w); goto finish; }
    bool image=r->kind==IMAGE;
    if (!(image?begin_image(w):begin_telemetry(w))) goto finish;
    state(w,image?"Sharing display":"Reading instruments");
    bool frozen=false;
    int64_t last=0,deadline=esp_timer_get_time()+KVB_STREAM_TIMEOUT_US;
    w->sequence=1;
    while (!stopped(w)) {
        portENTER_CRITICAL(&mux); bool freeze=paused; portEXIT_CRITICAL(&mux);
        if (image && freeze!=frozen) {
            snprintf(w->text,sizeof(w->text),"{\"type\":\"%s\",\"seq\":%u}",freeze?"FREEZE":"RESUME",++w->sequence);
            int64_t seq;
            if (!send_text(w,&w->control,w->text) || !reply(w,freeze?"FROZEN":"RESUMED",3000) ||
                !kvm_json_get_i64(&w->doc,member(w,"seq"),&seq) || seq!=w->sequence) break;
            if (w->crop_v && freeze) {
                int64_t fence;
                if (!kvm_json_get_i64(&w->doc,member(w,"last_frame"),&fence) || fence < -1 || fence>UINT32_MAX) break;
                w->fence_active=fence>=0;
                w->fence_seq=(uint32_t)fence;
                portENTER_CRITICAL(&mux); credit_state=CREDIT_IDLE; portEXIT_CRITICAL(&mux);
            }
            frozen=freeze;
            deadline=esp_timer_get_time()+KVB_STREAM_TIMEOUT_US;
        }
        if (message(w,&w->control,WS_TRANSPORT_OPCODES_TEXT,(uint8_t*)w->text,1024,0)!=0) break;
        bool metadata=image && w->crop_v && !w->pending_crop;
        bool binary=image && !metadata;
        int wait_ms=40;
        if (w->present_v && !send_frame_credit(w,&wait_ms)) break;
        int n=0;
        // A freeze fence can retire an old credit while its decoder is still reading.
        if (binary && w->borrowed && !r->buffer.writable(r->epoch))
            vTaskDelay(1);
        else n=message(w,&w->data,binary?WS_TRANSPORT_OPCODES_BINARY:WS_TRANSPORT_OPCODES_TEXT,
                      binary?w->picture:(uint8_t*)w->text,binary?KVM_LIVE_MAX_PAYLOAD+32:metadata?160:1024,wait_ms);
        int64_t now=esp_timer_get_time();
        if (n<0 || now-w->last_wire>8000000 || (w->pending_crop && now>w->pair_deadline) ||
            (!frozen && now>deadline)) {
            if (image && n>=0 && !frozen && now>deadline) w->capture_failure=CAP_FRAME_TIMEOUT;
            break;
        }
        if (n && !stopped(w)) {
            if (metadata) {
                if (w->present_v) {
                    portENTER_CRITICAL(&mux); bool outstanding=credit_state!=CREDIT_IDLE; portEXIT_CRITICAL(&mux);
                    if (outstanding) break;
                }
                if (!parse(w,n) || !kvb_crop_parse(&w->doc,w->crop_rx.stream_id,w->crop_w,w->crop_h,&w->crop)) break;
                w->pending_crop=true;
                w->pair_deadline=now+2000000;
                continue;
            }
            if (image) {
                const kvmbridge_crop_t *crop=NULL;
                if (w->crop_v) {
                    if (!w->pending_crop || !kvb_crop_match(&w->crop,&w->crop_rx,w->picture,n)) break;
                    w->pending_crop=false;
                    if (w->fence_active) {
                        uint32_t delta=w->crop.sequence-w->fence_seq;
                        if (!delta || delta>=UINT32_C(0x80000000)) continue;
                        w->fence_active=false;
                    }
                    crop=&w->crop.geometry;
                }
                if (freeze) continue;
                if (w->present_v) {
                    portENTER_CRITICAL(&mux);
                    if (!stopped(w)) {
                        credit_frame=w->crop.sequence;
                        credit_state=CREDIT_WAITING;
                    }
                    portEXIT_CRITICAL(&mux);
                }
                bool accepted=r->push_crop ? r->push_crop(r->epoch,w->picture,n,crop) : r->push(r->epoch,w->picture,n);
                if (!accepted) {
                    if (w->present_v) kvmbridge_frame_done(r->epoch,w->crop.sequence,false);
                    continue;
                }
            } else if (!sample_update(w,n,&last)) break;
            deadline=esp_timer_get_time()+KVB_STREAM_TIMEOUT_US;
        }
    }
finish:
    portENTER_CRITICAL(&mux);
    sessions[r->kind].link=KVB_LINK_IDLE;
    sessions[r->kind].composite_ready=false;
    if (r->kind==IMAGE) {
        credit_state=CREDIT_IDLE; presenting=false; resource[0]=0;
        if (capture_state!=CAP_DECODER && capture_state!=CAP_FRAME_TIMEOUT)
            capture_state=stopped(w) ? CAP_STOPPED : w->capture_failure;
    }
    if (r->kind==INSTRUMENTS) memset(samples,0,sizeof(samples));
    portEXIT_CRITICAL(&mux);
    if (w->borrowed) w->picture=NULL; // end releases the transport's last reference.
    if (r->kind==IMAGE && r->end) r->end(r->epoch);
    if (w->sequence && w->control.ws) {
        int n=snprintf(w->text,sizeof(w->text),"{\"type\":\"STOP\",\"seq\":%u}",w->sequence+1);
        (void)esp_transport_ws_send_raw(w->control.ws,WS_TRANSPORT_OPCODES_FIN|WS_TRANSPORT_OPCODES_TEXT,
                                        w->text,n,50);
    }
    close_channel(&w->data); close_channel(&w->control);
    if (w->picture) {
        mbedtls_platform_zeroize(w->picture, KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD);
        free(w->picture);
    }
    portENTER_CRITICAL(&mux);
    if (!result && foreground==r->kind) strlcpy(status,stopped(w)?"Stopped":"HOST unavailable, denied or disconnected",sizeof(status));
    sessions[r->kind].busy=false;
    if (r->kind==IMAGE) { confirm=false; paused=false; display_epoch=0; }
    portEXIT_CRITICAL(&mux);
    mbedtls_platform_zeroize(w,sizeof(*w)); free(w);
    vTaskDelete(NULL);
}

static bool launch(request_t *r)
{
    worker_t *w=heap_caps_calloc(1,sizeof(*w),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!w) {
        portENTER_CRITICAL(&mux);
        if (r->kind==IMAGE && !sessions[IMAGE].busy) capture_state=CAP_RESOURCES;
        portEXIT_CRITICAL(&mux);
        return false;
    }
    portENTER_CRITICAL(&mux);
    session_slot_t *telemetry=&sessions[INSTRUMENTS], *slot=&sessions[r->kind];
    bool negotiated=telemetry->composite_ready && !atomic_load(&telemetry->cancelled);
    bool paired=r->kind==IMAGE && telemetry->busy && (negotiated || telemetry->handoff) &&
                !strcmp(r->profile,telemetry->profile);
    if (sessions[ACTION].busy || sessions[IMAGE].busy || (telemetry->busy && !paired)) {
        if (r->kind==IMAGE && !sessions[IMAGE].busy) capture_state=CAP_BUSY;
        portEXIT_CRITICAL(&mux); free(w); return false;
    }
    foreground=r->kind;
    slot->busy=true; slot->composite_ready=false; slot->link=KVB_LINK_CONNECTING;
    atomic_store(&slot->cancelled,false);
    strlcpy(slot->profile,r->profile,sizeof(slot->profile));
    slot->composite_requested=r->composite; slot->handoff=false;
    if (r->kind==IMAGE) {
        capture_state=CAP_CONNECTING;
        r->composite=paired && negotiated;
        r->wait_for_telemetry=paired && !negotiated;
        display_epoch=r->epoch;
        r->start=callbacks.start; r->push=callbacks.push; r->push_crop=callbacks.push_crop; r->end=callbacks.end;
        r->buffer=callbacks.buffer;
    }
    w->request=*r;
    strlcpy(status,"Connecting to paired HOST",sizeof(status));
    portEXIT_CRITICAL(&mux);
    if ((r->kind==IMAGE && (!r->start || (!r->push && !r->push_crop) || !r->end)) ||
        xTaskCreate(worker,"kvmbridge",7168,w,3,NULL)!=pdPASS) {
        portENTER_CRITICAL(&mux);
        slot->busy=false; slot->link=KVB_LINK_IDLE;
        if (r->kind==IMAGE) { display_epoch=0; capture_state=CAP_RESOURCES; }
        if (foreground==r->kind) strlcpy(status,"HOST worker unavailable",sizeof(status));
        portEXIT_CRITICAL(&mux);
        free(w); return false;
    }
    return true;
}
static bool pack_profile(const char *profile)
{
    return pack_profile_id_valid(profile) && strcmp(profile,ids[2]) && strcmp(profile,ids[3]);
}

bool kvmbridge_capture(const char *profile,uint32_t epoch,kvm_live_codec_t codec,const char *mode)
{
    return kvmbridge_capture_paced(profile,epoch,codec,mode,0);
}
bool kvmbridge_capture_paced(const char *profile,uint32_t epoch,kvm_live_codec_t codec,const char *mode,unsigned max_fps)
{
    if (!epoch || !pack_profile(profile) || codec<1 || codec>3 || max_fps>10 || !mode ||
        (strcmp(mode,"OVERVIEW") && strcmp(mode,"ZOOM_2X") && strcmp(mode,"PIXEL_1X"))) return false;
    request_t r={.kind=IMAGE,.epoch=epoch,.codec=codec,.max_fps=max_fps};
    strcpy(r.profile,profile);
    strlcpy(r.mode,mode,sizeof(r.mode));
    return launch(&r);
}
bool kvmbridge_instruments(const char *profile,bool xplane,unsigned channel_mask)
{
    return kvmbridge_instruments_for_profile(profile,xplane,channel_mask,false);
}
bool kvmbridge_instruments_for_profile(const char *profile,bool xplane,unsigned channel_mask,bool composite)
{
    if (!pack_profile(profile) || !channel_mask || (channel_mask >> 6)) return false;
    request_t r={.kind=INSTRUMENTS,.xplane=xplane,.channel_mask=channel_mask,.composite=composite};
    strcpy(r.profile,profile);
    return launch(&r);
}
bool kvmbridge_action(bool obs,bool xplane,const char *name)
{
    const char *actions[]={"obs.record.status","obs.record.start","obs.record.stop","obs.input.mute",
                           "obs.input.unmute","obs.scene.select","sim.flaps.up","sim.flaps.down"};
    if (!name) return false;
    for (unsigned i=obs?0:6;i<(obs?6:8);i++) if (!strcmp(name,actions[i])) {
        request_t r={.kind=ACTION,.obs=obs,.xplane=xplane};
        strcpy(r.profile,ids[obs ? 2 : 3]);
        strlcpy(r.action,name,sizeof(r.action));
        return launch(&r);
    }
    return false;
}
