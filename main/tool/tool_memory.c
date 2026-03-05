/*
 * ESPClaw - tool/tool_memory.c
 * NVS persistent memory tool handlers. Included by tool_registry.c.
 */
#include "mem/nvs_manager.h"
#include "util/json_util.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static bool tool_memory_set(const char *input_json, char *result_buf, size_t result_sz)
__attribute__((unused));
static bool tool_memory_get(const char *input_json, char *result_buf, size_t result_sz)
__attribute__((unused));
static bool tool_memory_delete(const char *input_json, char *result_buf, size_t result_sz)
__attribute__((unused));
static bool tool_memory_list(const char *input_json, char *result_buf, size_t result_sz)
__attribute__((unused));

static bool tool_memory_set(const char *input_json, char *result_buf, size_t result_sz)
{
    char key[NVS_MAX_KEY_LEN + 1]   = {0};
    char value[NVS_MAX_VALUE_LEN + 1] = {0};

    if (!json_get_str(input_json, "key",   key,   sizeof(key))   || key[0] == '\0' ||
        !json_get_str(input_json, "value", value, sizeof(value))) {
        snprintf(result_buf, result_sz, "Error: missing key or value");
        return false;
    }
    if (strncmp(key, "u_", 2) != 0) {
        snprintf(result_buf, result_sz, "Error: key must start with u_");
        return false;
    }
    esp_err_t err = nvs_mgr_set_str(key, value);
    if (err != ESP_OK) {
        snprintf(result_buf, result_sz, "Error: NVS write failed (%s)", esp_err_to_name(err));
        return false;
    }
    snprintf(result_buf, result_sz, "Stored: %s = %s", key, value);
    return true;
}

static bool tool_memory_get(const char *input_json, char *result_buf, size_t result_sz)
{
    char key[NVS_MAX_KEY_LEN + 1] = {0};
    if (!json_get_str(input_json, "key", key, sizeof(key)) || key[0] == '\0') {
        snprintf(result_buf, result_sz, "Error: missing key");
        return false;
    }
    if (strncmp(key, "u_", 2) != 0) {
        snprintf(result_buf, result_sz, "Error: key must start with u_");
        return false;
    }
    char value[NVS_MAX_VALUE_LEN + 1] = {0};
    bool found = nvs_mgr_get_str(key, value, sizeof(value));
    if (!found) {
        snprintf(result_buf, result_sz, "Not found: %s", key);
        return false;
    }
    snprintf(result_buf, result_sz, "%s = %s", key, value);
    return true;
}

static bool tool_memory_delete(const char *input_json, char *result_buf, size_t result_sz)
{
    char key[NVS_MAX_KEY_LEN + 1] = {0};
    if (!json_get_str(input_json, "key", key, sizeof(key)) || key[0] == '\0') {
        snprintf(result_buf, result_sz, "Error: missing key");
        return false;
    }
    if (strncmp(key, "u_", 2) != 0) {
        snprintf(result_buf, result_sz, "Error: key must start with u_");
        return false;
    }
    /* nvs_mgr_set_str with empty string signals deletion */
    esp_err_t err = nvs_mgr_set_str(key, "");
    if (err != ESP_OK) {
        snprintf(result_buf, result_sz, "Error: NVS delete failed (%s)", esp_err_to_name(err));
        return false;
    }
    snprintf(result_buf, result_sz, "Deleted: %s", key);
    return true;
}

static bool tool_memory_list(const char *input_json, char *result_buf, size_t result_sz)
{
    (void)input_json;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        snprintf(result_buf, result_sz, "Error: NVS open failed (%s)", esp_err_to_name(err));
        return false;
    }

    size_t pos = 0;
    pos += snprintf(result_buf + pos, result_sz - pos, "User keys: ");

    nvs_iterator_t it = NULL;
    err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_STR, &it);
    int count = 0;
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "u_", 2) == 0) {
            if (count > 0 && pos < result_sz - 2) {
                result_buf[pos++] = ',';
                result_buf[pos++] = ' ';
            }
            if (pos < result_sz - NVS_MAX_KEY_LEN - 1) {
                pos += snprintf(result_buf + pos, result_sz - pos, "%s", info.key);
            }
            count++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);

    if (count == 0) {
        snprintf(result_buf, result_sz, "No user keys stored (keys must start with u_)");
    } else {
        result_buf[pos] = '\0';
    }
    return true;
}
