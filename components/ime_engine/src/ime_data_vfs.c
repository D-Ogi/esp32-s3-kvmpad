// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_data_vfs: see ime_data_vfs.h. Reads go straight to esp_partition_read(); nothing is cached here (the
// engines copy what they need into their own PSRAM buffers once).
#include "ime_data_vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

static const char *TAG = "ime_data";

#define IME_VFS_FDS 2
#define HASH_CHUNK 4096

typedef struct {
    bool used;
    uint32_t entry;
    uint32_t pos;
} vfd_t;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static const esp_partition_t *s_part;
static ime_data_vfs_status_t s_st;
static bool s_tried;
static esp_err_t s_result = ESP_ERR_INVALID_STATE;
static vfd_t s_fd[IME_VFS_FDS];

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static bool verify(const ime_data_entry_t *e)
{
    uint8_t *buf = heap_caps_malloc(HASH_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return false;
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t done = 0; ok && done < e->size;) {
        const uint32_t n = (e->size - done) < HASH_CHUNK ? (e->size - done) : HASH_CHUNK;
        ok = esp_partition_read(s_part, e->offset + done, buf, n) == ESP_OK &&
             psa_hash_update(&op, buf, n) == PSA_SUCCESS;
        done += n;
    }
    uint8_t digest[32];
    size_t dlen = 0;
    if (ok) {
        ok = psa_hash_finish(&op, digest, sizeof(digest), &dlen) == PSA_SUCCESS && dlen == sizeof(digest) &&
             memcmp(digest, e->sha256, sizeof(digest)) == 0;
    } else {
        (void)psa_hash_abort(&op);
    }
    heap_caps_free(buf);
    return ok;
}

// ---- VFS operations (context pointer unused; one image per device) ------------------------------
static int find_entry(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return -1;
    }
    for (uint32_t i = 0; i < s_st.count; i++) {
        if (strncmp(path + 1, s_st.entry[i].name, IME_DATA_NAME_CAP) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int vfs_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0) {
        lock();
        s_st.open_refused++;
        unlock();
        errno = EROFS;
        return -1;
    }
    lock();
    const int e = find_entry(path);
    if (e < 0 || !s_st.verified[e]) {
        s_st.open_refused++;
        unlock();
        errno = ENOENT;
        return -1;
    }
    for (int fd = 0; fd < IME_VFS_FDS; fd++) {
        if (!s_fd[fd].used) {
            s_fd[fd] = (vfd_t){.used = true, .entry = (uint32_t)e, .pos = 0};
            s_st.opens++;
            unlock();
            return fd;
        }
    }
    s_st.open_refused++;
    unlock();
    errno = ENFILE;
    return -1;
}

static bool fd_ok(int fd)
{
    return fd >= 0 && fd < IME_VFS_FDS && s_fd[fd].used;
}

static ssize_t read_at(int fd, void *dst, size_t size, uint32_t pos)
{
    const ime_data_entry_t *e = &s_st.entry[s_fd[fd].entry];
    if (pos >= e->size) {
        return 0;
    }
    const size_t n = size < (size_t)(e->size - pos) ? size : (size_t)(e->size - pos);
    if (esp_partition_read(s_part, e->offset + pos, dst, n) != ESP_OK) {
        errno = EIO;
        return -1;
    }
    s_st.reads++;
    return (ssize_t)n;
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size)
{
    (void)ctx;
    lock();
    if (!fd_ok(fd)) {
        unlock();
        errno = EBADF;
        return -1;
    }
    const ssize_t n = read_at(fd, dst, size, s_fd[fd].pos);
    if (n > 0) {
        s_fd[fd].pos += (uint32_t)n;
    }
    unlock();
    return n;
}

static ssize_t vfs_pread(void *ctx, int fd, void *dst, size_t size, off_t offset)
{
    (void)ctx;
    lock();
    if (!fd_ok(fd) || offset < 0) {
        unlock();
        errno = fd_ok(fd) ? EINVAL : EBADF;
        return -1;
    }
    const ssize_t n = read_at(fd, dst, size, (uint32_t)offset);
    unlock();
    return n;
}

static off_t vfs_lseek(void *ctx, int fd, off_t off, int whence)
{
    (void)ctx;
    lock();
    if (!fd_ok(fd)) {
        unlock();
        errno = EBADF;
        return -1;
    }
    const int64_t size = s_st.entry[s_fd[fd].entry].size;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)s_fd[fd].pos : whence == SEEK_END ? size : -1;
    const int64_t pos = base + (int64_t)off;
    if (base < 0 || pos < 0 || pos > size) {
        unlock();
        errno = EINVAL;
        return -1;
    }
    s_fd[fd].pos = (uint32_t)pos;
    unlock();
    return (off_t)pos;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    lock();
    if (!fd_ok(fd)) {
        unlock();
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = (off_t)s_st.entry[s_fd[fd].entry].size;
    unlock();
    return 0;
}

static int vfs_close(void *ctx, int fd)
{
    (void)ctx;
    lock();
    if (!fd_ok(fd)) {
        unlock();
        errno = EBADF;
        return -1;
    }
    s_fd[fd].used = false;
    unlock();
    return 0;
}

static ssize_t vfs_write(void *ctx, int fd, const void *src, size_t size)
{
    (void)ctx;
    (void)fd;
    (void)src;
    (void)size;
    errno = EROFS;
    return -1;
}

static const esp_vfs_fs_ops_t k_ops = {
    .write_p = vfs_write,
    .lseek_p = vfs_lseek,
    .read_p = vfs_read,
    .pread_p = vfs_pread,
    .open_p = vfs_open,
    .close_p = vfs_close,
    .fstat_p = vfs_fstat,
};

esp_err_t ime_data_mount(void)
{
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    portEXIT_CRITICAL(&s_init_mux);
    lock();
    if (s_tried) {
        const esp_err_t r = s_result;
        unlock();
        return r;
    }
    s_tried = true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ime_data");
    s_st.partition_found = s_part != NULL;
    if (s_part == NULL) {
        s_result = ESP_ERR_NOT_FOUND;
        unlock();
        return s_result;
    }
    s_st.partition_size = s_part->size;
    uint8_t *dir = heap_caps_malloc(IME_DATA_DIR_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dir == NULL || esp_partition_read(s_part, 0, dir, IME_DATA_DIR_LEN) != ESP_OK) {
        heap_caps_free(dir);
        s_result = ESP_ERR_NO_MEM;
        unlock();
        return s_result;
    }
    s_st.parse = ime_data_parse(dir, IME_DATA_DIR_LEN, s_part->size, s_st.entry, IME_DATA_MAX_ENTRIES, &s_st.count);
    heap_caps_free(dir);
    if (s_st.parse != IME_DATA_OK) {
        ESP_LOGW(TAG, "no usable image: %s", ime_data_status_name(s_st.parse));
        s_result = ESP_ERR_INVALID_STATE;
        unlock();
        return s_result;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        s_result = ESP_FAIL;
        unlock();
        return s_result;
    }
    const int64_t t0 = esp_timer_get_time();
    uint32_t good = 0;
    for (uint32_t i = 0; i < s_st.count; i++) {
        s_st.verified[i] = verify(&s_st.entry[i]);
        good += s_st.verified[i] ? 1u : 0u;
    }
    s_st.verify_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_result = esp_vfs_register_fs(IME_DATA_MOUNT, &k_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
    s_st.mounted = s_result == ESP_OK;
    ESP_LOGI(TAG, "%u file(s), %u verified in %u ms, mount %s", (unsigned)s_st.count, (unsigned)good,
             (unsigned)s_st.verify_ms, esp_err_to_name(s_result));
    unlock();
    return s_result;
}

esp_err_t ime_data_file(const char *name, ime_data_file_t *out)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (!s_st.mounted) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < s_st.count; i++) {
        if (strncmp(name, s_st.entry[i].name, IME_DATA_NAME_CAP) != 0) {
            continue;
        }
        if (s_st.verified[i] && s_st.entry[i].size != 0u) { // an unverified file is never handed out
            out->offset = s_st.entry[i].offset;
            out->size = s_st.entry[i].size;
            err = ESP_OK;
        }
        break;
    }
    unlock();
    return err;
}

bool ime_data_file_read(const ime_data_file_t *file, uint32_t off, void *dst, uint32_t n)
{
    // s_part is set once, before the mount that produced `file`, and never changes
    if (file == NULL || dst == NULL || s_part == NULL || off > file->size || n > file->size - off) {
        return false;
    }
    return n == 0u || esp_partition_read(s_part, file->offset + off, dst, n) == ESP_OK;
}

esp_err_t ime_data_load(const char *name, void **data, size_t *len)
{
    if (data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ime_data_file_t f;
    const esp_err_t err = ime_data_file(name, &f);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *buf = heap_caps_malloc(f.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // in chunks, so a long copy does not hold the flash driver against a writer for the whole file
    for (uint32_t done = 0; done < f.size;) {
        const uint32_t n = (f.size - done) < 65536u ? (f.size - done) : 65536u;
        if (!ime_data_file_read(&f, done, buf + done, n)) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        done += n;
    }
    *data = buf;
    *len = f.size;
    return ESP_OK;
}

void ime_data_get_status(ime_data_vfs_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_st;
    unlock();
}
