// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ptp_test_platform.h"
#include "hid_dispatch.h"
#include "ui_touch_bridge.h"
#include "usb_hid_device.h"
#include <pthread.h>
#include <stdio.h>

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned entered, permits;
static bool exiting;
static void (*task)(void *);
static StaticQueue_t bridge_queue;
static uint8_t queue_storage[4096];
static pointer_reader_t reader;
static lv_indev_t pointer;
static usb_hid_transport_t usb;
static bool inflight[4], pump;
static uint64_t clock_us;
static uint32_t sequence, generation;
static unsigned frames, mouse;
static int wheel_total;
static bool scroll_enabled=true, scroll_natural;
static kvm_gesture_speed_t speed = KVM_GEST_SPEED_NORMAL;
static bool hold_right = true;
static int travel_x;
static unsigned right_down;
static void apply_mouse(kvm_gesture_cfg_t *gesture)
{
    gesture->scroll_enabled = scroll_enabled;
    gesture->scroll_natural = scroll_natural;
    gesture->speed = speed;
    gesture->longpress_right_click = hold_right;
}
static uint8_t last[HPE_PTP_WIRE];
static hpe_features_t features = {.selective=3};

QueueHandle_t xQueueCreate(unsigned cap, unsigned width)
{
    assert(cap * width <= sizeof(queue_storage));
    return xQueueCreateStatic(cap, width, queue_storage, &bridge_queue);
}
bool test_bridge_receive(QueueHandle_t q, void *msg, unsigned wait)
{
    assert(q == &bridge_queue && wait);
    pthread_mutex_lock(&lock);
    ++entered;
    pthread_cond_broadcast(&condition);
    while (!permits && !exiting) pthread_cond_wait(&condition, &lock);
    if (exiting) { pthread_mutex_unlock(&lock); pthread_exit(NULL); }
    --permits;
    bool ok = xQueueReceive(q, msg, 0);
    pthread_mutex_unlock(&lock);
    return ok;
}
bool test_bridge_send(QueueHandle_t q, const void *msg, unsigned wait)
{
    (void)wait;
    assert(q == &bridge_queue);
    pthread_mutex_lock(&lock);
    bool ok = xQueueSend(q, msg, 0);
    pthread_mutex_unlock(&lock);
    return ok;
}
static void *run_task(void *arg) { task(arg); return NULL; }
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned n, void *arg, unsigned prio,
                            void *handle, unsigned cpu)
{
    (void)name; (void)n; (void)prio; (void)handle; (void)cpu;
    task = entry;
    assert(!pthread_create(&thread, NULL, run_task, arg));
    pthread_mutex_lock(&lock);
    while (!entered) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    return pdPASS;
}
static void step_bridge(void)
{
    pthread_mutex_lock(&lock);
    unsigned before = entered;
    ++permits;
    pthread_cond_signal(&condition);
    while (entered == before) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
}
lv_indev_t *lv_indev_create(void) { return &pointer; }
void lv_indev_set_read_cb(lv_indev_t *d, pointer_reader_t cb) { assert(d == &pointer); reader = cb; }
uint32_t esp_random(void) { return 42; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *t) { *t=args->callback; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period) { assert(t && period); return ESP_OK; }
esp_err_t usb_hid_device_set_transport(const usb_hid_transport_t *t) { usb = *t; return ESP_OK; }
void usb_hid_device_request_pump(void) { pump = true; }
uint8_t usb_hid_device_interface_count(void) { return 4; }
uint8_t usb_hid_device_protocol(uint8_t i) { assert(i < 4); return KVM_HID_PROTOCOL_REPORT; }
bool usb_hid_device_send(uint8_t i, const uint8_t *bytes, uint8_t len)
{
    assert(i < 4 && !inflight[i]);
    inflight[i] = true;
    if (i == KVM_HID_ITF_PTP) {
        assert(len == sizeof(last) && bytes[0] == HPE_PTP && bytes[33] <= 5);
        memcpy(last, bytes, len);
        ++frames;
    } else if (i == KVM_HID_ITF_MOUSE) {
        assert(len==KVM_HID_MOUSE_REPORT_LEN);
        wheel_total+=(int8_t)bytes[3];
        travel_x += (int8_t)bytes[1];
        if (bytes[0] & 2) ++right_down;
        for (unsigned k = 0; k < len; ++k) if (bytes[k]) { ++mouse; break; }
    }
    return true;
}
static void service(void)
{
    if (pump) { pump = false; usb.pump(usb.ctx); }
}
static void drain(void)
{
    for (unsigned round = 0; round < 20; ++round) {
        bool any = pump;
        service();
        for (unsigned i = 0; i < 4; ++i) if (inflight[i]) {
            any = true; inflight[i] = false; usb.on_complete(usb.ctx, (uint8_t)i);
        }
        if (!any) return;
    }
    assert(false);
}
static void feature(uint8_t id, uint8_t value)
{
    assert(hpe_feature_set(&features,id,&value,1));
    usb.on_ptp_route(usb.ctx, features.mode == 3, (features.selective & 1) != 0, ++generation);
}
static bool sink(void *ctx, const kvm_hid_intent_t *in) { (void)ctx; return hid_dispatch_local(in) == ESP_OK; }
typedef struct { uint8_t id; uint16_t x, y; } finger_t;
static void sensor(const finger_t *p, unsigned n)
{
    uint8_t wire[5 * GT911_POINT_BYTES] = {0};
    assert(n <= 5);
    for (unsigned i=0; i<n; ++i) {
        uint8_t *q = wire + i*GT911_POINT_BYTES;
        q[0]=p[i].id; q[1]=(uint8_t)p[i].x; q[2]=(uint8_t)(p[i].x>>8);
        q[3]=(uint8_t)p[i].y; q[4]=(uint8_t)(p[i].y>>8);
    }
    kvm_touch_sample_t sample = {.sample_seq=++sequence,.timestamp_us=clock_us};
    const kvm_touch_transform_t transform = {.raw_x_max=1023,.raw_y_max=599,.h_res=1024,.v_res=600};
    kvm_touch_decode_gt911((uint8_t)(GT911_STATUS_READY|n),wire,n*GT911_POINT_BYTES,&transform,&sample);
    ui_touch_bridge_post(&sample);
    step_bridge();
    service();
}
static const uint8_t *slot(uint8_t id)
{
    for (unsigned i=0;i<last[33];++i) if (last[2+6*i]==id) return &last[1+6*i];
    assert(false); return NULL;
}
static unsigned word(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1]<<8); }
static void expect_contact(finger_t p, bool tip)
{
    const uint8_t *c=slot(p.id);
    assert(c[0] == (tip ? 3 : 2));
    assert(word(c+2) == ((unsigned)p.x*32767+511)/1023);
    assert(word(c+4) == ((unsigned)p.y*32767+299)/599);
}

static unsigned pairs;
static bool local_pair(unsigned count, const kvm_touch_point_t points[2], uint64_t timestamp_us)
{
    (void)timestamp_us;
    if (count != 2) return false;
    assert(points[0].track_id < points[1].track_id);
    ++pairs;
    return true;
}

int main(void)
{
    const ui_touch_bridge_cfg_t cfg = {.h_res=1024,.v_res=600,.sink=sink,.hid_enabled=true,.local_contacts=local_pair,
                                       .apply_mouse_preferences=apply_mouse};
    assert(hid_dispatch_start()==ESP_OK && ui_touch_bridge_start(&cfg)==ESP_OK);
    ui_touch_bridge_set_pad(&(touch_rect_t){20,88,1003,495},true);
    step_bridge();
    usb.on_state(usb.ctx,KVM_USB_MOUNTED,1); drain();
    feature(HPE_INPUT_MODE,3); drain();
    clock_us=6553500;
    finger_t five[]={{13,700,270},{7,20,88},{9,1003,495},{4,310,360},{1,500,172}};
    sensor(five,5); drain();
    assert(last[33]==5 && word(last+31)==65535);
    for (unsigned i=0;i<5;++i) expect_contact(five[i],true);
    clock_us+=100;
    for (unsigned i=0;i<2;++i) { finger_t t=five[i]; five[i]=five[4-i]; five[4-i]=t; }
    sensor(five,5); drain();
    assert(last[33]==5 && !word(last+31));
    for (unsigned i=0;i<5;++i) expect_contact(five[i],true);

    // Four lifts and one held contact share a frame. Complete before feature change.
    clock_us+=10000;
    sensor(five,1); drain();
    assert(last[33]==5);
    for (unsigned i=0;i<5;++i) expect_contact(five[i],i==0);
    clock_us+=7300; feature(HPE_SELECT,0); drain();
    assert(last[33]==1 && word(last+31)==(uint16_t)(clock_us/100)); expect_contact(five[0],false);
    feature(HPE_SELECT,3); drain();
    sensor(NULL,0); drain();
    clock_us+=10000; sensor(five,5); drain();
    clock_us+=10000; sensor(five,1);
    assert(inflight[2] && last[33]==5);
    // A failed mixed transfer leaves all five candidate liftoffs outstanding.
    clock_us+=9700; inflight[2]=false; usb.on_failed(usb.ctx,2); drain();
    assert(last[33]==5 && word(last+31)==(uint16_t)(clock_us/100));
    for (unsigned i=0;i<5;++i) expect_contact(five[i],false);
    sensor(NULL,0); drain();

    // The mode changes while the mixed transfer is still in flight. Its successful
    // completion retires the four old IDs even though the logical ledger was cleared.
    clock_us+=10000; sensor(five,5); drain();
    clock_us+=10000; sensor(five,1);
    assert(inflight[2]);
    clock_us+=10400; feature(HPE_INPUT_MODE,0); drain();
    assert(last[33]==1 && word(last+31)==(uint16_t)(clock_us/100)); expect_contact(five[0],false);
    feature(HPE_INPUT_MODE,3); drain();
    sensor(NULL,0); drain();

    clock_us+=10000; sensor(five,1); drain();
    // A newly ready GT911 frame arrives after a gap before the bridge's periodic tick.
    clock_us+=120001; sensor(five,1); drain();
    assert(last[33]==1); expect_contact(five[0],false);
    unsigned before=frames;
    clock_us+=10000; sensor(five,1); drain(); assert(frames==before);
    sensor(NULL,0); drain();
    clock_us+=10000; sensor(five,1); drain(); expect_contact(five[0],true);
    finger_t duplicate[] = {five[0],five[0]};
    clock_us+=10000; sensor(duplicate,2); drain(); expect_contact(five[0],false);
    before=frames;
    clock_us+=10000; sensor(five,1); drain(); assert(frames==before);
    clock_us+=10000; sensor(NULL,0); drain();
    clock_us+=10000; sensor(five,1); drain(); expect_contact(five[0],true);
    clock_us+=130000; step_bridge(); service(); drain(); expect_contact(five[0],false);
    before=frames;
    clock_us+=10000; sensor(five,1); drain(); assert(frames==before);
    clock_us+=10000; sensor(NULL,0); drain();
    const finger_t chrome={12,34,21};
    before=frames; sensor(&chrome,1); drain();
    lv_indev_data_t data;
    reader(&pointer,&data);
    if (data.continue_reading) {
        assert(data.state==LV_INDEV_STATE_RELEASED);
        reader(&pointer,&data);
        assert(!data.continue_reading);
    }
    assert(frames==before && data.state==LV_INDEV_STATE_PRESSED && data.point.x==34 && data.point.y==21);
    ui_touch_bridge_stats_t old, current;
    ui_touch_bridge_get_stats(&old);
    ui_touch_bridge_cancel(); step_bridge();
    ui_touch_bridge_cancel(); step_bridge();
    const finger_t held[] = {chrome, five[0]};
    sensor(held,2); drain();
    reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_RELEASED && !data.continue_reading);
    ui_touch_bridge_get_stats(&current);
    assert(current.indev_resets==old.indev_resets+1 && frames==before);
    sensor(NULL,0); drain();
    sensor(&chrome,1); drain();
    reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_PRESSED && !data.continue_reading);
    ui_touch_bridge_cancel(); step_bridge();
    sensor(NULL,0); drain();
    sensor(&chrome,1); drain(); // same track ID, verified lift, before the next LVGL timer read
    reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_RELEASED && data.continue_reading);
    reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_PRESSED && !data.continue_reading && frames==before);
    sensor(NULL,0); drain();
    hid_dispatch_set_profile(HPE_KEYBOARD_PROFILE); drain();
    before=frames; clock_us+=10000; sensor(five,1); drain();
    assert(frames==before && !mouse);
    sensor(NULL,0); drain();
    hid_dispatch_set_profile(HPE_MOUSE_PROFILE); drain();
    clock_us+=10000; sensor(five,1); drain(); expect_contact(five[0],true);
    clock_us+=10000; sensor(NULL,0); drain(); expect_contact(five[0],false);
    // One captured ID crosses each side and keeps moving beyond the pad, through lift.
    const finger_t stroke[]={{3,500,250},{3,500,80},{3,500,40},{3,1010,40},{3,1023,0},
                             {3,1018,510},{3,1018,599},{3,12,550},{3,0,550}};
    for (unsigned i=0;i<sizeof(stroke)/sizeof(stroke[0]);i++) {
        clock_us+=10000; sensor(&stroke[i],1); drain();
        assert(last[33]==1);
        expect_contact(stroke[i],true);
    }
    clock_us+=10000; sensor(NULL,0); drain();
    expect_contact(stroke[sizeof(stroke)/sizeof(stroke[0])-1],false);
    before=frames;
    unsigned wraps=0;
    unsigned prev_scan=word(last+31);
    for (unsigned i=0; i<2000; ++i) {
        clock_us+=13333;
        five[0].x=(uint16_t)(20+(7*i)%984);
        five[2].y=(uint16_t)(88+(3*i)%408);
        sensor(five,5);
        drain();
        unsigned scan=word(last+31);
        assert(last[33]==5 && scan==(uint16_t)(clock_us/100));
        unsigned delta=(uint16_t)(scan-prev_scan);
        assert(delta==133 || delta==134);
        if (scan<prev_scan) ++wraps;
        prev_scan=scan;
        for (unsigned j=0; j<5; ++j) expect_contact(five[j],true);
    }
    assert(frames==before+2000 && wraps>=4 && !mouse);
    // A mode change must not reinterpret fingers already on the glass as mouse movement.
    feature(HPE_INPUT_MODE,0); drain();
    assert(last[33]==5);
    for (unsigned i=0; i<5; ++i) expect_contact(five[i],false);
    before=frames;
    five[0].x=50; clock_us+=10000; sensor(five,1); drain();
    five[0].x=350; clock_us+=10000; sensor(five,1); drain();
    assert(!mouse && frames==before);
    clock_us+=10000; sensor(NULL,0); drain();
    for (unsigned i=0; i<5; ++i) {
        five[0].x=(uint16_t)(100+30*i);
        clock_us+=10000; sensor(five,1); drain();
    }
    clock_us+=10000; sensor(NULL,0); drain();
    assert(mouse>0 && frames==before);
    // Same GT911 ID, resting on the side rail: repeat mouse motion until reentry or lift.
    finger_t edge={6,24,250};
    clock_us+=10000; sensor(&edge,1); drain();
    edge.x=10;
    clock_us+=10000; sensor(&edge,1); drain();
    for (unsigned i=0;i<4;i++) {
        int previous_x=travel_x;
        clock_us+=20000; sensor(&edge,1); drain();
        assert(travel_x<previous_x);
    }
    edge.x=40;
    clock_us+=10000; sensor(&edge,1); drain();
    unsigned resting=mouse;
    clock_us+=20000; sensor(&edge,1); drain();
    clock_us+=10000; sensor(NULL,0); drain();
    assert(mouse==resting && frames==before);
    // Beginning a new software gesture samples both preferences. Changes mid-swipe
    // must neither reverse that swipe nor turn the lift into a click.
    for (unsigned mode=0; mode<3; ++mode) {
        finger_t contact[2]={{1,200,200},{2,300,200}};
        int prior_wheel=wheel_total;
        unsigned prior_mouse=mouse;
        clock_us+=10000; sensor(contact,2); drain();
        if (mode==0) scroll_natural=true;
        if (mode==1) scroll_enabled=false;
        contact[0].y+=48; contact[1].y+=48;
        clock_us+=10000; sensor(contact,2); drain();
        clock_us+=10000; sensor(NULL,0); drain();
        assert(wheel_total-prior_wheel==(mode==0 ? -2 : mode==1 ? 2 : 0));
        if (mode==2) assert(prior_mouse==mouse);
    }
    // Slow, normal and fast must produce different wire deltas for an identical swipe.
    int distance[3];
    for (unsigned mode = 0; mode < 3; ++mode) {
        speed = (kvm_gesture_speed_t)mode;
        finger_t finger = {1,200,200};
        int start_x = travel_x;
        clock_us += 10000; sensor(&finger,1); drain();
        speed = KVM_GEST_SPEED_FAST; // applies after the next full lift
        for (unsigned i = 0; i < 5; ++i) {
            finger.x += 12;
            clock_us += 50000; sensor(&finger,1); drain();
        }
        clock_us += 10000; sensor(NULL,0); drain();
        distance[mode] = travel_x - start_x;
    }
    assert(distance[0] > 0 && distance[0] < distance[1] && distance[1] < distance[2]);
    for (unsigned mode = 0; mode < 2; ++mode) {
        finger_t finger = {1,200,200};
        const unsigned start_right = right_down, start_mouse = mouse;
        hold_right = mode != 0;
        clock_us += 10000; sensor(&finger,1); drain();
        hold_right = !hold_right;
        for (unsigned i = 0; i < 12; ++i) {
            clock_us += 50000; sensor(&finger,1); drain();
        }
        clock_us += 10000; sensor(NULL,0); drain();
        assert(right_down - start_right == mode);
        if (!mode) assert(mouse == start_mouse);
    }
    printf("Software mouse: slow/normal/fast X deltas %d/%d/%d, hold right-click off/on verified.\n",
           distance[0], distance[1], distance[2]);
    assert(pairs==0);
    ui_touch_bridge_set_pad(NULL,false); step_bridge();
    ui_touch_bridge_set_hid_enabled(false); step_bridge();
    sensor(NULL,0); drain(); reader(&pointer,&data);
    before=frames;
    unsigned previous_mouse=mouse;
    clock_us+=10000; sensor(five,1); reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_PRESSED);
    clock_us+=10000; sensor(five,2); reader(&pointer,&data);
    assert(pairs==1 && data.state==LV_INDEV_STATE_RELEASED);
    clock_us+=10000; sensor(five,1); reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_RELEASED);
    clock_us+=10000; sensor(NULL,0); reader(&pointer,&data);
    clock_us+=10000; sensor(five,1); reader(&pointer,&data);
    assert(data.state==LV_INDEV_STATE_PRESSED && mouse==previous_mouse && frames==before);
    puts("Local chart pair captured on the LVGL reader, tail suppressed, fresh single touch accepted.");
    pthread_mutex_lock(&lock); exiting=true; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    assert(!pthread_join(thread,NULL));
    puts("PTP PASS: 5 raw GT911 IDs, reordered records, scan 65535->0, axis bounds and last positions.");
    puts("PTP PASS: mixed transfer ACK, transport failure and mode switch in flight; exact liftoff sets 1/5/1.");
    puts("PTP PASS: ready-frame gap, no-scan watchdog, corrupt duplicate sensor ID, full-lift rearm, chrome and profile gate.");
    printf("PTP PASS: 2000 five-contact scans >=75 Hz modeled, 10000 coordinate/ID checks, %u clock wraps; "
           "mode 0 rearmed IF1 only after a full physical lift.\n", wraps);
    return 0;
}
