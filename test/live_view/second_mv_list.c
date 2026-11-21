// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Exercise the decoder's cold mixed-slice fallback and both allocation failures.
#include "libavutil/buffer.h"
#include "libavutil/error.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static int reject_reference;
static int replace_reference(AVBufferRef **dst, const AVBufferRef *src)
{
    return reject_reference ? AVERROR(ENOMEM) : av_buffer_replace(dst, src);
}

#define av_buffer_replace replace_reference
#include "libavcodec/h264_slice.c"
#undef av_buffer_replace

const char *kvm_media_rr_mode(void) { return NULL; }

int main(void)
{
    static H264Context h;
    H264Picture *pic = h.cur_pic_ptr = &h.DPB[0];
    AVBufferRef *first = pic->motion_val_buf[0] = av_buffer_alloc(256);
    assert(first);
    memset(first->data, 0xa5, first->size);
    pic->motion_val_buf[1] = av_buffer_ref(first);
    for (int i = 0; i < 2; ++i) {
        h.cur_pic.motion_val_buf[i] = av_buffer_ref(first);
        assert(pic->motion_val_buf[i] && h.cur_pic.motion_val_buf[i]);
        h.cur_pic.motion_val[i] = pic->motion_val[i] = (int16_t (*)[2])first->data + 4;
    }
    for (int stage = 0; stage < 2; ++stage) {
        av_max_alloc(stage ? INT_MAX : 128);
        reject_reference = stage;
        assert(kvm_h264_detach_second_mv_list(&h) == AVERROR(ENOMEM));
        assert(pic->motion_val_buf[1]->data == first->data && h.cur_pic.motion_val_buf[1]->data == first->data);
        assert(av_buffer_get_ref_count(first) == 4);
    }
    reject_reference = 0;
    assert(kvm_h264_detach_second_mv_list(&h) == 0);
    AVBufferRef *second = pic->motion_val_buf[1];
    assert(second->data != first->data && second->data == h.cur_pic.motion_val_buf[1]->data);
    assert(second->size == first->size && av_buffer_get_ref_count(first) == 2 && av_buffer_get_ref_count(second) == 2);
    assert(pic->motion_val[1] == h.cur_pic.motion_val[1] && pic->motion_val[1] == (int16_t (*)[2])second->data + 4);
    for (size_t i = 0; i < first->size; ++i)
        assert(first->data[i] == 0xa5 && second->data[i] == 0);
    for (int i = 0; i < 2; ++i) {
        av_buffer_unref(&pic->motion_val_buf[i]);
        av_buffer_unref(&h.cur_pic.motion_val_buf[i]);
    }
    return 0;
}
