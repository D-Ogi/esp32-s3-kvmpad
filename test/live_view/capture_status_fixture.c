// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Exercise the production worker with a virtual clock and one HOST control reply on stdin.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WS_TRANSPORT_OPCODES_CONT, WS_TRANSPORT_OPCODES_TEXT, WS_TRANSPORT_OPCODES_BINARY,
       WS_TRANSPORT_OPCODES_CLOSE=8, WS_TRANSPORT_OPCODES_PING, WS_TRANSPORT_OPCODES_PONG,
       WS_TRANSPORT_OPCODES_FIN=128, pdPASS=1, ESP_OK=0 };
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define heap_caps_calloc(n,s,m) calloc(n,s)
static unsigned private_buffers;
static void *private_packet(size_t n) { ++private_buffers; return malloc(n); }
#define heap_caps_malloc(s,m) private_packet(s)
#define mbedtls_platform_zeroize(p,n) memset(p,0,n)
#define vTaskDelete(t) ((void)0)
#define pdMS_TO_TICKS(t) (t)
static int64_t clock_us;
#define vTaskDelay(t) (clock_us += (t)*1000)
#define esp_timer_get_time() clock_us
static void *task_arg;
#define xTaskCreate(fn,name,stack,arg,prio,h) (task_arg=(arg),pdPASS)
#define strlcpy(dst,src,n) snprintf(dst,n,"%s",src)

typedef struct { const char *ws_path; bool propagate_control_frames; } esp_transport_ws_config_t;
typedef struct { bool image; int op, length, offset; const char *payload; } *esp_transport_handle_t;
#define esp_transport_ssl_init() calloc(1,sizeof(*((esp_transport_handle_t)0)))
#define esp_transport_ws_init(t) esp_transport_ssl_init()
#define esp_transport_ssl_set_cert_data_der(...) ((void)0)
#define esp_transport_ssl_set_client_cert_data_der(...) ((void)0)
#define esp_transport_ssl_set_client_key_data_der(...) ((void)0)
#define esp_transport_ssl_skip_common_name_check(...) ((void)0)
#define esp_transport_close(t) ((void)0)
#define esp_transport_destroy(t) free(t)
#define esp_transport_ws_get_read_opcode(t) ((t)->op)
#define esp_transport_ws_get_read_payload_len(t) ((t)->length)
#define esp_transport_ws_get_fin_flag(t) true
static const char *variant;
static int review_polls, pongs, receiver_starts, binds, ends;
static bool replied, awaiting_start, freeze_requested;
static unsigned progress_messages;
static char wire[1025];
static const char *expected_hint, *expected_review;
static char hint_snapshot[64];
static int esp_transport_ws_set_config(esp_transport_handle_t t,const esp_transport_ws_config_t *config)
{
    t->image=!strcmp(config->ws_path,"/image");
    return 0;
}
#define esp_transport_connect(t,...) ((t)->image && (!strcmp(variant,"bind") || !strcmp(variant,"borrow_bind") || !strcmp(variant,"cursor_bind") || !strcmp(variant,"mapping_bind")) ? -1 : 0)
static int esp_transport_poll_read(esp_transport_handle_t t,int ms);
static int esp_transport_ws_send_raw(esp_transport_handle_t t,int opcode,const char *data,int n,int ms);
static int esp_transport_read(esp_transport_handle_t t,char *out,int n,int ms)
{
    (void)ms;
    int count=t->length-t->offset;
    if (count>n) count=n;
    if (count>31) count=31; // TLS can split one extended-length WebSocket payload across reads.
    if (t->op==WS_TRANSPORT_OPCODES_CLOSE && count>1) count=1;
    memcpy(out,t->payload+t->offset,count);
    t->offset+=count;
    return count;
}
#include "bridge.c"

static void incoming(esp_transport_handle_t t,int op,const char *payload)
{
    t->op=op; t->payload=payload; t->length=(int)strlen(payload); t->offset=0;
}
static int esp_transport_ws_send_raw(esp_transport_handle_t t,int opcode,const char *data,int n,int ms)
{
    (void)ms;
    if (opcode==(WS_TRANSPORT_OPCODES_FIN|WS_TRANSPORT_OPCODES_PONG)) {
        assert(n==4 && !memcmp(data,"beat",4)); ++pongs;
    } else if (strstr(data,"\"HELLO\"")) {
        incoming(t,WS_TRANSPORT_OPCODES_TEXT,!strncmp(variant,"progress_label_",15) ?
                 "{\"type\":\"SESSION\",\"session\":\"1234567890abcdef1234567890abcdef\",\"version\":1,\"crop_v\":1,\"present_v\":1,\"progress_v\":1,\"pointer_label_v\":1}" :
                 !strcmp(variant,"label_without_progress") ?
                 "{\"type\":\"SESSION\",\"session\":\"1234567890abcdef1234567890abcdef\",\"version\":1,\"crop_v\":1,\"present_v\":1,\"pointer_label_v\":1}" :
                 !strncmp(variant,"progress_",9) ?
                 "{\"type\":\"SESSION\",\"session\":\"1234567890abcdef1234567890abcdef\",\"version\":1,\"crop_v\":1,\"present_v\":1,\"progress_v\":1}" :
                 !strcmp(variant,"unpaced") ?
                 "{\"type\":\"SESSION\",\"session\":\"1234567890abcdef1234567890abcdef\",\"version\":1,\"crop_v\":1}" :
                 "{\"type\":\"SESSION\",\"session\":\"1234567890abcdef1234567890abcdef\",\"version\":1,\"crop_v\":1,\"present_v\":1}");
    } else if (strstr(data,"\"START\"")) {
        assert((strstr(data,"\"progress_v\":1")!=NULL)==(!strncmp(variant,"progress_",9)));
        assert((strstr(data,"\"pointer_label_v\":1")!=NULL)==(!strncmp(variant,"progress_label_",15)));
        if (!strcmp(variant,"send")) return -1;
        awaiting_start=true;
        incoming(t,WS_TRANSPORT_OPCODES_PING,"beat");
    } else if (strstr(data,"\"BIND\"")) {
        assert(t->image && receiver_starts==1 && !strcmp(kvmbridge_activity().capture_code,"binding"));
        ++binds;
    } else if (strstr(data,"\"FREEZE\"")) {
        assert(!t->image && !strcmp(variant,"cursor_freeze") && !freeze_requested);
        freeze_requested=true;
        incoming(t,WS_TRANSPORT_OPCODES_CLOSE,"\x0f\xa1");
    }
    return n;
}
static int esp_transport_poll_read(esp_transport_handle_t t,int ms)
{
    clock_us+=(int64_t)ms*1000;
    if (t->offset<t->length) return 1;
    if (awaiting_start && !replied && strcmp(variant,"timeout")) {
        const char *expected=progress_messages==0 ? "awaiting_host" : progress_messages==1 ? "awaiting_picker" : "awaiting_pointer";
        assert(!strcmp(kvmbridge_activity().capture_code,expected) && pongs==1);
        assert(!receiver_starts && !binds && !review_polls);
        char label[64];
        assert(kvmbridge_display(18,label)==KVB_DISPLAY_NONE && !label[0]);
        assert(kvmbridge_display(19,label)==KVB_DISPLAY_NONE);
        assert(!strcmp(label,progress_messages==2 ? expected_hint : ""));
        kvmbridge_allow_display(19);
        assert(!confirm && !kvmbridge_activity().sharing);
        if (progress_messages==2) memcpy(hint_snapshot,label,sizeof(label));
        if (!strcmp(variant,"progress_expiry")) {
            if (clock_us<INT64_C(110000000) || progress_messages==2) return 0;
        }
        if (progress_messages) assert(fgets(wire,sizeof(wire),stdin));
        if (!strcmp(variant,"late_selection") && clock_us < INT64_C(112000000)) return 0;
        if (strstr(wire,"START_PROGRESS")) ++progress_messages;
        else replied=true;
        if (!strcmp(variant,"lost")) return -1;
        if (!strcmp(variant,"mapping_before_review")) {
            incoming(t,WS_TRANSPORT_OPCODES_CLOSE,"\x0f\xa3");
            return 1;
        }
        wire[strcspn(wire,"\r\n")]=0;
        incoming(t,WS_TRANSPORT_OPCODES_TEXT,wire);
        return 1;
    }
    if (capture_state==CAP_REVIEW) {
        char label[64];
        assert(kvmbridge_display(19,label)==KVB_DISPLAY_REVIEW && !strcmp(label,expected_review));
        assert(!kvmbridge_activity().sharing && receiver_starts==0);
        ++review_polls;
        if (review_polls==1) { incoming(t,WS_TRANSPORT_OPCODES_PING,"beat"); return 1; }
        if (!strcmp(variant,"cursor_review")) { incoming(t,WS_TRANSPORT_OPCODES_CLOSE,"\x0f\xa1"); return 1; }
        if (!strcmp(variant,"mapping_review")) { incoming(t,WS_TRANSPORT_OPCODES_CLOSE,"\x0f\xa3"); return 1; }
        if (strcmp(variant,"allow_timeout")) kvmbridge_allow_display(19);
    }
    if (capture_state==CAP_BINDING && (!strcmp(variant,"cursor_bind") || !strcmp(variant,"mapping_bind"))) {
        assert(!t->image);
        incoming(t,WS_TRANSPORT_OPCODES_CLOSE,!strcmp(variant,"mapping_bind") ? "\x0f\xa3" : "\x0f\xa1");
        return 1;
    }
    if (capture_state==CAP_STREAMING) {
        if (atomic_load(&sessions[IMAGE].cancelled)) return -1;
        assert(kvmbridge_activity().sharing && binds==1);
        if (!strcmp(variant,"cursor_freeze")) {
            assert(kvmbridge_freeze(19,true));
            return 0;
        }
        if (!strncmp(variant,"source_timeout_",15) || !strncmp(variant,"mapping_",8) ||
            !strncmp(variant,"pointer_position_",17)) {
            if (t->image!=(strstr(variant,"_data")!=NULL)) return 0;
            const char *code=!strncmp(variant,"mapping_",8) ? "\x0f\xa3" :
                             !strncmp(variant,"pointer_position_",17) ? "\x0f\xa4" : "\x0f\xa2";
            incoming(t,WS_TRANSPORT_OPCODES_CLOSE,code);
            return 1;
        }
        if (!strcmp(variant,"cursor_data") || !strcmp(variant,"cursor_control")) {
            if (t->image!=(!strcmp(variant,"cursor_data"))) return 0;
            incoming(t,WS_TRANSPORT_OPCODES_CLOSE,"\x0f\xa1");
            return 1;
        }
        kvmbridge_stop_display(ids[IMAGE],19);
    }
    return 0;
}
bool kvb_peer_load(kvb_peer_t *p) { (void)p; return strcmp(variant,"control")!=0; }
bool kvb_identity(kvm_srt_identity_t *p,bool key) { (void)p; return key; }
bool pack_profile_id_valid(const char *p) { return !strcmp(p,ids[IMAGE]); }
static bool start_receiver(uint32_t epoch,uint32_t id,kvm_live_codec_t codec)
{
    assert(epoch==19 && id && codec==KVM_LIVE_JPEG && confirm && pongs==2);
    ++receiver_starts;
    kvmbridge_capture_decoder_failed(18);
    assert(capture_state==CAP_STARTING);
    if (!strcmp(variant,"decoder")) { kvmbridge_capture_decoder_failed(epoch); return false; }
    return strcmp(variant,"resources")!=0;
}
static bool push(uint32_t epoch,const uint8_t *data,size_t size,const kvmbridge_crop_t *crop)
{ (void)epoch; (void)data; (void)size; (void)crop; abort(); }
static uint8_t borrowed_packet[KVM_LIVE_HEADER+KVM_LIVE_MAX_PAYLOAD];
static uint8_t *start_borrowed(uint32_t epoch,uint32_t id,kvm_live_codec_t codec)
{
    assert(!strncmp(variant,"borrow",6) && !private_buffers);
    return start_receiver(epoch,id,codec) ? borrowed_packet : NULL;
}
static bool writable(uint32_t epoch) { return epoch==19; }
static void ended(uint32_t epoch) { assert(epoch==19 && !ends++); }
int main(int argc,char **argv)
{
    assert(argc>=3 && argc<=5 && fgets(wire,sizeof(wire),stdin));
    variant=argv[1];
    expected_hint=argc>=4 ? argv[3] : "";
    expected_review=argc>=5 ? argv[4] : "\\\\.\\DISPLAY1";
    assert(!strcmp(kvmbridge_activity().capture_code,"none"));
    bool borrowed=!strncmp(variant,"borrow",6);
    if (borrowed || !strcmp(variant,"unpaced"))
        kvmbridge_media_with_buffer(start_receiver,push,ended,(kvmbridge_receive_buffer_t){start_borrowed,writable});
    else
        kvmbridge_media_with_crop(start_receiver,push,ended);
    assert(kvmbridge_capture_paced(ids[IMAGE],19,KVM_LIVE_JPEG,"PIXEL_1X",10));
    assert(!strcmp(kvmbridge_activity().capture_code,"connecting"));
    // Keep a simultaneous telemetry session connected through image teardown.
    sessions[INSTRUMENTS].link=KVB_LINK_CONNECTED;
    worker(task_arg);
    char after[64];
    assert(kvmbridge_display(19,after)==KVB_DISPLAY_NONE && !after[0]);
    assert(!strcmp(hint_snapshot,expected_hint));
    assert(kvmbridge_activity().link==KVB_LINK_CONNECTED && !kvmbridge_activity().sharing);
    if (strcmp(kvmbridge_activity().capture_code,argv[2]))
        fprintf(stderr,"%s: got %s after %u progress messages, expected %s\n",variant,
                kvmbridge_activity().capture_code,progress_messages,argv[2]);
    assert(!strcmp(kvmbridge_activity().capture_code,argv[2]));
    assert(!strstr(kvmbridge_activity().capture_text,"DISPLAY"));
    if (!strcmp(argv[2],"cursorwait")) assert(strstr(kvmbridge_activity().capture_text,"did not reach the selected display"));
    if (!strcmp(argv[2],"pickedwindow")) assert(!receiver_starts && !review_polls &&
        strstr(kvmbridge_activity().capture_text,"whole monitor"));
    if (!strcmp(argv[2],"cursor")) assert(strstr(kvmbridge_activity().capture_text,"left the selected display"));
    bool successful=!strcmp(argv[2],"stopped") ||
                    !strcmp(variant,"cursor_control") || !strcmp(variant,"cursor_data") ||
                    !strcmp(variant,"cursor_freeze") ||
                    !strncmp(variant,"source_timeout_",15) || !strcmp(variant,"mapping_control") ||
                    !strcmp(variant,"mapping_data") || !strncmp(variant,"pointer_position_",17);
    assert(freeze_requested==(!strcmp(variant,"cursor_freeze")));
    assert(binds==(successful ? 1 : 0) && ends==1);
    if (!strcmp(variant,"progress_expiry")) assert(clock_us<INT64_C(126000000) && progress_messages==2);
    if (borrowed || !strcmp(variant,"unpaced")) assert(private_buffers==(borrowed ? 0 : 1));
    kvmbridge_stop();
    assert(!strcmp(kvmbridge_activity().capture_code,argv[2]));
    puts(argv[2]);
    return 0;
}
