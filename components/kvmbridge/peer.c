// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "peer.h"
#include "kvmbridge.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

// Published only by an authenticated CLIENT connection. Its disconnect revokes the offer.
static portMUX_TYPE peer_mux=portMUX_INITIALIZER_UNLOCKED;
static uint64_t owner;
static kvb_peer_t attached;

static bool read_blob(nvs_handle_t h,const char *name,void *out,size_t cap,size_t *len)
{
    *len=cap;
    return nvs_get_blob(h,name,out,len)==ESP_OK && *len && *len<=cap;
}

bool kvb_identity(kvm_srt_identity_t *out,bool need_private)
{
    nvs_handle_t h;
    if (nvs_open("kvm_srt",NVS_READONLY,&h)!=ESP_OK) return false;
    bool ok=read_blob(h,KVM_SRT_IDENTITY_BLOB_CERT,out->cert_der,sizeof(out->cert_der),&out->cert_len);
    if (ok && need_private)
        ok=read_blob(h,KVM_SRT_IDENTITY_BLOB_KEY,out->key_der,sizeof(out->key_der),&out->key_len);
    nvs_close(h);
    return ok;
}

static bool valid_leaf(const uint8_t *bytes,size_t count)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok=mbedtls_x509_crt_parse_der(&crt,bytes,count)==0 && crt.version==3 && crt.raw.len==count &&
        !mbedtls_x509_crt_get_ca_istrue(&crt) && crt.subject_raw.len==crt.issuer_raw.len &&
        !memcmp(crt.subject_raw.p,crt.issuer_raw.p,crt.subject_raw.len);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

bool kvmbridge_host_attach(uint64_t session,const char *address,uint16_t port,const uint8_t *leaf,size_t length)
{
    struct in_addr ip;
    if (!session || !address || !port || !leaf || !length || length>sizeof(attached.leaf) ||
        strlen(address)>=sizeof(attached.address) || inet_pton(AF_INET,address,&ip)!=1) return false;
    uint32_t first=ntohl(ip.s_addr)>>24;
    if (!first || first==127 || first>=224 || !valid_leaf(leaf,length)) return false;

    portENTER_CRITICAL(&peer_mux);
    bool accepted=!owner || (owner==session && attached.port==port && attached.length==length &&
                              !strcmp(address,attached.address) && !memcmp(leaf,attached.leaf,length));
    if (!owner) {
        owner=session;
        strcpy(attached.address,address);
        memcpy(attached.leaf,leaf,length);
        attached.port=port;
        attached.length=(uint16_t)length;
    }
    portEXIT_CRITICAL(&peer_mux);
    return accepted;
}

void kvmbridge_host_detach(uint64_t session)
{
    portENTER_CRITICAL(&peer_mux);
    if (session && session==owner) {
        owner=0;
        memset(&attached,0,sizeof(attached));
        // Stop existing workers before a different CLIENT can register a listener.
        // Workers read the peer before taking the bridge state lock.
        kvmbridge_stop();
    }
    portEXIT_CRITICAL(&peer_mux);
}

bool kvb_peer_load(kvb_peer_t *out)
{
    portENTER_CRITICAL(&peer_mux);
    bool ready=owner!=0;
    if (ready) *out=attached;
    portEXIT_CRITICAL(&peer_mux);
    return ready;
}

bool kvmbridge_host_endpoint(char endpoint[24])
{
    char address[sizeof(attached.address)];
    portENTER_CRITICAL(&peer_mux);
    uint16_t port=attached.port;
    memcpy(address,attached.address,sizeof(address));
    portEXIT_CRITICAL(&peer_mux);
    if (port) snprintf(endpoint,24,"%s:%u",address,port);
    else endpoint[0]=0;
    return port!=0;
}
