#include "rpc.h"

#include "ble.h"
#include "board_identity.h"
#include "cfg.h"
#include "wifi.h"
#include "feishu.h"
#include "cmd.h"
#include "cmd_policy.h"
#include "device_status.h"
#include "net_health.h"
#include "sms.h"
#include "health.h"
#include "sms_spool.h"
#include "peer_mark.h"
#include "factory_reset.h"
#include "firmware_update.h"
#include "runtime.h"
#include "telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "rpc";
static SemaphoreHandle_t s_rpc_lock;

esp_err_t rpc_init(void)
{
    /* Called during single-threaded service startup by BLE and LAN. */
    if (!s_rpc_lock) s_rpc_lock = xSemaphoreCreateMutex();
    return s_rpc_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool rpc_complete_setup(void)
{
    if (!s_rpc_lock || xSemaphoreTake(s_rpc_lock, 0) != pdTRUE) return false;
    bool complete = false;
    if (!factory_reset_pending()) {
        app_cfg_t saved;
        complete = cfg_setup_done() ||
                   (wifi_connected() && feishu_ws_connected() &&
                    cfg_read_setup_credentials(&saved) == ESP_OK && wifi_online_for(&saved) &&
                    feishu_ws_authenticated_for(saved.app_id, saved.app_secret) &&
                    cfg_commit_setup(&saved) == ESP_OK);
    }
    xSemaphoreGive(s_rpc_lock);
    return complete;
}

static const char *json_string(cJSON *root, const char *key, bool *present)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (present) *present = item != NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool copy_bounded(char *dst, size_t dst_len, const char *src, size_t max_len,
                         bool allow_empty)
{
    if (!src) return false;
    size_t n = strlen(src);
    if ((!allow_empty && n == 0) || n > max_len || n >= dst_len) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool exact_integer(cJSON *item, int min_value, int max_value, int *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < min_value ||
        item->valuedouble > max_value) return false;
    int converted = (int)item->valuedouble;
    if ((double)converted != item->valuedouble) return false;
    *value = converted;
    return true;
}

static bool valid_ipv4(const char *text)
{
    esp_ip4_addr_t address = {0};
    return text && esp_netif_str_to_ip4(text, &address) == ESP_OK && address.addr != 0;
}

static bool valid_wifi_config(const app_cfg_t *cfg)
{
    size_t ssid_len = strlen(cfg->wifi_ssid), pass_len = strlen(cfg->wifi_pass);
    if (!ssid_len || ssid_len > 32 || pass_len > 63) return false;
    if (cfg->dhcp) return true;
    const char *dns = cfg->dns[0] ? cfg->dns : cfg->gw;
    return valid_ipv4(cfg->ip) && valid_ipv4(cfg->mask) &&
           valid_ipv4(cfg->gw) && valid_ipv4(dns);
}

static const char *telemetry_state_name(telemetry_state_t state)
{
    switch (state) {
    case TELEMETRY_OK: return "ok";
    case TELEMETRY_WARMING_UP: return "warming_up";
    case TELEMETRY_UNSUPPORTED: return "unsupported";
    case TELEMETRY_OUT_OF_RANGE: return "out_of_range";
    default: return "unavailable";
    }
}

static cJSON *telemetry_data(void)
{
    telemetry_snapshot_t sample;
    telemetry_read(&sample);
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    cJSON *cpu = cJSON_AddObjectToObject(data, "cpu");
    if (!cpu) { cJSON_Delete(data); return NULL; }
    cJSON_AddStringToObject(cpu, "state", telemetry_state_name(sample.cpu.state));
    cJSON_AddStringToObject(cpu, "source", "freertos_idle_task");
    if (sample.cpu.state == TELEMETRY_OK) {
        cJSON *cores = cJSON_AddArrayToObject(cpu, "core_pct");
        if (!cores) { cJSON_Delete(data); return NULL; }
        for (unsigned i = 0; i < sample.cpu.cores; ++i) {
            cJSON *percent = cJSON_CreateNumber(sample.cpu.core_pct[i]);
            if (!percent) { cJSON_Delete(data); return NULL; }
            cJSON_AddItemToArray(cores, percent);
        }
        cJSON_AddNumberToObject(cpu, "used_pct", sample.cpu.used_pct);
        cJSON_AddNumberToObject(cpu, "window_ms", sample.cpu.window_ms);
        cJSON_AddNumberToObject(cpu, "age_ms", sample.cpu.age_ms);
    } else {
        cJSON_AddNullToObject(cpu, "core_pct");
        cJSON_AddNullToObject(cpu, "used_pct");
        cJSON_AddNullToObject(cpu, "window_ms");
        cJSON_AddNullToObject(cpu, "age_ms");
    }
    cJSON_AddStringToObject(data, "temperature_state", telemetry_state_name(sample.temperature_state));
    if (sample.temperature_state == TELEMETRY_OK) {
        cJSON_AddNumberToObject(data, "temperature_c", sample.temperature_c);
        cJSON_AddNumberToObject(data, "temperature_age_ms", sample.temperature_age_ms);
    } else {
        cJSON_AddNullToObject(data, "temperature_c");
        cJSON_AddNullToObject(data, "temperature_age_ms");
    }
    cJSON_AddNumberToObject(data, "uptime_s", (double)sample.uptime_s);
    cJSON_AddNumberToObject(data, "heap_free_bytes", sample.heap_free_bytes);
    cJSON_AddNumberToObject(data, "heap_total_bytes", sample.heap_total_bytes);
    cJSON_AddNumberToObject(data, "heap_min_free_bytes", sample.heap_min_free_bytes);
    if (sample.psram_total_bytes) {
        cJSON_AddNumberToObject(data, "psram_free_bytes", sample.psram_free_bytes);
        cJSON_AddNumberToObject(data, "psram_total_bytes", sample.psram_total_bytes);
    } else {
        cJSON_AddNullToObject(data, "psram_free_bytes");
        cJSON_AddNullToObject(data, "psram_total_bytes");
    }
    return data;
}

static void add_age_ms(cJSON *object, const char *key, int64_t stamp_ms)
{
    if (stamp_ms <= 0) {
        cJSON_AddNullToObject(object, key);
        return;
    }
    int64_t age = esp_timer_get_time() / 1000 - stamp_ms;
    cJSON_AddNumberToObject(object, key, (double)(age > 0 ? age : 0));
}

static const char *reset_reason_name(uint8_t reason)
{
    switch (reason) {
    case MODEM_RESET_REASON_AT: return "at";
    case MODEM_RESET_REASON_SMS: return "sms";
    case MODEM_RESET_REASON_MANUAL: return "manual";
    case MODEM_RESET_REASON_SCHEDULED: return "scheduled";
    default: return "";
    }
}

static cJSON *status_data(void)
{
    const app_cfg_t *cfg = cfg_get();
    char ssid[40] = "", ip[20] = "";
    int rssi = 0;
    wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);

    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    cJSON *slots = cJSON_AddArrayToObject(data, "slots");
    cJSON *feishu = cJSON_AddObjectToObject(data, "feishu");
    cJSON *wifi = cJSON_AddObjectToObject(data, "wifi");
    cJSON *clock = cJSON_AddObjectToObject(data, "time");
    cJSON *firmware = cJSON_AddObjectToObject(data, "firmware");
    if (!slots || !feishu || !wifi || !clock || !firmware) { cJSON_Delete(data); return NULL; }
    cJSON_AddStringToObject(firmware, "version", firmware_version());
    cJSON_AddStringToObject(firmware, "board", SMSF_BOARD_LAYOUT);
    cJSON_AddBoolToObject(firmware, "can_upload", firmware_can_upload());
    for (int i = 0; i < 2; ++i) {
        cJSON *slot = cJSON_CreateObject();
        if (!slot) { cJSON_Delete(data); return NULL; }
        cJSON_AddNumberToObject(slot, "n", i + 1);
        cJSON_AddStringToObject(slot, "name", cfg->slot_name[i]);
        cJSON_AddStringToObject(slot, "cfg_num", cfg->slot_num[i]);
        const char *number = cmd_slot_number(i);
        char local_key[12];
        cJSON_AddStringToObject(slot, "num", cmd_policy_local_number(number, local_key, sizeof(local_key)) ? number : "");
        if (device_slots[i].valid) cJSON_AddBoolToObject(slot, "reg", device_slots[i].registered);
        else cJSON_AddNullToObject(slot, "reg");
        cJSON_AddStringToObject(slot, "op", device_slots[i].valid ? device_slots[i].operator_name : "");
        if (device_slots[i].valid && device_slots[i].csq >= 0)
            cJSON_AddNumberToObject(slot, "csq", device_slots[i].csq);
        else cJSON_AddNullToObject(slot, "csq");
        cJSON_AddItemToArray(slots, slot);
    }
    cJSON_AddBoolToObject(data, "setup_done", cfg_setup_done());
    cJSON_AddStringToObject(feishu, "state", feishu_ws_authenticated_for(cfg->app_id, cfg->app_secret) ? "online" : "offline");
    cJSON_AddBoolToObject(feishu, "configured", feishu_configured());
    cJSON_AddBoolToObject(feishu, "bound", cfg->open_id[0] || cfg->chat_id[0]);
    cJSON_AddStringToObject(feishu, "app_id", cfg->app_id);
    /* Diagnostics: "connected but silent" and "still retrying" used to look
     * identical from the outside, which made the 30 h outage invisible. */
    cJSON_AddBoolToObject(feishu, "alive", feishu_ws_alive());
    cJSON_AddNumberToObject(feishu, "rx_age_ms", (double)feishu_ws_rx_age_ms());
    cJSON_AddNumberToObject(feishu, "connected_ms", (double)feishu_ws_connected_age_ms());
    cJSON_AddNumberToObject(feishu, "reconnects", feishu_ws_reconnects());
    cJSON_AddNumberToObject(feishu, "send_fail_streak", (double)feishu_send_fail_streak());
    cJSON *net = cJSON_AddObjectToObject(data, "net");
    if (net) net_health_status_json(net);
    cJSON *spool = cJSON_AddObjectToObject(data, "spool");
    if (spool) sms_spool_status_json(spool);
    cJSON *sms = cJSON_AddObjectToObject(data, "sms");
    if (sms) {
        /* journal_ok=false means dedup is degraded (possible duplicate forward after
         * a crash); sweep_ms shows how far the idle SIM sweep has backed off. */
        cJSON_AddBoolToObject(sms, "journal_ok", sms_journal_ok());
        cJSON_AddNumberToObject(sms, "sweep_ms", (double)sms_sweep_delay_ms());
        /* Counters separate "no SMS arrived" from "arrived but not forwarded",
         * and the SIM watermark exposes a full card that would silently drop new
         * messages.  All counters are since boot. */
        sms_stats_t stats;
        sms_get_stats(&stats);
        cJSON_AddNumberToObject(sms, "cmti", stats.cmti);
        cJSON_AddNumberToObject(sms, "reads", stats.reads);
        cJSON_AddNumberToObject(sms, "delivered", stats.delivered);
        cJSON_AddNumberToObject(sms, "kept", stats.kept);
        cJSON_AddNumberToObject(sms, "deletes", stats.deletes);
        cJSON_AddNumberToObject(sms, "scans", stats.scans);
        add_age_ms(sms, "last_cmti_age_ms", stats.last_cmti_ms);
        add_age_ms(sms, "last_delivered_age_ms", stats.last_delivered_ms);
        cJSON *used = cJSON_AddArrayToObject(sms, "sim_used");
        cJSON *cap = cJSON_AddArrayToObject(sms, "sim_capacity");
        if (used && cap) {
            for (int i = 0; i < 2; ++i) {
                cJSON_AddItemToArray(used, cJSON_CreateNumber(stats.sim_used[i]));
                cJSON_AddItemToArray(cap, cJSON_CreateNumber(stats.sim_capacity[i]));
            }
        }
    }
    cJSON *modems = cJSON_AddArrayToObject(data, "modems");
    if (modems) {
        /* A bare AT probe cannot see a hung SMS/URC path; expose each layer and
         * the reset history so "silent for hours" is diagnosable. */
        for (int i = 0; i < 2; ++i) {
            modem_health_status_t health;
            health_get_status(i, &health);
            cJSON *item = cJSON_CreateObject();
            if (!item) { cJSON_Delete(data); return NULL; }
            cJSON_AddNumberToObject(item, "n", i + 1);
            cJSON_AddBoolToObject(item, "at_ok", health.at_ok);
            cJSON_AddBoolToObject(item, "cpms_ok", health.cpms_ok);
            cJSON_AddBoolToObject(item, "cnmi_ok", health.cnmi_ok);
            cJSON_AddNumberToObject(item, "probes", health.probes);
            cJSON_AddNumberToObject(item, "at_fail", health.at_fail_streak);
            cJSON_AddNumberToObject(item, "sms_fail", health.sms_fail_streak);
            cJSON_AddNumberToObject(item, "resets", health.resets);
            cJSON_AddNumberToObject(item, "repairs", health.repairs);
            add_age_ms(item, "probe_age_ms", health.last_probe_ms);
            add_age_ms(item, "reset_age_ms", health.last_reset_ms);
            cJSON_AddStringToObject(item, "reset_reason", reset_reason_name(health.last_reset_reason));
            cJSON_AddItemToArray(modems, item);
        }
    }
    cJSON_AddNumberToObject(data, "modem_reset_hours", cfg->modem_reset_hours);
    cJSON *mark = cJSON_AddObjectToObject(data, "mark");
    if (mark) {
        /* The key itself is never echoed back; enabled only reports whether a
         * service URL is configured. */
        cJSON_AddBoolToObject(mark, "enabled", peer_mark_enabled());
        /* The template is not secret (only the key is); showing it lets the
         * settings page round-trip without echoing the key. */
        cJSON_AddStringToObject(mark, "url", cfg->mark_url);
        cJSON_AddNumberToObject(mark, "queries", (double)peer_mark_queries());
        cJSON_AddNumberToObject(mark, "timeouts", (double)peer_mark_timeouts());
        cJSON_AddNumberToObject(mark, "failures", (double)peer_mark_failures());
        cJSON_AddNumberToObject(mark, "cache", (double)peer_mark_cache_entries());
        cJSON_AddNumberToObject(mark, "last_ms", (double)peer_mark_last_ms());
        cJSON_AddNumberToObject(mark, "last_bytes", (double)peer_mark_last_bytes());
        cJSON_AddStringToObject(mark, "last_label", peer_mark_last_label());
        cJSON_AddNumberToObject(mark, "timeout_ms", (double)cfg->mark_timeout_ms);
    }
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddNumberToObject(wifi, "rssi", rssi);
    cJSON_AddNumberToObject(wifi, "up", wifi_uptime_seconds());
    cJSON_AddStringToObject(wifi, "cfg_ssid", cfg->wifi_ssid);
    cJSON_AddBoolToObject(wifi, "dhcp", cfg->dhcp);
    cJSON_AddStringToObject(wifi, "cfg_ip", cfg->ip);
    cJSON_AddStringToObject(wifi, "mask", cfg->mask);
    cJSON_AddStringToObject(wifi, "gw", cfg->gw);
    cJSON_AddStringToObject(wifi, "dns", cfg->dns);
    cJSON_AddBoolToObject(clock, "synced", time_synced());
    cJSON_AddNumberToObject(clock, "utc", time(NULL));
    cJSON_AddNumberToObject(clock, "tz", cfg->tz_offset);
    return data;
}

static esp_err_t send_reply(int id, bool ok, const char *err, cJSON *data,
                            char *out, size_t out_len)
{
    cJSON *reply = cJSON_CreateObject();
    if (!reply) { cJSON_Delete(data); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(reply, "id", id);
    cJSON_AddBoolToObject(reply, "ok", ok);
    if (!ok && err) cJSON_AddStringToObject(reply, "err", err);
    if (data) cJSON_AddItemToObject(reply, "data", data);
    char *encoded = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!encoded) return ESP_ERR_NO_MEM;
    size_t n = strlen(encoded);
    if (!out || !out_len || n >= out_len) {
        free(encoded);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, encoded, n + 1);
    free(encoded);
    return ESP_OK;
}

static esp_err_t error_reply(int id, const char *err, char *out, size_t out_len)
{
    esp_err_t result = send_reply(id, false, err, NULL, out, out_len);
    if (result != ESP_OK && out && out_len) {
        snprintf(out, out_len, "{\"id\":%d,\"ok\":false,\"err\":\"response\"}", id);
    }
    return result;
}

static bool explicit_reset_confirmation(cJSON *request)
{
    bool confirmed = false;
    cJSON *field;
    cJSON_ArrayForEach(field, request) {
        if (field->string && strcmp(field->string, "confirm") == 0) {
            if (confirmed || !cJSON_IsTrue(field)) return false;
            confirmed = true;
        }
    }
    return confirmed;
}

esp_err_t rpc_dispatch_json(const char *request, char *response, size_t response_len,
                            bool via_ble, rpc_post_reply_action_t *post_action)
{
    if (post_action) *post_action = RPC_POST_NONE;
    if (response && response_len) response[0] = '\0';
    if (!request || !response || response_len < 64 || !post_action) return ESP_ERR_INVALID_ARG;
    if (!s_rpc_lock) {
        snprintf(response, response_len, "{\"id\":0,\"ok\":false,\"err\":\"unavailable\"}");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_ParseWithOpts(request, NULL, true);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return error_reply(0, "json", response, response_len);
    }
    int id = 0;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id_item && !exact_integer(id_item, 0, 2147483647, &id)) {
        cJSON_Delete(root);
        return error_reply(0, "id", response, response_len);
    }
    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring[0]) {
        cJSON_Delete(root);
        return error_reply(id, "cmd", response, response_len);
    }
    const char *cmd = cmd_item->valuestring;

    xSemaphoreTake(s_rpc_lock, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    cJSON *data = NULL;
    const char *err = NULL;

    if (factory_reset_pending()) {
        err = "reset_pending";
    } else if (via_ble && cfg_setup_done()) {
        err = "setup_complete";
    } else if (!strcmp(cmd, "factory_reset")) {
        if (via_ble) err = "lan_required";
        else if (!explicit_reset_confirmation(root)) err = "confirm_required";
        else if (!firmware_restart_reserve()) err = "firmware_busy";
        else if (factory_reset_request() != ESP_OK) {
            firmware_restart_release();
            err = "reset_storage_failed";
        }
        else *post_action = RPC_POST_RESTART;
    } else if (!strcmp(cmd, "status")) {
        data = status_data();
        if (data && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "telemetry"))) {
            cJSON *sample = telemetry_data();
            if (!sample || !cJSON_AddItemToObject(data, "telemetry", sample)) {
                cJSON_Delete(sample);
                cJSON_Delete(data);
                data = NULL;
            }
        }
        if (!data) err = "memory";
    } else if (!strcmp(cmd, "telemetry")) {
        data = telemetry_data();
        if (!data) err = "memory";
    } else if (!strcmp(cmd, "wifi_scan")) {
        char *scan = malloc(4096);
        if (!scan) err = "memory";
        else if (wifi_scan(scan, 4096) != ESP_OK) err = "scan";
        else {
            data = cJSON_Parse(scan);
            if (!data) err = "scan";
        }
        free(scan);
    } else if (!strcmp(cmd, "wifi_set")) {
        app_cfg_t backup = *cfg_get(), candidate = backup;
        bool present;
        const char *value = json_string(root, "ssid", &present);
        if (present && !copy_bounded(candidate.wifi_ssid, sizeof(candidate.wifi_ssid), value, 32, false))
            err = "ssid";
        value = json_string(root, "pass", &present);
        if (present && (!value || strlen(value) > 63)) err = "password";
        else if (present && value[0]) copy_bounded(candidate.wifi_pass, sizeof(candidate.wifi_pass), value, 63, true);

        cJSON *dhcp = cJSON_GetObjectItemCaseSensitive(root, "dhcp");
        if (!err && dhcp) {
            if (!cJSON_IsBool(dhcp)) err = "dhcp";
            else candidate.dhcp = cJSON_IsTrue(dhcp);
        }
        const char *const keys[] = { "ip", "mask", "gw", "dns" };
        char *const dests[] = { candidate.ip, candidate.mask, candidate.gw, candidate.dns };
        const size_t lens[] = { sizeof(candidate.ip), sizeof(candidate.mask), sizeof(candidate.gw), sizeof(candidate.dns) };
        for (size_t i = 0; !err && i < 4; ++i) {
            value = json_string(root, keys[i], &present);
            if (present && !copy_bounded(dests[i], lens[i], value, lens[i] - 1, true)) err = keys[i];
        }
        if (!err && !valid_wifi_config(&candidate)) err = "network_config_invalid";
        if (!err) {
            *cfg_mut() = candidate;
            if (cfg_save() != ESP_OK) { *cfg_mut() = backup; err = "save_failed"; }
            else if (post_action) *post_action = RPC_POST_WIFI_RECONFIGURE;
        }
    } else if (!strcmp(cmd, "feishu_set")) {
        app_cfg_t backup = *cfg_get(), candidate = backup;
        bool present;
        const char *value = json_string(root, "app_id", &present);
        if (present && (!value || (value[0] &&
            !copy_bounded(candidate.app_id, sizeof(candidate.app_id), value, 63, false)))) err = "app_id";
        value = json_string(root, "app_secret", &present);
        if (!err && present && (!value || (value[0] &&
            !copy_bounded(candidate.app_secret, sizeof(candidate.app_secret), value, 127, false)))) err = "app_secret";
        bool account_changed = strcmp(candidate.app_id, backup.app_id) != 0;
        if (account_changed) { candidate.open_id[0] = 0; candidate.chat_id[0] = 0; }
        if (!err) {
            *cfg_mut() = candidate;
            if (cfg_save() != ESP_OK) { *cfg_mut() = backup; err = "save_failed"; }
            else {
                feishu_cfg_t f = {0};
                strlcpy(f.app_id, candidate.app_id, sizeof(f.app_id));
                strlcpy(f.app_secret, candidate.app_secret, sizeof(f.app_secret));
                strlcpy(f.open_id, candidate.open_id, sizeof(f.open_id));
                strlcpy(f.chat_id, candidate.chat_id, sizeof(f.chat_id));
                if (feishu_init(&f) != ESP_OK) {
                    *cfg_mut() = backup;
                    (void)cfg_save();
                    feishu_cfg_t old = {0};
                    strlcpy(old.app_id, backup.app_id, sizeof(old.app_id));
                    strlcpy(old.app_secret, backup.app_secret, sizeof(old.app_secret));
                    strlcpy(old.open_id, backup.open_id, sizeof(old.open_id));
                    strlcpy(old.chat_id, backup.chat_id, sizeof(old.chat_id));
                    (void)feishu_init(&old);
                    err = "apply_failed";
                } else network_config_changed();
            }
        }
    } else if (!strcmp(cmd, "slot_set")) {
        int slot = 0;
        if (!exact_integer(cJSON_GetObjectItemCaseSensitive(root, "slot"), 1, 2, &slot)) err = "slot";
        else {
            app_cfg_t backup = *cfg_get(), candidate = backup;
            const char *value = json_string(root, "name", NULL);
            bool present = cJSON_GetObjectItemCaseSensitive(root, "name") != NULL;
            if (present && !copy_bounded(candidate.slot_name[slot - 1], sizeof(candidate.slot_name[0]), value, 23, false)) err = "name";
            value = json_string(root, "number", NULL);
            present = cJSON_GetObjectItemCaseSensitive(root, "number") != NULL;
            if (!err && present) {
                char key[12];
                if (!copy_bounded(candidate.slot_num[slot - 1], sizeof(candidate.slot_num[0]), value, 31, true) ||
                    (value[0] && !cmd_policy_local_number(value, key, sizeof(key)))) err = "number";
            }
            if (!err) {
                *cfg_mut() = candidate;
                if (cfg_save() != ESP_OK) { *cfg_mut() = backup; err = "save_failed"; }
                else device_slot_config_changed(slot - 1, candidate.slot_name[slot - 1], candidate.slot_num[slot - 1]);
            }
        }
    } else if (!strcmp(cmd, "tz_set")) {
        int offset = 0;
        if (!exact_integer(cJSON_GetObjectItemCaseSensitive(root, "offset"), -12, 14, &offset)) err = "timezone";
        else {
            int8_t old = cfg_get()->tz_offset;
            cfg_mut()->tz_offset = (int8_t)offset;
            if (cfg_save() != ESP_OK) { cfg_mut()->tz_offset = old; err = "save_failed"; }
            else cfg_apply_timezone();
        }
    } else if (!strcmp(cmd, "mark_set")) {
        app_cfg_t backup = *cfg_get(), candidate = backup;
        bool present;
        const char *value = json_string(root, "url", &present);
        if (present) {
            if (!value) err = "url";
            else if (!copy_bounded(candidate.mark_url, sizeof(candidate.mark_url), value, 191,
                                   true))
                err = "url";
            else if (candidate.mark_url[0] &&
                     (!strstr(candidate.mark_url, "{number}") ||
                      (strncmp(candidate.mark_url, "http://", 7) != 0 &&
                       strncmp(candidate.mark_url, "https://", 8) != 0)))
                err = "url";
        }
        value = json_string(root, "key", &present);
        if (!err && present) {
            if (!value) err = "key";
            else if (!copy_bounded(candidate.mark_key, sizeof(candidate.mark_key), value, 63, true))
                err = "key";
        }
        if (!err) {
            int timeout = 0;
            if (exact_integer(cJSON_GetObjectItemCaseSensitive(root, "timeout_ms"), 500, 5000,
                              &timeout))
                candidate.mark_timeout_ms = (uint16_t)timeout;
        }
        if (!err) {
            *cfg_mut() = candidate;
            if (cfg_save() != ESP_OK) { *cfg_mut() = backup; err = "save_failed"; }
        }
    } else if (!strcmp(cmd, "mark_test")) {
        /* Lets the owner check the configured service without waiting for a call. */
        bool has_number = false;
        const char *number = json_string(root, "number", &has_number);
        if (!peer_mark_enabled()) err = "not_configured";
        else if (!has_number || !number || !number[0] || strlen(number) > 24) err = "number";
        else {
            const char *label = peer_mark_lookup(number);
            data = cJSON_CreateObject();
            if (!data) err = "memory";
            else {
                cJSON_AddStringToObject(data, "label", label ? label : "");
                cJSON_AddNumberToObject(data, "ms", peer_mark_last_ms());
                cJSON_AddNumberToObject(data, "bytes", peer_mark_last_bytes());
            }
        }
    } else if (!strcmp(cmd, "sms_scan")) {
        /* Run the SIM sweep now instead of waiting for the backoff timer. */
        sms_sweep_now();
        data = cJSON_CreateObject();
        if (!data) err = "memory";
        else cJSON_AddBoolToObject(data, "queued", true);
    } else if (!strcmp(cmd, "modem_probe")) {
        health_request_probe();
        data = cJSON_CreateObject();
        if (!data) err = "memory";
        else cJSON_AddBoolToObject(data, "queued", true);
    } else if (!strcmp(cmd, "modem_reset")) {
        int slot = 0;
        if (!exact_integer(cJSON_GetObjectItemCaseSensitive(root, "slot"), 1, 2, &slot)) err = "slot";
        else if (!explicit_reset_confirmation(root)) err = "confirm_required";
        else if (firmware_busy()) err = "firmware_busy";
        else if (!health_request_reset(slot - 1)) err = "call_active";
        else {
            data = cJSON_CreateObject();
            if (!data) err = "memory";
            else cJSON_AddNumberToObject(data, "slot", slot);
        }
    } else if (!strcmp(cmd, "modem_reset_set")) {
        int hours = 0;
        if (!exact_integer(cJSON_GetObjectItemCaseSensitive(root, "hours"), 0,
                           MODEM_RESET_HOURS_MAX, &hours)) err = "hours";
        else {
            uint8_t old = cfg_get()->modem_reset_hours;
            cfg_mut()->modem_reset_hours = (uint8_t)hours;
            if (cfg_save() != ESP_OK) { cfg_mut()->modem_reset_hours = old; err = "save_failed"; }
        }
    } else if (!strcmp(cmd, "reboot")) {
        if (!firmware_restart_reserve()) err = "firmware_busy";
        else *post_action = RPC_POST_RESTART;
    } else {
        err = "unknown";
    }

    esp_err_t encoded = send_reply(id, err == NULL, err, data, response, response_len);
    if (encoded != ESP_OK) {
        if (*post_action != RPC_POST_NONE) {
            snprintf(response, response_len, "{\"id\":%d,\"ok\":true}", id);
            encoded = ESP_OK;
        } else {
            snprintf(response, response_len,
                     "{\"id\":%d,\"ok\":false,\"err\":\"response\"}", id);
        }
    }
    xSemaphoreGive(s_rpc_lock);
    cJSON_Delete(root);
    return encoded == ESP_OK ? result : encoded;
}

void rpc_run_post_reply_action(rpc_post_reply_action_t action)
{
    if (action == RPC_POST_WIFI_RECONFIGURE) {
        if (!s_rpc_lock) return;
        xSemaphoreTake(s_rpc_lock, portMAX_DELAY);
        if (factory_reset_pending()) {
            xSemaphoreGive(s_rpc_lock);
            return;
        }
        app_cfg_t snapshot = *cfg_get();
        xSemaphoreGive(s_rpc_lock);
        esp_err_t err = wifi_reconfigure(&snapshot);
        network_config_changed();
        if (err != ESP_OK) ESP_LOGW(TAG, "saved Wi-Fi config could not be applied (%d)", (int)err);
    } else if (action == RPC_POST_RESTART) {
        /* Do not restart on top of unsent events: a firmware update that reboots
         * during an outage must not lose the parked SMS/call notices. */
        sms_spool_persist();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}
