// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "test_port.h"
#include "media_decode.h"
#include "usb_jpeg_decode.h"
#include "monitor_present.h"
#include "monitor_wire.h"
#ifdef KVM_USB_PICTURE_CHECK
#include "usb_jpeg_check.h"
#endif
#include <stdatomic.h>
#include <string.h>
#include <time.h>

struct test_task { pthread_t id; StaticSemaphore_t wake; void (*entry)(void *); };
static struct test_task worker;
static bool first_start = true;
static unsigned input_capacity = 3;
static bool native_records, refuse_stripe;
enum { NATIVE_CELLS=1024*600/128, TAIL_CELLS=1024*8/128 };
static const uint16_t *padding_cells;
static bool expect_clear_padding;
static unsigned padded_decodes,failed_padded_decodes;
static atomic_bool hold;
static atomic_uint inside;
static atomic_bool hold_completed_decode;
static atomic_uint at_return, worker_waits;
static atomic_uint decoder_calls;
static bool fail_native_restore;
static const uint16_t *visible;
static unsigned selects;
static bool selected_full;
static monitor_damage_t selected_area;
static monitor_damage_t second_area;
static unsigned selected_count;
static unsigned jpeg_allocations, live_allocations;

static void delay(void)
{
    struct timespec interval = {0, 1000000};
    nanosleep(&interval, NULL);
}

static void *run_worker(void *arg)
{
    worker.entry(arg);
    return NULL;
}

int xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *), const char *name, unsigned bytes, void *arg,
                                   unsigned priority, TaskHandle_t *task, int core, unsigned caps)
{
    assert(name && bytes && !arg && priority && core == 1 && caps);
    if (first_start) test_semaphore(&worker.wake, 0);
    first_start = false;
    worker.entry = entry;
    *task = &worker;
    assert(!pthread_create(&worker.id, NULL, run_worker, NULL));
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &worker);
    xSemaphoreGive(&worker.wake);
}

void ulTaskNotifyTake(int clear, unsigned wait)
{
    assert(clear && wait == portMAX_DELAY);
    atomic_fetch_add(&worker_waits, 1);
    pthread_mutex_lock(&worker.wake.lock);
    while (!worker.wake.count) pthread_cond_wait(&worker.wake.event, &worker.wake.lock);
    worker.wake.count = 0; // FreeRTOS clears all accumulated notifications on take(pdTRUE).
    pthread_mutex_unlock(&worker.wake.lock);
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (size == MONITOR_JPEG_LIMIT) {
        if (++jpeg_allocations > input_capacity) return NULL;
    } else if (size == 1024 * 16 * sizeof(uint16_t)) {
        if (refuse_stripe) return NULL;
    } else {
        assert(size == MONITOR_WIDTH * MONITOR_HEIGHT * sizeof(uint16_t) || size == 1024 * 608 * sizeof(uint16_t));
    }
    void *p = malloc(size);
    assert(p);
    ++live_allocations;
    return p;
}

void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{
    assert(alignment == 16);
    return heap_caps_malloc(size,caps);
}

void heap_caps_free(void *ptr)
{
    assert(ptr && live_allocations);
    --live_allocations;
    free(ptr);
}

void kvm_media_zeroize(void *ptr, size_t size) { memset(ptr, 0, size); }
kvm_media_decoder_t *kvm_media_decoder_open(kvm_live_codec_t codec)
{
    assert(codec == KVM_LIVE_JPEG);
    return (kvm_media_decoder_t *)&worker;
}
void kvm_media_decoder_close(kvm_media_decoder_t *decoder) { assert(decoder == (void *)&worker); }

enum { PAYLOAD = 32, HEADER = 16 };
static bool decoded_nonblack;
bool kvm_media_usb_monitor_has_nonblack(const kvm_media_decoder_t *decoder)
{
    assert(decoder);
    return decoded_nonblack;
}

enum { CURSOR_LEFT=206, CURSOR_RIGHT, STAGGERED, CROSS, DENSE, EDGE_LEFT, EDGE_RIGHT,
       FOUR_ISLANDS, DIAGONAL_PAIR };
static uint16_t shape_pixel(unsigned id, unsigned x, unsigned y, unsigned width, unsigned height)
{
    bool bright=false;
    switch (id) {
    case CURSOR_LEFT: case CURSOR_RIGHT: {
        unsigned left=id==CURSOR_LEFT ? 32 : 480;
        bright=x>=left && x<left+16 && y>=32 && y<48;
        break;
    }
    case STAGGERED:
        // No one row describes all islands, and an isolated pixel falls inside the first row's gap.
        bright=(y>=13 && y<28 && (x<11 || x>=width-7)) ||
               (y==30 && x==width/2+3) ||
               (y>=79 && y<96 && ((x>=55 && x<70) || (x>=width-63 && x<width-57)));
        break;
    case CROSS: bright=x==width/2+1 || y==height/2-3; break;
    case DENSE: bright=(3*x+5*y)%7!=0; break;
    case EDGE_LEFT: bright=x<13 && y>=height-21; break;
    case EDGE_RIGHT: bright=x>=width-13 && y>=height-21; break;
    case FOUR_ISLANDS:
        bright=((x>=32 && x<40) || (x>=480 && x<488)) &&
               ((y>=20 && y<28) || (y>=60 && y<68));
        break;
    case DIAGONAL_PAIR:
        bright=(x>=32 && x<40 && y>=20 && y<28) || (x>=480 && x<488 && y>=60 && y<68);
        break;
    }
    return bright ? 0xffff : 200;
}

bool kvm_media_usb_monitor_jpeg(kvm_media_decoder_t *decoder, const uint8_t *input, size_t size,
                                const kvm_media_target_t *target)
{
    assert(decoder && (size == PAYLOAD || size == PAYLOAD - 1));
    atomic_fetch_add(&decoder_calls, 1);
    unsigned id = input[0];
    atomic_store(&inside, id);
    while (atomic_load(&hold) && !target->cancelled(target->ctx)) delay();
    for (size_t i = 0; i < size - (id == 220); ++i) assert(input[i] == id);
    atomic_store(&inside, 0);
    if (target->cancelled(target->ctx)) return false;
    for (size_t i = 0; i < target->width * target->height; ++i)
        target->pixels[i] = (uint16_t)(id >= 200 ? 200 : id);
    if (id>=CURSOR_LEFT && id<=DIAGONAL_PAIR) {
        for (unsigned y=0;y<target->height;y++) for (unsigned x=0;x<target->width;x++)
            target->pixels[y*target->width+x]=shape_pixel(id,x,y,target->width,target->height);
    }
    if (id == 201 || id == 202 || id == 204) target->pixels[target->width * target->height - 1] = 0;
    if (id == 220) target->pixels[target->width * target->height - 1] = (uint16_t)(size * 256 + input[size - 1]);
    if (id == 202) target->pixels[17 * target->width + 33] = 0;
    if (id == 203 || (native_records && id == 200 && fail_native_restore)) {
        memset(target->pixels, 0x55, 16 * target->width * sizeof(*target->pixels));
        return false;
    }
    decoded_nonblack = id != 0;
    if (atomic_load(&hold_completed_decode)) {
        atomic_store(&at_return, id);
        while (atomic_load(&hold_completed_decode)) delay();
        atomic_store(&at_return, 0);
    }
    return true;
}

usb_jpeg_decode_result_t usb_jpeg_decode(uint8_t *input, size_t size, const kvm_media_target_t *target,
                                        size_t capacity, bool *nonblack, usb_jpeg_damage_t *tracking)
{
    assert(capacity >= target->stride * target->height * sizeof(*target->pixels));
    kvm_media_decoder_t *fixture=(kvm_media_decoder_t *)target;
    kvm_media_target_t decoded = *target;
    bool in_place = tracking && tracking->stripe;
    if (in_place) {
        decoded.pixels = malloc(capacity); // The deterministic test decoder emits a full picture in one call.
        assert(decoded.pixels && tracking->previous == target->pixels);
    }
    bool ok = kvm_media_usb_monitor_jpeg(fixture,input,size,&decoded);
    if (target->width==1024 && !tracking) {
        // A direct final MCU block may write all 16 rows, including 600..607.
        // A vendor failure after writing output must invalidate these tags too.
        const size_t visible_pixels=target->width*target->height;
        assert(capacity==(visible_pixels+8*1024)*sizeof(uint16_t));
        memset(target->pixels+visible_pixels,ok ? 0xa5 : 0x5a,8*1024*sizeof(uint16_t));
        ++padded_decodes;
        if (!ok) ++failed_padded_decodes;
    }
    if (tracking) {
        if (in_place) {
            const uintptr_t p=(uintptr_t)tracking->cells;
            assert(p>=(uintptr_t)(target->pixels+target->width*target->height) &&
                   p+NATIVE_CELLS*sizeof(uint16_t)<=(uintptr_t)target->pixels+capacity);
            padding_cells=tracking->cells;
            if (expect_clear_padding) {
                for (unsigned i=0;i<NATIVE_CELLS;i++) assert(!padding_cells[i]);
                expect_clear_padding=false;
            }
        }
        unsigned valid = ok ? target->height : 16; // Late decoder error after at least one committed band.
        for (unsigned y = 0; y < valid; y += 16) {
            unsigned lines = valid - y < 16 ? valid - y : 16;
            size_t offset = (size_t)y * target->width;
            usb_jpeg_damage_band(tracking, decoded.pixels + offset, in_place ? target->pixels + offset : NULL,
                                 target->width, y, lines);
        }
    }
    if (in_place) free(decoded.pixels);
    if (!ok) return USB_JPEG_INVALID;
    *nonblack=kvm_media_usb_monitor_has_nonblack(fixture);
    return USB_JPEG_OK;
}

#ifdef KVM_USB_PICTURE_CHECK
usb_jpeg_result_t usb_jpeg_check_rom(const uint8_t *input,size_t size,const kvm_media_target_t *target)
{
    assert(target);
    return usb_jpeg_check(input,size);
}
#endif

bool kvm_media_usb_monitor_jpeg_fit(kvm_media_decoder_t *d, const uint8_t *p, size_t n,
                                    const kvm_media_target_t *t, unsigned width, unsigned height)
{
    assert(width == 640 && height == 480 && t->width == 1024 && t->height == 600);
    if (p[0] == 40) return false;  // This fixture stands for a native-sized JPEG.
    return kvm_media_usb_monitor_jpeg(d, p, n, t);
}

static void feed_encoding(unsigned id, size_t payload_size, unsigned last, size_t start, size_t end)
{
    uint8_t record[HEADER + PAYLOAD] = {0, 0, 3, 0, 0, 0, 0, 0, 0x80, 2, 0xe0, 1, 0, PAYLOAD * 4};
    assert(payload_size >= 4 && payload_size <= PAYLOAD && start < end && end <= HEADER + payload_size);
    if (native_records) {
        record[8] = 0; record[9] = 4;
        record[10] = 0x58; record[11] = 2;
    }
    record[13] = (uint8_t)(payload_size * 4);
    memset(record + HEADER, (int)id, payload_size);
    record[HEADER + payload_size - 1] = (uint8_t)last;
    usb_monitor_feed(record + start, end - start);
}

static void feed(unsigned id, size_t start, size_t end) { feed_encoding(id, PAYLOAD, id, start, end); }
static void frame(unsigned id) { feed(id, 0, HEADER + PAYLOAD); }
static void select_frame(const uint16_t *pixels, bool has_nonblack, const monitor_damage_t *damage, unsigned count)
{
    assert(has_nonblack == (pixels != NULL && pixels[0] != 0));
    ++selects;
    visible = pixels;
    selected_full = damage == NULL;
    assert(count <= 2);
    selected_count = count;
    selected_area = count ? damage[0] : (monitor_damage_t){0, 0, -1, -1};
    if (count == 2) second_area = damage[1];
}
static void check_pixels(unsigned id)
{
    assert(visible);
    for (size_t i = 0; i < MONITOR_WIDTH * MONITOR_HEIGHT; ++i) assert(visible[i] == id);
}

static void wait_decode(unsigned id)
{
    for (unsigned i = 0; i < 2000 && atomic_load(&inside) != id; ++i) delay();
    assert(atomic_load(&inside) == id);
}

static void wait_completed(unsigned count)
{
    for (unsigned i = 0; i < 2000 && usb_monitor_stats().decoded != count; ++i) delay();
    assert(usb_monitor_stats().decoded == count);
}

static void stop(void)
{
    visible = NULL; // The LVGL owner detaches the descriptor before stopping.
    monitor_stop();
    assert(!pthread_join(worker.id, NULL) && !live_allocations);
}

static void check_reconnect(unsigned slots)
{
    input_capacity = slots;
    jpeg_allocations = 0;
    atomic_store(&hold, false);
    assert(monitor_start(false) && usb_monitor_stats().input_slots == slots);
    frame(50);
    wait_completed(1);
    monitor_present(select_frame);
    check_pixels(50);

    unsigned expected = 1;
    for (unsigned split = 1; split < HEADER + PAYLOAD; ++split) {
        feed(51, 0, split);
        usb_monitor_reset();
        monitor_present(select_frame);
        assert(!visible);
        feed(52, 0, split);
        feed(52, split, HEADER + PAYLOAD);
        wait_completed(++expected);
        monitor_present(select_frame);
        check_pixels(52);
    }
    // USB reset can arrive after the decoder's final cancellation check, before its successful return.
    atomic_store(&hold_completed_decode, true);
    frame(53);
    for (unsigned i = 0; i < 2000 && atomic_load(&at_return) != 53; ++i) delay();
    assert(atomic_load(&at_return) == 53);
    unsigned waits = atomic_load(&worker_waits);
    usb_monitor_reset();
    // Split the first new record at its header. No JPEG bytes require storage yet.
    feed(54, 0, HEADER);
    if (slots > 1) feed(54, HEADER, HEADER + PAYLOAD);
    monitor_present(select_frame);
    assert(!visible && usb_monitor_stats().decoded == expected);
    atomic_store(&hold_completed_decode, false);
    if (slots == 1) {
        // The old task gives back the only input before any new JPEG bytes arrive.
        for (unsigned i = 0; i < 2000 && atomic_load(&worker_waits) == waits; ++i) delay();
        assert(atomic_load(&worker_waits) != waits);
        monitor_present(select_frame);
        assert(!visible && usb_monitor_stats().decoded == expected);
        feed(54, HEADER, HEADER + PAYLOAD);
    }
    assert(usb_monitor_stats().busy_drops == 0);
    wait_completed(++expected);
    monitor_present(select_frame);
    check_pixels(54);
    usb_monitor_stats_t stats = usb_monitor_stats();
    assert(stats.queued == expected + 1 && stats.decoded == expected && !stats.superseded && !stats.decode_errors);
    assert(!stats.busy_drops);
    stop();
    printf("PASS reconnect with %u JPEG slot(s), %u partial-record boundaries and late decoder success\n",
           slots, HEADER + PAYLOAD - 1);
}

#ifdef KVM_USB_PICTURE_CHECK
static atomic_bool hold_reference;
static atomic_uint reference_calls;

usb_jpeg_result_t usb_jpeg_check(const uint8_t *input, size_t size)
{
    assert(size == PAYLOAD);
    unsigned id = input[0];
    atomic_fetch_add(&reference_calls, 1);
    while (atomic_load(&hold_reference)) delay();
    for (size_t i = 0; i < size; ++i) assert(input[i] == id);
    return id == 42 ? USB_JPEG_FAILED : USB_JPEG_ALL_BLACK;
}

static void check_trial_reset(void)
{
    assert(!atomic_load(&reference_calls)); // Ordinary monitor sessions did not invoke the second decoder.
    input_capacity = 3;
    jpeg_allocations = 0;
    assert(monitor_start(false));
    usb_monitor_enable_picture_check();
    atomic_store(&hold, false);
    frame(40);
    wait_completed(1);
    monitor_present(select_frame);
    check_pixels(40);
    usb_monitor_picture_t pair = monitor_checked_picture(visible, "has_nonblack");
    assert(pair.sequence == 1 && !strcmp(pair.reference, "all_black"));
    atomic_store(&hold_reference, true);
    frame(41);
    for (unsigned i = 0; i < 2000 && atomic_load(&reference_calls) < 2; ++i) delay();
    assert(atomic_load(&reference_calls) == 2);
    frame(43);
    usb_monitor_reset();
    assert(!monitor_checked_picture(visible, "has_nonblack").sequence); // Even before the LVGL timer runs.
    frame(42);
    atomic_store(&hold_reference, false);
    wait_completed(2);
    monitor_present(select_frame);
    check_pixels(42);
    pair = monitor_checked_picture(visible, "has_nonblack");
    assert(pair.sequence == 2 && !strcmp(pair.reference, "failed") && !strcmp(pair.decoded, "has_nonblack"));
    stop();
    assert(!monitor_checked_picture(NULL, "absent").sequence);
    puts("PASS independent decoder uses immutable input, pairs with selected slot, drops reset work, preserves ROM output on failure");
}
#endif

static void check_vga_damage(bool accelerated)
{
    jpeg_allocations = 0;
    input_capacity = accelerated ? 4 : 3;
    assert(monitor_start(false));
    unsigned count = 0;
    const unsigned frames[] = {200, 201, 204, 202, 200, 200}; // 201/204 have equal pixels, different bytes.
    for (unsigned i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
        if (i == 3) {
            frame(203);
            for (unsigned j = 0; j < 2000 && !usb_monitor_stats().decode_errors; ++j) delay();
            assert(usb_monitor_stats().decode_errors == 1 && visible[MONITOR_WIDTH * MONITOR_HEIGHT - 1] == 0);
        }
        if (i == 5) usb_monitor_reset(); // New input can decode before the pending clear is selected.
        frame(frames[i]);
        wait_completed(++count);
        monitor_present(select_frame);
        assert(selected_full == (i == 0 || i == 5));
        if (i == 1) assert(selected_area.x1 == 639 && selected_area.x2 == 639 &&
                            selected_area.y1 == 479 && selected_area.y2 == 479);
        if (i == 2) assert(selected_area.x2 < selected_area.x1);
        if (i == 3) assert(selected_area.x1 == 33 && selected_area.x2 == 33 &&
                            selected_area.y1 == 17 && selected_area.y2 == 17);
        if (i == 4) assert(selected_count == 2 && selected_area.x1 == 33 && selected_area.y1 == 17 &&
                           selected_area.x2 == 33 && selected_area.y2 == 17 &&
                           second_area.x1 == 639 && second_area.y1 == 479 &&
                           second_area.x2 == 639 && second_area.y2 == 479);
    }
    assert(usb_monitor_stats().accelerated == (accelerated ? count : 0));
    stop();
    printf("PASS VGA %s damage against selected surface, identical, last pixel, disjoint, partial failure, reset\n",
           accelerated ? "accelerated" : "ROM");
}

static void check_native_damage(bool with_stripe)
{
    jpeg_allocations = 0;
    input_capacity = 5;
    native_records = true;
    refuse_stripe = !with_stripe;
    padding_cells=NULL;
    const unsigned before=padded_decodes;
    assert(monitor_start(true));
    frame(200);
    monitor_present(select_frame);
    assert(selected_full && visible && visible[0] == 200);
    assert(padded_decodes==before+1);
    expect_clear_padding=with_stripe;
    frame(201);
    monitor_present(select_frame);
    assert(selected_full == !with_stripe && visible[1024*600-1] == 0);
    if (with_stripe) assert(selected_area.x1 == 1023 && selected_area.x2 == 1023 &&
                            selected_area.y1 == 599 && selected_area.y2 == 599);
    assert(!expect_clear_padding && (padding_cells!=NULL)==with_stripe);
    if (padding_cells) {
        // Only the final 8-row stripe was overwritten. Earlier stripe tags remain usable.
        assert(padding_cells[NATIVE_CELLS-TAIL_CELLS-1]!=0);
        for (unsigned i=NATIVE_CELLS-TAIL_CELLS;i<NATIVE_CELLS;i++) assert(!padding_cells[i]);
    }
    const unsigned before_rollback=padded_decodes;
    frame(203);
    monitor_present(select_frame);
    assert(usb_monitor_stats().decode_errors == 1 && visible[0] == 200 && visible[1024*600-1] == 0);
    // The failed tracked update is followed by a direct rollback decode. Without
    // a stripe both the failed attempt and the restored frame use the padded target.
    assert(padded_decodes==before_rollback+(with_stripe ? 1u : 2u));
    if (padding_cells) for (unsigned i=0;i<NATIVE_CELLS;i++) assert(!padding_cells[i]);
    frame(204);
    monitor_present(select_frame);
    assert(selected_full == !with_stripe && (!with_stripe || selected_area.x2 < selected_area.x1));
    usb_monitor_reset();
    frame(204);
    monitor_present(select_frame);
    assert(selected_full && usb_monitor_stats().accelerated == 4);
    visible = NULL;
    padding_cells=NULL;
    monitor_stop();
    assert(!live_allocations);
    native_records = refuse_stripe = false;
    printf("PASS native accelerated selected surface with %s: first, final pixel, failed JPEG rollback, repeat, reset\n",
           with_stripe ? "padding-resident cache" : "full repaint on unavailable stripe");
}

static void check_byte_scans(void)
{
    const size_t sizes[] = {0,1,2,3,7,15,16,17,30,31,32,33,34,47,48,63,64,65,97,511};
    for (unsigned ai=0;ai<4;ai++) for (unsigned bi=0;bi<4;bi++) {
        for (unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
            size_t size=sizes[n];
            // Place the final byte directly against the ASan allocation boundary.
            uint8_t *base_a=malloc(size+ai ? size+ai : 1);
            uint8_t *base_b=malloc(size+bi ? size+bi : 1);
            assert(base_a && base_b);
            memset(base_a,0xff,ai);
            memset(base_b,0xff,bi);
            uint8_t *a=base_a+ai, *b=base_b+bi;
            memset(a,0,size);
            memset(b,0,size);
            assert(usb_jpeg_equal_bytes(a,b,size) && !usb_jpeg_any_nonzero(b,size));
            for (size_t byte=0;byte<size;byte++) {
                b[byte]=(uint8_t)(1u<<(byte%8));
                assert(usb_jpeg_equal_bytes(a,b,size)==(memcmp(a,b,size)==0));
                assert(usb_jpeg_any_nonzero(b,size));
                b[byte]=0;
            }
            for (size_t i=0;i<size;i++) a[i]=b[i]=(uint8_t)(i*131u+17u);
            assert(usb_jpeg_equal_bytes(a,b,size) && usb_jpeg_any_nonzero(a,size)==(size!=0));
            if (size>=8) {
                // Two equal bit changes at the same offset in different words must not cancel.
                b[1]^=0x40; b[5]^=0x40;
                assert(!usb_jpeg_equal_bytes(a,b,size));
            }
            free(base_a);
            free(base_b);
        }
    }
    puts("PASS RGB565 word scans: 320 alignment/length pairs, every differing byte, odd tails and equal-bit changes");
}

static void check_cached_cell_colors(void)
{
    enum { pixels=128 };
    _Alignas(16) uint16_t old[pixels],fresh[pixels];
    for (unsigned color=0;color<=UINT16_MAX;color++) {
        uint16_t cell=0;
        for (unsigned i=0;i<pixels;i++) old[i]=fresh[i]=(uint16_t)color;
        const unsigned changed=color%pixels;
        const uint16_t bit=(uint16_t)(1u<<((color/pixels)%16));
        if (color%2) {
            fresh[changed]^=bit;
            assert(!usb_jpeg_cached_band_equal(&cell,old,fresh,pixels));
            fresh[changed]^=bit;
        }
        assert(usb_jpeg_cached_band_equal(&cell,old,fresh,pixels));
        fresh[changed]^=bit;
        assert(!usb_jpeg_cached_band_equal(&cell,old,fresh,pixels));
        fresh[changed]^=bit;
        assert(usb_jpeg_cached_band_equal(&cell,old,fresh,pixels));
    }
    puts("PASS native JPEG cache: all 65536 uniform RGB565 colors, single-pixel differences across every position and bit");
}

static void check_cached_band_rows(unsigned rows)
{
    const size_t width=1024, pixels=width*rows, bytes=pixels*sizeof(uint16_t);
    const size_t cells_count=pixels/128;
    // Exact-sized allocations let ASan catch a final 8-row band read as a full 16 rows.
    uint16_t *old=malloc(bytes),*fresh=malloc(bytes);
    uint16_t *cells=calloc(cells_count,sizeof(*cells));
    assert(old && fresh && cells);
    uint32_t seed=0x1b873593u;
    for (size_t i=0;i<pixels;i++) {
        seed=1664525u*seed+1013904223u;
        // Interleave constant and varying cells, including completely black and white.
        unsigned cell=(unsigned)(i/128);
        const uint16_t flat[]={0,UINT16_MAX,0x07e0,0xf81f};
        old[i]=cell%2 ? (uint16_t)(seed>>16) : flat[(cell/2)%4];
    }
    memcpy(fresh,old,bytes);
    assert(usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    for (size_t cell=0;cell<cells_count;cell++) {
        const size_t offsets[]={0,1,63,64,126,127};
        for (unsigned j=0;j<sizeof(offsets)/sizeof(offsets[0]);j++) {
            size_t i=128*cell+offsets[j];
            fresh[i]^=(uint16_t)(1u<<(cell%16));
            assert(usb_jpeg_cached_band_equal(cells,old,fresh,pixels)==(memcmp(old,fresh,bytes)==0));
            fresh[i]=old[i];
        }
    }
    // Permuting two interior pixels leaves the first/last pixels, sum, XOR and histogram unchanged.
    size_t a=128+29,b=128+85;
    assert(old[a]!=old[b]);
    fresh[a]=old[b];fresh[b]=old[a];
    assert(!usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    memcpy(old,fresh,bytes);
    memset(cells,0,cells_count*sizeof(*cells));
    assert(usb_jpeg_cached_band_equal(cells,old,fresh,pixels));

    // The caller overwrites the old surface after damage and invalidates the entire band's cache.
    old[1]=fresh[1]=0xf800; // previously uniform first cell is now mixed
    for (size_t i=128;i<256;i++) old[i]=fresh[i]=0x001f; // previously mixed cell is now flat
    memset(cells,0,cells_count*sizeof(*cells));
    assert(usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    fresh[1]=0;
    assert(!usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    fresh[1]=old[1]; fresh[197]=0;
    assert(!usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    fresh[197]=old[197];
    assert(usb_jpeg_cached_band_equal(cells,old,fresh,pixels));
    free(old);free(fresh);free(cells);
    printf("PASS native JPEG cache: 1024x%u mixed pixels, edge differences, permutations and reset\n",rows);
}

static void check_adaptive_cache(void)
{
    enum { width=1024, cell=128, half=8*width, count=2*half/cell, middle=count/2 };
    _Alignas(16) static uint16_t old[2*half],fresh[2*half];
    uint16_t tags[count];

    // Test the strict cutoff with a full 8-row band, including a mismatch before
    // the first successful comparison. Every mixed cell has one different pixel.
    for (unsigned flat=middle/8-1;flat<=middle/8;flat++) {
        for (unsigned i=0;i<half;i++)
            old[i]=i/cell<flat ? 0x07e0 : (i%cell==61 ? 0x1234 : 0x8002);
        memcpy(fresh,old,half*sizeof(*old));
        memset(tags,0,sizeof(tags));
        fresh[half-1]^=1;
        assert(!usb_jpeg_cached_band_equal(tags,old,fresh,half) && tags[0]!=2);
        fresh[half-1]^=1;
        assert(usb_jpeg_cached_band_equal(tags,old,fresh,half));
        assert((tags[0]==2)==(flat<middle/8));
        fresh[half-1]^=1;
        assert(!usb_jpeg_cached_band_equal(tags,old,fresh,half));
    }

    // One flat upper half and one textured lower half. The lower band's marker
    // becomes an interior tag when the next JPEG switches from 8 to 16 rows.
    for (unsigned i=0;i<half;i++) {
        old[i]=0xf800;
        old[half+i]=i%cell==61 ? 0x1234 : 0x8002;
    }
    for (unsigned i=0;i<3*cell;i++) old[cell+i]=(uint16_t)(0x8000+i/cell);
    memcpy(fresh,old,sizeof(old));
    memset(tags,0,sizeof(tags));
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,half));
    assert(tags[0]!=2 && tags[1]==1 && tags[2]==1 && tags[3]==1);
    assert(usb_jpeg_cached_band_equal(tags+middle,old+half,fresh+half,half) && tags[middle]==2);
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,2*half));
    fresh[half+61]=0x8002; // This one pixel makes the entire formerly mixed cell solid 0x8002.
    assert(!usb_jpeg_cached_band_equal(tags,old,fresh,2*half));
    fresh[half+61]=old[half+61];

    // Conversely, a marker at the start of an 8-row block must cover the
    // requested 16 rows when merged, and only 8 rows after splitting again.
    memcpy(old,old+half,half*sizeof(*old));
    memcpy(fresh,old,sizeof(old));
    memset(tags,0,sizeof(tags));
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,half) && tags[0]==2);
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,2*half));
    fresh[2*half-1]^=1;
    assert(!usb_jpeg_cached_band_equal(tags,old,fresh,2*half));
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,half));
    assert(!usb_jpeg_cached_band_equal(tags+middle,old+half,fresh+half,half));
    fresh[2*half-1]^=1;
    assert(usb_jpeg_cached_band_equal(tags+middle,old+half,fresh+half,half) && tags[middle]==2);

    // Reset the decoder's history, learn the hint again and switch to a flat
    // image via normal writeback. This change must invalidate the learned hint.
    memset(tags,0,sizeof(tags));
    assert(usb_jpeg_cached_band_equal(tags,old,fresh,2*half) && tags[0]==2);
    for (unsigned i=0;i<2*half;i++) fresh[i]=0x001f;
    usb_jpeg_damage_t damage={.previous=old,.cells=tags,.bounds={0,0,-1,-1}};
    usb_jpeg_damage_band(&damage,fresh,old,width,0,16);
    assert(!memcmp(old,fresh,sizeof(old)) && damage.bounds.x1==0 && damage.bounds.x2==width-1 &&
           damage.bounds.y1==0 && damage.bounds.y2==15);
    for (unsigned i=0;i<count;i++) assert(!tags[i]);
    damage.bounds=(monitor_damage_t){0,0,-1,-1};
    usb_jpeg_damage_band(&damage,fresh,old,width,0,16);
    assert(damage.bounds.x2<damage.bounds.x1);
    for (unsigned i=0;i<count;i++) assert(tags[i]==(0x001f^0x8000));
    puts("PASS adaptive JPEG cache: strict 1/8 cutoff, reserved RGB565, mixed markers across 8/16 rows, reset to flat");
}

static void check_motion_pipeline(bool native, bool accelerated)
{
    unsigned width=monitor_width(native), height=monitor_height(native);
    native_records=native;
    input_capacity=native ? 5 : accelerated ? 4 : 3;
    jpeg_allocations=0;
    assert(monitor_start(native));
    const unsigned sequence[]={CURSOR_LEFT,CURSOR_RIGHT,CURSOR_LEFT,200,STAGGERED,200,CROSS,200,
                               DENSE,200,EDGE_LEFT,EDGE_RIGHT,EDGE_LEFT,200,FOUR_ISLANDS,200,DIAGONAL_PAIR};
    for (unsigned i=0;i<sizeof(sequence)/sizeof(sequence[0]);i++) {
        frame(sequence[i]);
        if (!native) wait_completed(i+1);
        monitor_present(select_frame);
        assert(visible && selected_full==(i==0));
        monitor_damage_t boxes[2]={selected_area,second_area};
        unsigned changed=0,painted=0;
        for (unsigned j=0;j<selected_count;j++) {
            monitor_damage_t b=boxes[j];
            assert(b.x1>=0 && b.x1<=b.x2 && (unsigned)b.x2<width);
            assert(b.y1>=0 && b.y1<=b.y2 && (unsigned)b.y2<height);
            painted+=(unsigned)(b.x2-b.x1+1)*(unsigned)(b.y2-b.y1+1);
        }
        for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
            uint16_t pixel=shape_pixel(sequence[i],x,y,width,height);
            assert(visible[y*width+x]==pixel); // Also verifies the in-place native writeback.
            if (!i || pixel==shape_pixel(sequence[i-1],x,y,width,height)) continue;
            bool covered=false;
            for (unsigned j=0;j<selected_count;j++) {
                monitor_damage_t b=boxes[j];
                covered |= x>=(unsigned)b.x1 && x<=(unsigned)b.x2 && y>=(unsigned)b.y1 && y<=(unsigned)b.y2;
            }
            assert(covered);
            changed++;
        }
        if (i) assert(changed>0);
        // Two boxes whose summed area is exactly the number of changed pixels cannot
        // overlap or include unchanged pixels. Every changed pixel was checked above.
        if (i==1 || i==2) assert(selected_count==2 && changed==512 && painted==changed);
        if (i==11 || i==12) assert(selected_count==2 && changed==546 && painted==changed);
        // Both axes qualify. For four islands use the 768-pixel column pair instead of
        // the 7296-pixel row pair. Diagonal islands favor two exact rows (128 vs 768).
        if (sequence[i]==FOUR_ISLANDS) assert(selected_count==2 && changed==256 && painted==768);
        if (sequence[i]==DIAGONAL_PAIR) assert(selected_count==2 && changed==128 && painted==128);
    }
    assert(usb_monitor_stats().accelerated==(accelerated ? sizeof(sequence)/sizeof(sequence[0]) : 0));
    if (native) { visible=NULL;monitor_stop();assert(!live_allocations); }
    else stop();
    native_records=false;
    printf("PASS horizontal cursor damage %ux%u %s: 512/7424 pixels, both directions; tight odd edge boxes, staggered rows, cross, dense frame, least-cost axis (768/7296, 128/768)\n",
           width,height,accelerated ? "accelerated" : "ROM");
}

static void check_damage_bands(void)
{
    enum { WIDTH = 163, ROWS = 49, BAND = 16 };
    uint16_t before[WIDTH * ROWS], next[WIDTH * ROWS], copied[WIDTH * ROWS];
    uint32_t rng = 42;
    for (unsigned trial = 0; trial < 128; ++trial) {
        for (unsigned i = 0; i < WIDTH * ROWS; ++i) next[i] = before[i] = (uint16_t)(rng + i);
        monitor_damage_t expected = {WIDTH, ROWS, -1, -1};
        for (unsigned n = 0; n < trial; ++n) {
            rng = rng * 1664525u + 1013904223u;
            unsigned offset = rng % (WIDTH * ROWS);
            next[offset] = before[offset] ^ 0xffff;
            int x = offset % WIDTH, y = offset / WIDTH;
            if (x < expected.x1) expected.x1 = x;
            if (y < expected.y1) expected.y1 = y;
            if (x > expected.x2) expected.x2 = x;
            if (y > expected.y2) expected.y2 = y;
        }
        for (unsigned native = 0; native <= 1; ++native) {
            memcpy(copied, before, sizeof(copied));
            usb_jpeg_damage_t track = {.previous = native ? copied : before, .bounds = {0, 0, -1, -1}};
            for (unsigned y = 0; y < ROWS; y += BAND) {
                unsigned count = ROWS - y < BAND ? ROWS - y : BAND;
                usb_jpeg_damage_band(&track, next + y*WIDTH, native ? copied + y*WIDTH : NULL, WIDTH, y, count);
            }
            if (native) assert(!memcmp(copied, next, sizeof(next)));
            monitor_damage_t regions[2];
            unsigned nr = usb_jpeg_damage_regions(&track, regions);
            assert(nr <= 2 && (nr == 0) == (expected.x2 < expected.x1));
            for (int y = 0; y < ROWS; ++y) for (int x = 0; x < WIDTH; ++x) {
                if (before[y*WIDTH+x] == next[y*WIDTH+x]) continue;
                bool covered = false;
                for (unsigned k = 0; k < nr; ++k) {
                    monitor_damage_t r = regions[k];
                    covered |= r.x1 <= x && x <= r.x2 && r.y1 <= y && y <= r.y2;
                }
                assert(covered);
            }
            if (!trial) assert(track.bounds.x2 < track.bounds.x1);
            else assert(track.bounds.x1 == expected.x1 && track.bounds.x2 == expected.x2 &&
                        track.bounds.y1 == expected.y1 && track.bounds.y2 == expected.y2);
        }
    }
    puts("PASS 256 exact damage comparisons against pixel oracle, native writeback and partial final band");
}

static void await_picture_counts(unsigned decoded, unsigned duplicates, unsigned errors)
{
    for (unsigned i = 0; i < 2000; ++i) {
        monitor_present(select_frame);
        usb_monitor_stats_t s = usb_monitor_stats();
        if (s.decoded == decoded && s.duplicate_jpegs == duplicates && s.decode_errors == errors) {
            // A VGA worker can publish between monitor_present and the stats read.
            monitor_present(select_frame);
            return;
        }
        delay();
    }
    assert(false);
}

static void tail_frame(size_t size, unsigned last)
{
    feed_encoding(220, size, last, 0, HEADER + size);
}

static void check_duplicates(bool native, unsigned slots)
{
    native_records = native;
    input_capacity = slots + native;
    jpeg_allocations = 0;
    assert(monitor_start(native) && usb_monitor_stats().input_slots == slots);
    size_t corner = (native ? 1024 * 600 : 640 * 480) - 1;
    unsigned decoded = 1, duplicates = 0;
    tail_frame(PAYLOAD, 220);
    await_picture_counts(decoded, duplicates, 0);
    assert(visible && selected_full && visible[corner] == PAYLOAD * 256 + 220);
    monitor_refresh_complete();
    const bool cached = native || slots > 1;
    unsigned calls = atomic_load(&decoder_calls), selections = selects;
    for (unsigned i = 0; i < 3; ++i) {
        tail_frame(PAYLOAD, 220);
        if (cached) ++duplicates;
        else ++decoded;
        await_picture_counts(decoded, duplicates, 0);
    }
    assert(atomic_load(&decoder_calls) == calls + (cached ? 0 : 3));
    assert(selects == selections + (cached ? 0 : 3));
    assert(usb_monitor_latency().samples == 1 && usb_monitor_latency().sequence == 1);

    // Change only the last byte, then truncate to an exact prefix. Both must reach the screen.
    tail_frame(PAYLOAD, 219);
    await_picture_counts(++decoded, duplicates, 0);
    assert(visible[corner] == PAYLOAD * 256 + 219);
    tail_frame(PAYLOAD - 1, 220);
    await_picture_counts(++decoded, duplicates, 0);
    assert(visible[corner] == (PAYLOAD - 1) * 256 + 220);
    monitor_refresh_complete();
    assert(usb_monitor_latency().sequence == 6);

    // Never cache a failed JPEG. In native mode rollback decodes its saved good JPEG.
    for (unsigned i = 1; i <= 2; ++i) {
        frame(203);
        await_picture_counts(decoded, duplicates, i);
        assert(monitor_last_decode_failed() && visible[corner] == (PAYLOAD - 1) * 256 + 220);
    }
    calls = atomic_load(&decoder_calls);
    tail_frame(PAYLOAD - 1, 220);
    if (cached) ++duplicates;
    else ++decoded;
    await_picture_counts(decoded, duplicates, 2);
    assert(!monitor_last_decode_failed() && atomic_load(&decoder_calls) == calls + !cached);
    usb_monitor_reset();
    monitor_present(select_frame);
    assert(!visible);
    tail_frame(PAYLOAD - 1, 220);
    await_picture_counts(++decoded, duplicates, 2);
    assert(visible && selected_full && visible[corner] == (PAYLOAD - 1) * 256 + 220);
    usb_monitor_stats_t stats = usb_monitor_stats();
    assert(stats.queued == 10 && !stats.busy_drops && !stats.superseded);
    if (native) {
        visible = NULL;
        monitor_stop();
        assert(!live_allocations);
    } else stop();
    native_records = false;
    printf("PASS %s, %u slot(s): %u decoded, %u exact duplicates, %u invalid, 0 lost; final byte, length and reset\n",
           native ? "native" : "VGA", slots, decoded, duplicates, stats.decode_errors);
}

static void check_cached_slot_reuse(void)
{
    jpeg_allocations = 0;
    input_capacity = 2;
    assert(monitor_start(false));
    frame(60);
    await_picture_counts(1, 0, 0);
    check_pixels(60);
    atomic_store(&hold, true);
    frame(61);
    wait_decode(61);
    frame(60); // Its only available input slot held the cached first image.
    atomic_store(&hold, false);
    wait_completed(2);
    monitor_present(select_frame);
    check_pixels(61);
    wait_completed(3);
    monitor_present(select_frame);
    check_pixels(60);
    assert(!usb_monitor_stats().busy_drops && !usb_monitor_stats().duplicate_jpegs);
    stop();
    puts("PASS VGA cached slot reused under pressure; final change back to first picture displayed");
}

static void check_windows_modes(void)
{
    input_capacity = 5;
    jpeg_allocations = 0;
    assert(monitor_start(true) && usb_monitor_enable_windows_modes());
    usb_monitor_reset();
    native_records = true;
    frame(40);
    await_picture_counts(1, 0, 0);
    native_records = false;
    frame(40);  // Same bytes, different wire geometry: must validate again.
    await_picture_counts(1, 0, 1);
    check_pixels(40);
    frame(41);
    await_picture_counts(2, 0, 1);
    assert(selected_full);
    frame(203);  // Damaged VGA picture must restore the previously scaled frame.
    await_picture_counts(2, 0, 2);
    check_pixels(41);
    native_records = true;
    frame(42);
    await_picture_counts(3, 0, 2);
    check_pixels(42);
    visible = NULL;
    monitor_stop();
    assert(!live_allocations);
    native_records = false;
    puts("PASS same-session native/VGA/native, geometry mismatch, rollback and USB reset");
}

static void check_lost_native_picture(void)
{
    native_records = true;
    padding_cells=NULL;
    jpeg_allocations = 0;
    input_capacity = 5;
    assert(monitor_start(true));
    frame(200);
    await_picture_counts(1, 0, 0);
    assert(selected_full && visible);
    fail_native_restore = true;
    const unsigned before_failure=failed_padded_decodes;
    frame(203);
    await_picture_counts(1, 0, 1);
    assert(!visible && monitor_last_decode_failed());
    assert(failed_padded_decodes==before_failure+1 && padding_cells);
    for (unsigned i=0;i<NATIVE_CELLS;i++) assert(!padding_cells[i]);
    fail_native_restore = false;
    frame(200);
    await_picture_counts(2, 0, 1);
    assert(visible && selected_full && !monitor_last_decode_failed());
    for (unsigned i = 0; i < 1024 * 600; ++i) assert(visible[i] == 200);
    expect_clear_padding=true;
    frame(201);
    await_picture_counts(3,0,1);
    assert(!expect_clear_padding && !selected_full && selected_count==1 &&
           selected_area.x1==1023 && selected_area.x2==1023 && selected_area.y1==599 && selected_area.y2==599);
    padding_cells=NULL;
    visible = NULL;
    monitor_stop();
    assert(!live_allocations);
    native_records = false;
    puts("PASS failed native rollback overwrites MCU padding; recovery restores 614400 pixels and tracks the last pixel again");
}

#ifdef KVM_USB_PICTURE_CHECK
static void check_first_reference(bool native)
{
    native_records = native;
    input_capacity = 5;
    jpeg_allocations = 0;
    assert(monitor_start(native));
    frame(40);
    await_picture_counts(1, 0, 0);
    unsigned calls = atomic_load(&reference_calls);
    usb_monitor_enable_picture_check();
    frame(40);
    await_picture_counts(2, 0, 0);
    assert(atomic_load(&reference_calls) == calls + 1);
    frame(40);
    await_picture_counts(2, 1, 0);
    assert(atomic_load(&reference_calls) == calls + 1);
    usb_monitor_picture_t pair = monitor_checked_picture(visible, "has_nonblack");
    assert(pair.sequence == 2 && !strcmp(pair.reference, "all_black"));
    if (native) {
        visible = NULL;
        monitor_stop();
        assert(!live_allocations);
    } else stop();
    native_records = false;
    printf("PASS %s first requested reference acquired on identical input, then reused\n", native ? "native" : "VGA");
}
#endif

int main(void)
{
    // Windows may send just one final update before a long static interval.
    // Keep frame 13 while 11 is decoding, even with 12 already waiting.
    assert(monitor_start(false) && usb_monitor_stats().input_slots == 3);
    atomic_store(&hold, true);
    frame(11);
    wait_decode(11);
    frame(12);
    frame(13);
    feed(14, 0, HEADER + 7); // A partial transfer must not displace 13.
    usb_monitor_stats_t burst = usb_monitor_stats();
    assert(burst.queued == 3 && burst.superseded == 1 && !burst.busy_drops);
    atomic_store(&hold, false);
    wait_completed(1);
    monitor_present(select_frame);
    check_pixels(11);
    wait_completed(2);
    monitor_present(select_frame);
    check_pixels(13);
    stop();

    input_capacity = 2;
    jpeg_allocations = 0;
    assert(monitor_start(false));
    assert(usb_monitor_stats().input_slots == 2);
    atomic_store(&hold, true);
    frame(1);
    wait_decode(1);
    frame(2);
    assert(usb_monitor_stats().queued == 2 && !usb_monitor_stats().busy_drops);
    frame(3);
    usb_monitor_stats_t stats = usb_monitor_stats();
    assert(stats.busy_drops == 1 && !stats.superseded);
    atomic_store(&hold, false);
    wait_completed(1);

    frame(4); // The UI is paused. Keep only the newest complete waiting input.
    feed(5, 0, HEADER + 7);
    stats = usb_monitor_stats();
    assert(stats.queued == 3 && stats.busy_drops == 1 && stats.superseded == 1);
    atomic_store(&hold, true);
    monitor_present(select_frame);
    check_pixels(1);
    wait_decode(4); // The incomplete input has not displaced the completed one.
    feed(5, HEADER + 7, HEADER + PAYLOAD);
    atomic_store(&hold, false);
    wait_completed(2);
    check_pixels(1); // Both RGB surfaces exist, and the selected one stayed immutable.

    atomic_store(&hold, true);
    monitor_present(select_frame);
    check_pixels(4);
    wait_decode(5);
    feed(6, 0, HEADER + 3);
    usb_monitor_reset();
    frame(7);
    wait_decode(7);
    monitor_present(select_frame);
    assert(!visible);
    atomic_store(&hold, false);
    wait_completed(3);
    monitor_present(select_frame);
    check_pixels(7);
    assert(!usb_monitor_stats().decode_errors && jpeg_allocations == 4 && live_allocations == 4);
    stop();

    input_capacity = 1;
    jpeg_allocations = 0;
    assert(monitor_start(false) && live_allocations == 3 && usb_monitor_stats().input_slots == 1);
    stats = usb_monitor_stats();
    assert(!stats.busy_drops && !stats.superseded);
    atomic_store(&hold, true);
    frame(8);
    wait_decode(8);
    frame(9);
    stats = usb_monitor_stats();
    assert(stats.queued == 1 && stats.busy_drops == 1 && !stats.superseded);
    atomic_store(&hold, false);
    wait_completed(1);
    monitor_present(select_frame);
    check_pixels(8);
    assert(usb_monitor_latency().samples == 0);
    delay();
    monitor_refresh_complete();
    usb_monitor_latency_t measured = usb_monitor_latency();
    assert(measured.sequence == 1 && measured.samples == 1 && measured.mean.refresh > 0);
    assert(measured.mean.refresh == measured.last.refresh);
    assert(measured.peak_total == measured.last.receive + measured.last.queue + measured.last.decode +
                                  measured.last.ready + measured.last.refresh);
    usb_monitor_reset();
    monitor_refresh_complete();
    assert(usb_monitor_latency().samples == 1);
    atomic_store(&hold, true);
    frame(10);
    wait_decode(10);
    stop(); // Cancels an in-flight worker before freeing either of its buffers.
    frame(11);
    assert(usb_monitor_stats().queued == 2 && usb_monitor_stats().decoded == 1);
    for (unsigned slots = 1; slots <= 3; ++slots) check_reconnect(slots);
#ifdef KVM_USB_PICTURE_CHECK
    check_trial_reset();
#endif
    check_byte_scans();
    check_cached_cell_colors();
    check_cached_band_rows(16);
    check_cached_band_rows(8);
    check_adaptive_cache();
    check_damage_bands();
    check_vga_damage(false);
    check_vga_damage(true);
    check_native_damage(false);
    check_native_damage(true);
    check_motion_pipeline(false,false);
    check_motion_pipeline(false,true);
    check_motion_pipeline(true,true);
    check_cached_slot_reuse();
    check_lost_native_picture();
    check_windows_modes();
#ifdef KVM_USB_PICTURE_CHECK
    check_first_reference(false);
    check_first_reference(true);
#endif
    for (unsigned slots = 1; slots <= 3; ++slots) {
        check_duplicates(false, slots);
        check_duplicates(true, slots);
    }
    puts("PASS USB reception during decode, bounded pending input, immutable selected RGB, partial/reset/stop and low-memory fallback");
}
