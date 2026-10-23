#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_littlefs.h"

#include "config.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_FS_BASE_PATH,
        .partition_label = CFG_FS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    storage_usage(&total, &used);
    ESP_LOGI(TAG, "LittleFS %s: %u KB belegt von %u KB", CFG_FS_BASE_PATH,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

bool storage_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

char *storage_read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (len_out) {
        *len_out = n;
    }
    return buf;
}

esp_err_t storage_write_file(const char *path, const void *data, size_t len)
{
    /* Erst in temporäre Datei, dann umbenennen: kein halbfertiger Inhalt bei Stromausfall. */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s fehlgeschlagen", tmp);
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    if (n != len) {
        unlink(tmp);
        return ESP_FAIL;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_remove(const char *path)
{
    return (unlink(path) == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_usage(size_t *total, size_t *used)
{
    return esp_littlefs_info(CFG_FS_PARTITION_LABEL, total, used);
}
