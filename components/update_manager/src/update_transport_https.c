// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Locally triggered HTTPS image source (TASK-110, ADR-031). One connection at a time, no
// allocation of its own beyond what esp_http_client needs, and no credential ever logged.
//
// What this file does NOT do: decide whether an update may start, verify the image, or expose any
// network entry point. It only hands bytes to the state machine, which verifies the signature
// through esp_ota_end() and treats the transport as untrusted.
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "update_transport.h"

static const char *TAG = "ota_http";

static esp_http_client_handle_t s_client;
static const char *s_cert_pem;

void update_transport_https_set_cert(const char *pem)
{
    s_cert_pem = pem;
}

static void http_close(void *ctx)
{
    (void)ctx;
    if (s_client != NULL) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static int http_open(void *ctx, const char *url, size_t *out_size)
{
    (void)ctx;
    if (url == NULL || out_size == NULL || strncmp(url, "https://", 8) != 0) {
        return -1; // TECH-041: no plain http, no "temporary" downgrade
    }
    if (s_client != NULL) {
        http_close(NULL);
    }
    *out_size = 0u;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = KVM_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .method = HTTP_METHOD_GET,
    };
    if (s_cert_pem != NULL) {
        cfg.cert_pem = s_cert_pem; // pinned certificate
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach; // CA-verified chain
    }

    s_client = esp_http_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return -1;
    }
    esp_err_t err = esp_http_client_open(s_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err)); // no URL in the log line
        http_close(NULL);
        return -1;
    }

    int64_t len = esp_http_client_fetch_headers(s_client);
    int status = esp_http_client_get_status_code(s_client);
    if (status != 200) {
        ESP_LOGE(TAG, "http status %d", status);
        http_close(NULL);
        return -1;
    }
    if (esp_http_client_is_chunked_response(s_client) || len <= 0) {
        // Without a length a truncated transfer looks exactly like a complete one.
        ESP_LOGE(TAG, "no Content-Length (chunked or empty response)");
        http_close(NULL);
        return -1;
    }
    if ((uint64_t)len > (uint64_t)KVM_OTA_HTTP_MAX_IMAGE_BYTES) {
        ESP_LOGE(TAG, "image of %lld bytes is larger than an OTA slot", (long long)len);
        http_close(NULL);
        return -1;
    }
    *out_size = (size_t)len;
    ESP_LOGI(TAG, "source open, %lld bytes", (long long)len);
    return 0;
}

static int http_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    if (s_client == NULL || buf == NULL || out_len == NULL || cap == 0u) {
        return -1;
    }
    *out_len = 0u;
    int n = esp_http_client_read(s_client, (char *)buf, (int)(cap > (size_t)INT32_MAX ? INT32_MAX : cap));
    if (n < 0) {
        ESP_LOGE(TAG, "read error");
        return -1;
    }
    if (n == 0) {
        // Either the end of the body or a closed connection; the state machine compares what it
        // received against Content-Length and calls a short transfer truncated.
        *out_len = 0u;
        return 0;
    }
    *out_len = (size_t)n;
    return 0;
}

kvm_ota_source_t update_transport_https(void)
{
    kvm_ota_source_t src = {
        .ctx = NULL,
        .open = http_open,
        .read = http_read,
        .close = http_close,
    };
    return src;
}
