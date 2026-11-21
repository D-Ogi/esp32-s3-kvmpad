// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "crop_wire.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p+2)<<16); }
static int32_t i32(const uint8_t *p)
{
    uint32_t u=u32(p);
    return (int32_t)((int64_t)u - ((u>>31) ? INT64_C(4294967296) : 0));
}

static uint32_t crc(const uint8_t *p,size_t size)
{
    uint32_t n=UINT32_MAX;
    while (size--) {
        n^=*p++;
        for (unsigned j=0;j<8;j++) n=(n>>1)^((0u-(n&1u))&UINT32_C(0xedb88320));
    }
    return ~n;
}

static int nibble(char c)
{
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    return -1;
}

bool kvb_crop_parse(const kvm_json_doc_t *doc,uint32_t stream,unsigned cw,unsigned ch,kvb_crop_record_t *record)
{
    static const char *const keys[]={"type","data",NULL};
    uint8_t b[48];
    char hex[97];
    if (!stream || !cw || !ch || !record || !doc || !kvm_json_keys_allowed(doc,0,keys) ||
        !kvm_json_str_eq(doc,kvm_json_member(doc,0,"type"),"CROP1") ||
        kvm_json_str_copy(doc,kvm_json_member(doc,0,"data"),hex,sizeof(hex))!=96) return false;
    for (unsigned i=0;i<sizeof(b);i++) {
        int hi=nibble(hex[2*i]),lo=nibble(hex[2*i+1]);
        if (hi<0 || lo<0) return false;
        b[i]=(uint8_t)((hi<<4)|lo);
    }
    if (memcmp(b,"KCM1",4) || b[28]>1 || b[29] || b[30] || b[31] ||
        u32(b+32)!=stream || u32(b+44)!=crc(b,44)) return false;
    kvmbridge_crop_t g={.cursor_x=i32(b+4),.cursor_y=i32(b+8),.left=i32(b+12),.top=i32(b+16),
        .width=u16(b+20),.height=u16(b+22),.pad_x=u16(b+24),.pad_y=u16(b+26),.clamped=b[28]!=0};
    int64_t x=(int64_t)g.cursor_x-g.left, y=(int64_t)g.cursor_y-g.top;
    if (!g.width || !g.height || g.width>cw || g.height>ch ||
        x<0 || y<0 || x>=g.width || y>=g.height ||
        (int64_t)g.left+g.width-1>INT32_MAX || (int64_t)g.top+g.height-1>INT32_MAX ||
        g.pad_x!=(cw-g.width)/2 || g.pad_y!=(ch-g.height)/2 ||
        g.clamped!=(g.width<cw || g.height<ch || x!=cw/2 || y!=ch/2)) return false;
    *record=(kvb_crop_record_t){.geometry=g,.sequence=u32(b+36),.image_crc=u32(b+40)};
    return true;
}

bool kvb_crop_match(const kvb_crop_record_t *r,kvm_live_receiver_t *rx,const uint8_t *p,size_t size)
{
    return r && p && size>KVM_LIVE_HEADER && u32(p+12)==r->sequence && u32(p+28)==r->image_crc &&
           kvm_live_receive(rx,p,size)!=NULL;
}
