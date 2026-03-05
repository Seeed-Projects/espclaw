/*
 * ESPClaw - tool/tool_network.c
 * Network tools: WiFi scan, HTTP requests, etc.
 * Included directly by tool_registry.c (not a standalone TU).
 */
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "util/json_util.h"
#include "net/wifi_manager.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations */
static bool tool_wifi_scan(const char *input_json, char *result_buf, size_t result_sz)
__attribute__((unused));

/*
 * WiFi Scan Tool
 * Scans nearby APs and returns results as JSON-like string.
 * Note: This will temporarily disconnect from current AP (2-5 seconds).
 */
static bool tool_wifi_scan(const char *input_json, char *result_buf, size_t result_sz)
{
    (void)input_json;
    
    /* Default configuration for scan */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300,
            },
        },
    };

    ESP_LOGI(TAG, "Starting WiFi scan...");

    /* Remember if we were connected */
    bool was_connected = wifi_mgr_is_connected();

    /* Start scan (will disconnect temporarily) */
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        snprintf(result_buf, result_sz, "Error: WiFi scan failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Get number of APs found */
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        snprintf(result_buf, result_sz, "No WiFi networks found.");
        return true;
    }

    /* Limit to 20 APs to avoid buffer overflow */
    if (ap_count > 20) ap_count = 20;

    /* Allocate buffer for AP records */
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        snprintf(result_buf, result_sz, "Error: Out of memory");
        return false;
    }

    /* Get AP records */
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (err != ESP_OK) {
        free(ap_list);
        snprintf(result_buf, result_sz, "Error: Failed to get scan results: %s", esp_err_to_name(err));
        return false;
    }

    /* Build result string */
    int pos = 0;
    pos += snprintf(result_buf + pos, result_sz - pos, "Found %d WiFi networks:\n", ap_count);
    
    for (int i = 0; i < ap_count && pos < (int)result_sz - 100; i++) {
        wifi_ap_record_t *ap = &ap_list[i];
        
        /* Auth mode string */
        const char *auth = "Open";
        switch (ap->authmode) {
            case WIFI_AUTH_WEP:        auth = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:    auth = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK:   auth = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK:   auth = "WPA3"; break;
            default: break;
        }

        /* Channel from primary */
        uint8_t channel = ap->primary;

        pos += snprintf(result_buf + pos, result_sz - pos,
            "%d. %s (Ch:%d, RSSI:%d, %s)\n",
            i + 1,
            ap->ssid[0] ? (char *)ap->ssid : "[Hidden]",
            channel,
            ap->rssi,
            auth);
    }

    free(ap_list);

    /* Note about reconnection */
    if (was_connected) {
        pos += snprintf(result_buf + pos, result_sz - pos, 
            "\n(Reconnecting to previous network...)");
    }

    ESP_LOGI(TAG, "WiFi scan complete: %d APs found", ap_count);
    return true;
}
