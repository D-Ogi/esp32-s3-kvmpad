// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// The input stream consists solely of synthetic fixture records from test_crop_metadata.py.
#include "crop_wire.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static uint8_t frame[KVM_LIVE_HEADER+KVM_LIVE_MAX_PAYLOAD];

int main(int argc,char **argv)
{
    if (argc!=5) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);
#endif
    kvm_live_receiver_t rx;
    if (!kvm_live_start(&rx,(uint32_t)strtoul(argv[2],NULL,10),(kvm_live_codec_t)atoi(argv[1]))) return 2;
    unsigned width=(unsigned)atoi(argv[3]),height=(unsigned)atoi(argv[4]);
    uint8_t sizes[6];
    while (fread(sizes,1,sizeof(sizes),stdin)==sizeof(sizes)) {
        unsigned n=sizes[0]+256u*sizes[1];
        uint32_t f=sizes[2]+(uint32_t)sizes[3]*256+(uint32_t)sizes[4]*65536+(uint32_t)sizes[5]*16777216;
        char text[161]; kvm_json_doc_t doc; kvb_crop_record_t r={0};
        if (n>=sizeof(text) || f>sizeof(frame) || fread(text,1,n,stdin)!=n || fread(frame,1,f,stdin)!=f) return 2;
        if (kvm_json_parse(&doc,text,n)!=KVM_JSON_OK || !kvb_crop_parse(&doc,rx.stream_id,width,height,&r) ||
            !kvb_crop_match(&r,&rx,frame,f)) { puts("REJECT"); continue; }
        const kvmbridge_crop_t *g=&r.geometry;
        printf("%d %d %d %d %u %u %u %u %u\n",(int)g->cursor_x,(int)g->cursor_y,(int)g->left,(int)g->top,
               g->width,g->height,g->pad_x,g->pad_y,(unsigned)g->clamped);
    }
    return ferror(stdin)?2:0;
}
