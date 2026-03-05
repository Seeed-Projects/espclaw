/*
 * ESPClaw - channel/channel_telegram.c
 * Telegram Bot channel — long polling getUpdates + sendMessage.
 *
 * Improvements from zclaw:
 * - cJSON for robust JSON parsing
 * - Offset persistence to NVS (prevents message replay on reboot)
 * - Exponential backoff on network failures
 * - Stale/duplicate message detection and auto-resync
 * - Detailed network diagnostics logging
 * - Chat ID whitelist with proper parsing
 */
#include "channel/channel.h"
#include "channel/telegram_helpers.h"
#include "bus/message_bus.h"
#include "platform.h"
#include "messages.h"
#include "config.h"
#include "nvs_keys.h"
#include "mem/nvs_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>

static const char *TAG = "telegram";

/* Telegram state */
static message_bus_t *s_bus = NULL;
static char s_token[128] = "";
static int64_t s_primary_chat_id = 0;
static int64_t s_allowed_chat_ids[TELEGRAM_MAX_ALLOWED_CHAT_IDS] = {0};
static size_t s_allowed_chat_count = 0;
static int64_t s_last_update_id = 0;
static uint32_t s_stale_poll_streak = 0;
static int s_consecutive_failures = 0;

/* Output queue for Telegram responses */
static QueueHandle_t s_tg_out_queue = NULL;

/* Telegram-internal mutex: serializes poll GET and sendMessage POST */
static SemaphoreHandle_t s_tg_http_mutex = NULL;

/* Static buffers to avoid heap fragmentation from repeated malloc/free */
static char s_poll_buf[2048];       /* Response buffer for getUpdates */
static char s_send_resp_buf[256];   /* Response buffer for sendMessage */

/* HTTP response context */
typedef struct {
    char *buf;
    size_t buf_sz;
    size_t written;
    bool truncated;
} http_ctx_t;

/* Network diagnostics snapshot */
typedef struct {
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t largest_block;
    int rssi;
    bool rssi_valid;
} net_diag_t;

/* -------------------------------------------------------------------------
 * Utility Functions
 * ------------------------------------------------------------------------- */

static const char *http_transport_name(esp_http_client_transport_t transport)
{
    switch (transport) {
        case HTTP_TRANSPORT_OVER_TCP:
            return "tcp";
        case HTTP_TRANSPORT_OVER_SSL:
            return "ssl";
        default:
            return "unknown";
    }
}

static uint32_t elapsed_ms_since(int64_t started_us)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us <= started_us) {
        return 0;
    }
    return (uint32_t)((now_us - started_us) / 1000);
}

static void capture_net_diag(net_diag_t *snap)
{
    if (!snap) return;

    snap->free_heap = (uint32_t)esp_get_free_heap_size();
    snap->min_heap = (uint32_t)esp_get_minimum_free_heap_size();
    snap->largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snap->rssi = 0;
    snap->rssi_valid = false;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snap->rssi = ap_info.rssi;
        snap->rssi_valid = true;
    }
}

static bool format_int64_decimal(int64_t value, char *out, size_t out_len)
{
    char reversed[24];
    size_t reversed_len = 0;
    uint64_t magnitude;
    size_t pos = 0;

    if (!out || out_len == 0) return false;

    if (value < 0) {
        out[pos++] = '-';
        magnitude = (uint64_t)(-(value + 1)) + 1ULL;
    } else {
        magnitude = (uint64_t)value;
    }

    do {
        if (reversed_len >= sizeof(reversed)) {
            out[0] = '\0';
            return false;
        }
        reversed[reversed_len++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    } while (magnitude > 0);

    if (pos + reversed_len + 1 > out_len) {
        out[0] = '\0';
        return false;
    }

    while (reversed_len > 0) {
        out[pos++] = reversed[--reversed_len];
    }
    out[pos] = '\0';
    return true;
}

static void log_http_diag(const char *op,
                          esp_http_client_handle_t client,
                          esp_err_t err,
                          int status,
                          int64_t started_us,
                          size_t resp_len,
                          int result_count,
                          int stale_count,
                          int accepted_count,
                          const net_diag_t *before,
                          const net_diag_t *after)
{
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;

    if (client) {
        if (status < 0) {
            status = esp_http_client_get_status_code(client);
        }
        sock_errno = esp_http_client_get_errno(client);
        transport = esp_http_client_get_transport_type(client);
    }

    bool ok = (err == ESP_OK && status == 200);
    int heap_free = after ? (int)after->free_heap : 0;
    int heap_min = after ? (int)after->min_heap : 0;
    int heap_largest = after ? (int)after->largest_block : 0;
    int heap_delta = (before && after) ? (int)after->free_heap - (int)before->free_heap : 0;
    int rssi = (after && after->rssi_valid) ? after->rssi : 0;

    if (ok) {
        ESP_LOGI(TAG,
                 "NETDIAG op=%s ok=1 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%d heap_delta=%d heap_min=%d heap_largest=%d rssi=%d",
                 op ? op : "http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno, sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 elapsed_ms_since(started_us),
                 result_count, stale_count, accepted_count,
                 (unsigned)resp_len,
                 heap_free, heap_delta, heap_min, heap_largest, rssi);
    } else {
        ESP_LOGW(TAG,
                 "NETDIAG op=%s ok=0 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%d heap_delta=%d heap_min=%d heap_largest=%d rssi=%d",
                 op ? op : "http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno, sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 elapsed_ms_since(started_us),
                 result_count, stale_count, accepted_count,
                 (unsigned)resp_len,
                 heap_free, heap_delta, heap_min, heap_largest, rssi);
    }
}

/* -------------------------------------------------------------------------
 * HTTP Handler
 * ------------------------------------------------------------------------- */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && ctx) {
        size_t remaining = ctx->buf_sz - ctx->written - 1;
        size_t to_copy = (size_t)evt->data_len < remaining ? (size_t)evt->data_len : remaining;
        if (to_copy > 0) {
            memcpy(ctx->buf + ctx->written, evt->data, to_copy);
            ctx->written += to_copy;
            ctx->buf[ctx->written] = '\0';
        }
        if ((size_t)evt->data_len > remaining) {
            ctx->truncated = true;
        }
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Token & Chat ID Management
 * ------------------------------------------------------------------------- */

static void clear_allowed_chat_ids(void)
{
    memset(s_allowed_chat_ids, 0, sizeof(s_allowed_chat_ids));
    s_allowed_chat_count = 0;
    s_primary_chat_id = 0;
}

static bool set_allowed_chat_ids_from_string(const char *input)
{
    int64_t parsed_ids[TELEGRAM_MAX_ALLOWED_CHAT_IDS] = {0};
    size_t parsed_count = 0;

    if (!tg_chat_ids_parse(input, parsed_ids, TELEGRAM_MAX_ALLOWED_CHAT_IDS, &parsed_count)) {
        return false;
    }

    clear_allowed_chat_ids();
    for (size_t i = 0; i < parsed_count; i++) {
        s_allowed_chat_ids[i] = parsed_ids[i];
    }
    s_allowed_chat_count = parsed_count;
    s_primary_chat_id = s_allowed_chat_ids[0];
    return true;
}

static bool tg_load_credentials(void)
{
    /* Try NVS first */
    if (nvs_mgr_get_str(NVS_KEY_TG_TOKEN, s_token, sizeof(s_token)) == ESP_OK &&
        strlen(s_token) > 0) {
        char bot_id[24];
        if (tg_extract_bot_id(s_token, bot_id, sizeof(bot_id))) {
            ESP_LOGI(TAG, "Token loaded from NVS (bot ID: %s)", bot_id);
        } else {
            ESP_LOGI(TAG, "Token loaded from NVS");
        }
    } else {
#ifdef CONFIG_ESPCLAW_TELEGRAM_TOKEN
        strncpy(s_token, CONFIG_ESPCLAW_TELEGRAM_TOKEN, sizeof(s_token) - 1);
        ESP_LOGI(TAG, "Token from Kconfig");
#else
        ESP_LOGW(TAG, "No Telegram token configured");
        return false;
#endif
    }

    /* Load chat ID whitelist */
    char chat_ids_str[256];
    if (nvs_mgr_get_str(NVS_KEY_TG_CHAT_IDS, chat_ids_str, sizeof(chat_ids_str)) == ESP_OK) {
        if (set_allowed_chat_ids_from_string(chat_ids_str)) {
            ESP_LOGI(TAG, "Loaded %u authorized chat IDs (primary: %lld)",
                     (unsigned)s_allowed_chat_count, (long long)s_primary_chat_id);
        } else if (strlen(chat_ids_str) > 0) {
            ESP_LOGW(TAG, "Invalid chat ID list: '%s'", chat_ids_str);
        }
    }

#ifdef CONFIG_ESPCLAW_TG_CHAT_IDS
    if (s_allowed_chat_count == 0) {
        strncpy(chat_ids_str, CONFIG_ESPCLAW_TG_CHAT_IDS, sizeof(chat_ids_str) - 1);
        if (set_allowed_chat_ids_from_string(chat_ids_str)) {
            ESP_LOGI(TAG, "Loaded chat IDs from Kconfig: %s", chat_ids_str);
        }
    }
#endif

    /* Load last update offset from NVS for persistence across reboots */
    char offset_str[24] = "";
    if (nvs_mgr_get_str(NVS_KEY_TG_OFFSET, offset_str, sizeof(offset_str)) == ESP_OK) {
        /* Validate: only digits allowed */
        bool valid = true;
        for (const char *p = offset_str; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                valid = false;
                break;
            }
        }
        if (valid && strlen(offset_str) > 0) {
            s_last_update_id = strtoll(offset_str, NULL, 10);
            ESP_LOGI(TAG, "Resuming from update_id=%lld", (long long)s_last_update_id);
        } else {
            ESP_LOGW(TAG, "Invalid offset in NVS, clearing");
            s_last_update_id = 0;
        }
    }

    return strlen(s_token) > 0;
}

static bool tg_chat_allowed(int64_t chat_id)
{
    if (s_allowed_chat_count == 0) {
        return true;  /* No whitelist = allow all */
    }
    return tg_chat_ids_contains(s_allowed_chat_ids, s_allowed_chat_count, chat_id);
}

static int64_t resolve_target_chat_id(int64_t requested_chat_id)
{
    /* No whitelist configured = allow all outbound */
    if (s_allowed_chat_count == 0) {
        return (requested_chat_id != 0) ? requested_chat_id : s_primary_chat_id;
    }

    int64_t target = tg_chat_ids_resolve_target(s_allowed_chat_ids, s_allowed_chat_count,
                                                  s_primary_chat_id, requested_chat_id);
    if (requested_chat_id != 0 && target == 0) {
        ESP_LOGW(TAG, "Refusing outbound send to unauthorized chat ID");
    }
    return target;
}

/* -------------------------------------------------------------------------
 * Backoff
 * ------------------------------------------------------------------------- */

static int get_backoff_delay_ms(void)
{
    if (s_consecutive_failures == 0) return 0;

    int delay = TELEGRAM_BACKOFF_BASE_MS;
    for (int i = 1; i < s_consecutive_failures && delay < TELEGRAM_BACKOFF_MAX_MS; i++) {
        delay *= TELEGRAM_BACKOFF_MULTIPLIER;
    }
    return delay > TELEGRAM_BACKOFF_MAX_MS ? TELEGRAM_BACKOFF_MAX_MS : delay;
}

/* -------------------------------------------------------------------------
 * Telegram API: HTTP GET
 * ------------------------------------------------------------------------- */

static esp_err_t tg_http_get(const char *url, http_ctx_t *ctx)
{
    /* Global TLS lock: only one TLS session at a time (LLM or Telegram) */
    if (!espclaw_tls_lock(pdMS_TO_TICKS(15000))) {
        ESP_LOGW(TAG, "TLS lock timeout (GET)");
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(s_tg_http_mutex, portMAX_DELAY);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        xSemaphoreGive(s_tg_http_mutex);
        espclaw_tls_unlock();
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(s_tg_http_mutex);
    espclaw_tls_unlock();

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET failed: err=%s status=%d", esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Telegram API: HTTP POST
 * ------------------------------------------------------------------------- */

static esp_err_t tg_http_post(const char *url, const char *body, http_ctx_t *ctx)
{
    if (!espclaw_tls_lock(pdMS_TO_TICKS(15000))) {
        ESP_LOGW(TAG, "TLS lock timeout (POST)");
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(s_tg_http_mutex, portMAX_DELAY);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        xSemaphoreGive(s_tg_http_mutex);
        espclaw_tls_unlock();
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(s_tg_http_mutex);
    espclaw_tls_unlock();

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "POST failed: err=%s status=%d", esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Save offset to NVS
 * ------------------------------------------------------------------------- */

static void save_offset_to_nvs(int64_t offset)
{
    char buf[24];
    if (format_int64_decimal(offset, buf, sizeof(buf))) {
        nvs_mgr_set_str(NVS_KEY_TG_OFFSET, buf);
    }
}

/* -------------------------------------------------------------------------
 * Telegram API: getUpdates (long poll)
 * ------------------------------------------------------------------------- */

static esp_err_t tg_get_updates(void)
{
    char url[384];
    char offset_buf[24];
    http_ctx_t ctx = {0};
    int64_t started_us = esp_timer_get_time();
    int result_count = 0, stale_count = 0, accepted_count = 0;
    net_diag_t before = {0}, after = {0};

    capture_net_diag(&before);

    /* Use static buffer to avoid heap fragmentation */
    ctx.buf = s_poll_buf;
    ctx.buf_sz = sizeof(s_poll_buf);
    ctx.buf[0] = '\0';

    /* Build URL with offset */
    int64_t next_offset = (s_last_update_id == INT64_MAX) ?
                          s_last_update_id : s_last_update_id + 1;

    if (!format_int64_decimal(next_offset, offset_buf, sizeof(offset_buf))) {
        return ESP_FAIL;
    }

    snprintf(url, sizeof(url),
             "%s%s/getUpdates?timeout=%d&limit=1&offset=%s",
             TELEGRAM_API_URL, s_token, TELEGRAM_POLL_TIMEOUT, offset_buf);

    esp_err_t err = tg_http_get(url, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    /* Handle truncated response */
    if (ctx.truncated) {
        int64_t recovered_id = 0;
        if (tg_extract_max_update_id(ctx.buf, &recovered_id)) {
            s_last_update_id = recovered_id;
            ESP_LOGW(TAG, "Recovered from truncated response, skipping to update_id=%lld",
                     (long long)s_last_update_id);
            save_offset_to_nvs(s_last_update_id);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Truncated response without parseable update_id");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Parse with cJSON */
    cJSON *root = cJSON_Parse(ctx.buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "API returned not ok");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsArray(result)) {
        /* Empty result is fine */
        if (s_stale_poll_streak > 0) {
            ESP_LOGI(TAG, "Stale-only poll streak cleared at %u", (unsigned)s_stale_poll_streak);
            s_stale_poll_streak = 0;
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    /* Process each update */
    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        if (!update_id || !cJSON_IsNumber(update_id)) continue;

        result_count++;
        int64_t incoming_id = (int64_t)update_id->valuedouble;

        /* Skip stale/duplicate updates */
        if (incoming_id <= s_last_update_id) {
            stale_count++;
            ESP_LOGW(TAG, "Skipping stale update_id=%lld (last=%lld)",
                     (long long)incoming_id, (long long)s_last_update_id);
            continue;
        }

        s_last_update_id = incoming_id;
        accepted_count++;

        /* Save offset to NVS periodically */
        save_offset_to_nvs(s_last_update_id);

        /* Extract message */
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        cJSON *text = cJSON_GetObjectItem(message, "text");

        if (chat && text && cJSON_IsString(text)) {
            cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
            if (chat_id && cJSON_IsNumber(chat_id)) {
                int64_t incoming_chat_id = (int64_t)chat_id->valuedouble;

                /* Check whitelist */
                if (!tg_chat_allowed(incoming_chat_id)) {
                    ESP_LOGW(TAG, "Rejected message from unauthorized chat: %lld",
                             (long long)incoming_chat_id);
                    continue;
                }

                /* Post to inbound queue */
                inbound_msg_t msg = {0};
                strncpy(msg.text, text->valuestring, sizeof(msg.text) - 1);
                msg.source = MSG_SOURCE_TELEGRAM;
                msg.chat_id = incoming_chat_id;

                ESP_LOGI(TAG, "Rx (update_id=%lld): %s",
                         (long long)incoming_id, msg.text);

                message_bus_post_inbound(s_bus, &msg, pdMS_TO_TICKS(100));
            }
        }
    }

    /* Stale poll streak detection */
    if (result_count > 0 && stale_count == result_count && accepted_count == 0) {
        s_stale_poll_streak++;
        if ((s_stale_poll_streak % TELEGRAM_STALE_POLL_LOG_INTERVAL) == 0) {
            ESP_LOGW(TAG, "Stale-only poll streak=%u (result_count=%d)",
                     (unsigned)s_stale_poll_streak, result_count);
        }
    } else if (s_stale_poll_streak > 0) {
        ESP_LOGI(TAG, "Stale-only poll streak cleared at %u",
                 (unsigned)s_stale_poll_streak);
        s_stale_poll_streak = 0;
    }

    capture_net_diag(&after);
    log_http_diag("getUpdates", NULL, ESP_OK, 200, started_us, ctx.written,
                  result_count, stale_count, accepted_count, &before, &after);

    cJSON_Delete(root);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Telegram API: sendMessage
 * ------------------------------------------------------------------------- */

static esp_err_t tg_send_message(int64_t chat_id, const char *text)
{
    if (strlen(s_token) == 0 || chat_id == 0) {
        ESP_LOGW(TAG, "Cannot send - not configured or no chat ID");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s/sendMessage", TELEGRAM_API_URL, s_token);

    /* Build JSON body with snprintf — avoids cJSON malloc */
    char chat_id_str[24];
    format_int64_decimal(chat_id, chat_id_str, sizeof(chat_id_str));

    /* Estimate body size: fixed overhead + escaped text */
    size_t text_len = text ? strlen(text) : 0;
    size_t body_sz = 64 + strlen(chat_id_str) + text_len * 2; /* worst case: every char escaped */
    if (body_sz > 4096) body_sz = 4096;

    char *body = malloc(body_sz);
    if (!body) return ESP_ERR_NO_MEM;

    int blen = snprintf(body, body_sz, "{\"chat_id\":%s,\"text\":\"", chat_id_str);

    /* JSON-escape the text inline */
    const char *sp = text ? text : "";
    while (*sp && blen < (int)body_sz - 3) {
        unsigned char c = (unsigned char)*sp++;
        if      (c == '"')  { body[blen++] = '\\'; body[blen++] = '"';  }
        else if (c == '\\') { body[blen++] = '\\'; body[blen++] = '\\'; }
        else if (c == '\n') { body[blen++] = '\\'; body[blen++] = 'n';  }
        else if (c == '\r') { body[blen++] = '\\'; body[blen++] = 'r';  }
        else if (c == '\t') { body[blen++] = '\\'; body[blen++] = 't';  }
        else                { body[blen++] = (char)c; }
    }
    body[blen++] = '"';
    body[blen++] = '}';
    body[blen]   = '\0';

    /* Use static response buffer */
    http_ctx_t ctx = {
        .buf = s_send_resp_buf,
        .buf_sz = sizeof(s_send_resp_buf),
    };
    s_send_resp_buf[0] = '\0';

    esp_err_t err = tg_http_post(url, body, &ctx);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sendMessage failed: %s", esp_err_to_name(err));
        if (s_send_resp_buf[0] != '\0') {
            ESP_LOGW(TAG, "Response: %.200s", s_send_resp_buf);
        }
    } else {
        ESP_LOGI(TAG, "Sent to %lld: %.60s...", (long long)chat_id, text);
    }

    free(body);
    return err;
}

/* -------------------------------------------------------------------------
 * Flush pending updates on startup
 * ------------------------------------------------------------------------- */

static esp_err_t tg_flush_pending_updates(void)
{
    char url[384];
    http_ctx_t ctx = {
        .buf = s_poll_buf,
        .buf_sz = sizeof(s_poll_buf),
    };
    s_poll_buf[0] = '\0';

    snprintf(url, sizeof(url),
             "%s%s/getUpdates?timeout=0&limit=1&offset=-1",
             TELEGRAM_API_URL, s_token);

    esp_err_t err = tg_http_get(url, &ctx);
    if (err != ESP_OK) {
        return err;
    }

    int64_t latest_id = 0;
    if (tg_extract_max_update_id(ctx.buf, &latest_id)) {
        s_last_update_id = latest_id;
        s_stale_poll_streak = 0;
        save_offset_to_nvs(s_last_update_id);
        ESP_LOGI(TAG, "Flushed pending updates up to update_id=%lld",
                 (long long)s_last_update_id);
    } else {
        ESP_LOGI(TAG, "No pending updates to flush");
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Telegram Poll Task
 * ------------------------------------------------------------------------- */

static void tg_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Polling task started");

    while (1) {
        if (strlen(s_token) > 0) {
            esp_err_t err = tg_get_updates();
            if (err != ESP_OK) {
                s_consecutive_failures++;
                int backoff = get_backoff_delay_ms();
                ESP_LOGW(TAG, "Poll failed (%d consecutive), backoff %dms",
                         s_consecutive_failures, backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff));
            } else {
                if (s_consecutive_failures > 0) {
                    ESP_LOGI(TAG, "Poll recovered after %d failures", s_consecutive_failures);
                    s_consecutive_failures = 0;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }

        vTaskDelay(pdMS_TO_TICKS(TELEGRAM_POLL_INTERVAL));
    }
}

/* -------------------------------------------------------------------------
 * Telegram Output Task
 * ------------------------------------------------------------------------- */

/* Static buffer for output task — only accessed by tg_output_task */
static telegram_msg_t s_out_msg;

static void tg_output_task(void *arg)
{
    while (1) {
        if (xQueueReceive(s_tg_out_queue, &s_out_msg, portMAX_DELAY) == pdTRUE) {
            int64_t target = resolve_target_chat_id(s_out_msg.chat_id);
            if (target == 0) continue;

            tg_send_message(target, s_out_msg.text);
        }
    }
}

/* -------------------------------------------------------------------------
 * Channel Ops
 * ------------------------------------------------------------------------- */

static esp_err_t telegram_start(message_bus_t *bus)
{
    s_bus = bus;

    if (!tg_load_credentials()) {
        ESP_LOGW(TAG, "Telegram disabled — no token");
        return ESP_ERR_NOT_FOUND;
    }

    /* Create output queue */
    s_tg_out_queue = xQueueCreate(TELEGRAM_OUTPUT_QUEUE_LENGTH, sizeof(telegram_msg_t));
    if (!s_tg_out_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Create HTTP mutex to serialize poll and send */
    s_tg_http_mutex = xSemaphoreCreateMutex();
    if (!s_tg_http_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Flush pending updates on startup */
    if (TELEGRAM_FLUSH_ON_START) {
        ESP_LOGI(TAG, "Flushing pending updates...");
        esp_err_t flush_err = tg_flush_pending_updates();
        if (flush_err != ESP_OK) {
            ESP_LOGW(TAG, "Flush failed, may replay old messages");
        }
    }

    /* Start tasks */
    if (xTaskCreate(tg_poll_task, "tg_poll", TG_POLL_TASK_STACK_SIZE,
                    NULL, TG_POLL_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll task");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(tg_output_task, "tg_out", TG_OUTPUT_TASK_STACK_SIZE,
                    NULL, CHANNEL_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create output task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Telegram bot started");
    return ESP_OK;
}

static bool telegram_is_available(void)
{
    /* Check if token is configured */
#ifdef CONFIG_ESPCLAW_TELEGRAM_TOKEN
    return strlen(CONFIG_ESPCLAW_TELEGRAM_TOKEN) > 0;
#else
    char buf[128];
    return nvs_mgr_get_str(NVS_KEY_TG_TOKEN, buf, sizeof(buf)) == ESP_OK;
#endif
}

const channel_ops_t telegram_channel_ops = {
    .name = "telegram",
    .start = telegram_start,
    .is_available = telegram_is_available,
};

/* -------------------------------------------------------------------------
 * Public API: post message to Telegram output queue
 * ------------------------------------------------------------------------- */

void telegram_post(const char *text, int64_t chat_id)
{
    if (!s_tg_out_queue) return;

    telegram_msg_t *msg = calloc(1, sizeof(telegram_msg_t));
    if (!msg) {
        ESP_LOGE(TAG, "OOM for telegram_post");
        return;
    }
    strncpy(msg->text, text, sizeof(msg->text) - 1);
    msg->chat_id = chat_id;

    if (xQueueSend(s_tg_out_queue, msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Telegram output queue full");
    }
    free(msg);
}
