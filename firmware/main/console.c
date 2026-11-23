#include "console.h"
#include "firmware_update.h"
#include "pdu.h"
#include "sms.h"
#include "pbbp2.h"
#include "cmd.h"
#include "cmd_policy.h"
#include "feishu.h"
#include "cfg.h"
#include "wifi.h"
#include "modem_timing.h"
#include "power_policy.h"
#include "sim_number_policy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "console";

static modem_t *s_modems;

static const char *number_prefix_class(const char *number)
{
    if (!strncmp(number, "0086", 4)) return "0086";
    if (!strncmp(number, "+86", 3)) return "+86";
    if (!strncmp(number, "86", 2)) return "86";
    if (number[0] == '1') return "national";
    return "other";
}

static void print_number_result_shapes(const modem_t *m, const char *response,
                                       const char *anchor, const char *kind)
{
    unsigned records = 0;
    size_t anchor_len = strlen(anchor);
    for (const char *line = response; line && *line;) {
        while (*line == '\r' || *line == '\n') ++line;
        const char *end = line + strcspn(line, "\r\n");
        if ((size_t)(end - line) >= anchor_len && !memcmp(line, anchor, anchor_len)) {
            char field2[48] = {0};
            char shape[256];
            size_t n = 0;
            for (const char *p = line + anchor_len; p < end && n + 1 < sizeof(shape); ++p) {
                unsigned char c = (unsigned char)*p;
                shape[n++] = c == ' ' || c == ',' || c == '"' || c == '+'
                           ? (char)c : '#';
            }
            shape[n] = '\0';
            bool have_field2 = sim_number_csv_field(line + anchor_len, end, 1,
                                                     field2, sizeof(field2));
            ++records;
            printf("[M%d] %s record=%u shape=%s field2_prefix=%s\n",
                   m->idx + 1, kind, records, shape,
                   have_field2 ? number_prefix_class(field2) : "other");
        }
        line = end;
    }
    printf("[M%d] %s records=%u\n", m->idx + 1, kind, records);
}

static bool is_msisdn_record_read(const char *cmd)
{
    unsigned length = 0;
    int used = 0;
    return sscanf(cmd, "AT+CRSM=178,28480,1,4,%u%n", &length, &used) == 1 &&
           length > 0 && cmd[used] == '\0';
}

static void print_crsm_display_match(const modem_t *m, const char *response)
{
    uint8_t record[255];
    size_t record_len = 0;
    char decoded[32], decoded_key[12], display_key[12];
    bool matches = sim_number_parse_crsm(response, record, sizeof(record), &record_len) &&
                   sim_number_decode_msisdn(record, record_len, decoded, sizeof(decoded)) &&
                   cmd_policy_local_number(decoded, decoded_key, sizeof(decoded_key)) &&
                   cmd_policy_local_number(cmd_slot_number(m->idx), display_key, sizeof(display_key)) &&
                   !strcmp(decoded_key, display_key);
    printf("[M%d] CRSM MSISDN matches_display=%s\n", m->idx + 1,
           matches ? "yes" : "no");
}

static void cmd_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    printf("\n--- info ---\n");
    printf("idf      : %s\n", esp_get_idf_version());
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        char hash[17];
        for (int i = 0; i < 8; i++) snprintf(hash + i * 2, 3, "%02x", app->app_elf_sha256[i]);
        printf("app sha  : %s\n", hash);
    }
    printf("chip     : %s rev %d, cores %d\n", CONFIG_IDF_TARGET, ci.revision, ci.cores);
    printf("flash    : %lu bytes\n", (unsigned long)flash);
    printf("heap int : %lu free / %lu total\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    printf("heap psram: %lu free\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("uptime   : %lu s\n", (unsigned long)(xTaskGetTickCount() / 1000));
    char power[96];
    power_policy_status(power, sizeof(power));
    printf("power    : %s\n", power);
    for (int i = 0; i < MODEM_COUNT; i++) {
        printf("modem%d   : uart%d tx%d rx%d rst%d ready=%d\n", i + 1,
               s_modems[i].uart, s_modems[i].pin.tx, s_modems[i].pin.rx,
               s_modems[i].pin.rst, s_modems[i].ready);
    }
    printf("------------\n");
}

static void run_at(modem_t *m, const char *cmd, int timeout_ms)
{
    char resp[1024] = {0};
    esp_err_t err = modem_cmd(m, cmd, resp, sizeof(resp), timeout_ms);
    bool private_cmd = strstr(cmd, "CNUM") || strstr(cmd, "CPBR") || strstr(cmd, "ICCID") ||
                       strstr(cmd, "CRSM") || strstr(cmd, "CIMI") || strstr(cmd, "CMGR") || strstr(cmd, "CMGS") ||
                       strstr(cmd, "CCFC") || !strncmp(cmd, "ATD", 3) || strstr(cmd, "CLCC");
    printf("[%s] > %s  (%s)\n", modem_name(m), private_cmd ? "<sensitive AT command>" : cmd,
           err == ESP_OK ? "OK" : err == ESP_ERR_TIMEOUT ? "TIMEOUT" :
           err == MODEM_ERR_BUSY ? "BUSY" : err == MODEM_ERR_DESYNC ? "DESYNC" :
           err == MODEM_ERR_OVERFLOW ? "OVERFLOW" : "FAIL");
    /* Only expose the voice state and whether the target is the other installed SIM. */
    int reason = -1, mode = -1;
    if (sscanf(cmd, "AT+CCFC=%d,%d", &reason, &mode) == 2 && mode == 2) {
        const char *other = cmd_slot_number(1 - m->idx);
        bool on = cmd_policy_ccfc_voice(resp, 1, NULL);
        bool off = cmd_policy_ccfc_voice(resp, 0, NULL);
        const char *state = err != ESP_OK || on == off ? "unknown" : on ? "on" : "off";
        printf("[M%d] forwarding reason=%d voice=%s target=%s\n",
               m->idx + 1, reason, state, on && other &&
               cmd_policy_ccfc_voice(resp, 1, other) ? "other-installed-SIM" :
               on ? "other-or-unspecified" : "none-reported");
    }
    if (!strcmp(cmd, "AT+CNUM")) {
        print_number_result_shapes(m, resp, "+CNUM:", "CNUM");
    } else if (!strncmp(cmd, "AT+CPBR=", 8)) {
        print_number_result_shapes(m, resp, "+CPBR:", "CPBR");
    } else if (is_msisdn_record_read(cmd)) {
        print_crsm_display_match(m, resp);
    }
    if (resp[0] && !private_cmd) {
        printf("%s", resp);
        if (resp[strlen(resp) - 1] != '\n') {
            printf("\n");
        }
    }
}

static void cmd_probe(modem_t *m)
{
    printf("\n===== probe %s =====\n", modem_name(m));
    run_at(m, "AT", 1000);
    run_at(m, "ATE0", 1000);
    run_at(m, "ATI", 3000);
    run_at(m, "AT+CGMM", 3000);
    run_at(m, "AT+CPIN?", AT_SIM_IDENTITY_MS);
    run_at(m, "AT+CSQ", AT_SIGNAL_MS);
    run_at(m, "AT+CEREG?", AT_REGISTRATION_MS);
    run_at(m, "AT+COPS?", AT_OPERATOR_MS);
    run_at(m, "AT+CIMI", 3000);
    run_at(m, "AT+ICCID", AT_SIM_IDENTITY_MS);
    run_at(m, "AT+CNUM", AT_SIM_IDENTITY_MS);
    run_at(m, "AT+CPBS=\"ON\"", 3000);
    run_at(m, "AT+CPBR=1,10", 5000);
    run_at(m, "AT+CLIP?", 2000);
    run_at(m, "AT+CCFC=0,2", 8000);
    run_at(m, "AT+CFUN?", 2000);
    printf("===== probe done =====\n\n");
}

static void cmd_pdutest(void)
{
    char report[1024];
    int fails = pdu_selftest(report, sizeof(report));
    printf("\n%s--- pdu selftest: %d failed ---\n", report, fails);
}

static void cmd_pdudec(const char *hex)
{
    uint8_t buf[512];
    size_t n = pdu_hex2bin(hex, buf, sizeof(buf));
    sms_deliver_t d;
    int rc = pdu_decode_deliver(buf, n, &d);
    printf("\npdu %u bytes, rc=%d\n", (unsigned)n, rc);
    if (rc == 0) {
        printf("from   : <redacted>\n");
        printf("scts   : %ld (tz %d quarters)\n", (long)d.scts, d.scts_tz_quarters);
        printf("dcs    : 0x%02X\n", d.dcs);
        printf("concat : %s ref=%d %d/%d\n", d.has_udh ? "yes" : "no",
               d.concat_ref, d.concat_seq, d.concat_total);
        printf("text   : <redacted>\n");
    }
    printf("\n");
}

static void cmd_pdusub(const char *args)
{
    char dest[40] = {0};
    const char *p = args;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(dest)) dest[i++] = *p++;
    while (*p == ' ') p++;
    if (!dest[0] || !*p) {
        printf("usage: pdusub <num> <text>\n");
        return;
    }
    uint8_t buf[700];
    size_t off = 0;
    int tpdu = pdu_encode_submit(s_modems[0].sca[0] ? s_modems[0].sca : NULL, dest, p, 0, 0, 0,
                                 buf, sizeof(buf), &off);
    char hex[1400];
    pdu_bin2hex(buf, (size_t)tpdu + off, hex);
    printf("\ntpdu_len=%d\npdu=<redacted>\n\n", tpdu);
}

static void cmd_send(const char *args)
{
    /* send <1|2> <num> <text> */
    int slot = 0;
    char num[40] = {0};
    const char *p = args;
    if (*p == '1' || *p == '2') slot = *p++ - '1';
    else { printf("usage: send <1|2> <num> <text>\n"); return; }
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(num)) num[i++] = *p++;
    while (*p == ' ') p++;
    if (!num[0] || !*p) { printf("usage: send <1|2> <num> <text>\n"); return; }

    char dial[40], display[40];
    if (!cmd_policy_dial_number(num, dial, sizeof(dial), display, sizeof(display))) {
        printf("[send] invalid number\n");
        return;
    }
    (void)display;

    char err[256] = {0};
    printf("\n[send] slot=%d\n", slot + 1);
    int rc = sms_send(&s_modems[slot], dial, p, err, sizeof(err));
    printf("[send] rc=%d %s\n\n", rc, rc == 0 ? "OK" : err);
}

static void cmd_smsinit(void)
{
    for (int i = 0; i < MODEM_COUNT; ++i) {
        esp_err_t result = sms_modem_configure(i);
        printf("[%s] SMS configuration %s\n", modem_name(&s_modems[i]),
               result == ESP_OK ? "complete" : "pending");
    }
}

static void cmd_sendt(const char *args)
{
    int slot = 0;
    char num[40] = {0};
    const char *p = args;
    if (*p == '1' || *p == '2') slot = *p++ - '1';
    else { printf("usage: sendt <1|2> <num> <text>\n"); return; }
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(num)) num[i++] = *p++;
    while (*p == ' ') p++;
    if (!num[0] || !*p) { printf("usage: sendt <1|2> <num> <text>\n"); return; }

    modem_t *m = &s_modems[slot];
    modem_cmd(m, "AT+CMGF=1", NULL, 0, 3000);
    char resp[512] = {0};
    printf("\n[sendt] slot=%d\n", slot + 1);
    esp_err_t e = modem_sms_text(m, num, p, resp, sizeof(resp), 30000);
    printf("[sendt] %s\n", esp_err_to_name(e));
    modem_cmd(m, "AT+CMGF=0", NULL, 0, 3000);
}

static void cmd_pbbptest(void)
{
    char report[512];
    int fails = pbbp2_selftest(report, sizeof(report));
    printf("\n%s--- pbbp2 selftest: %d failed ---\n", report, fails);
}

static void cmd_cmdtest(void)
{
    char report[512];
    int fails = cmd_selftest(report, sizeof(report));
    printf("\n%s--- cmd selftest: %d failed ---\n", report, fails);
}

static void cmd_fset(const char *args)
{
    /* fset <app_id> <app_secret> */
    char id[64] = {0}, secret[128] = {0};
    const char *p = args;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(id)) id[i++] = *p++;
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(secret)) secret[i++] = *p++;
    if (!id[0] || !secret[0]) {
        printf("usage: fset <app_id> <app_secret>\n");
        return;
    }
    app_cfg_t *c = cfg_mut();
    bool account_changed = strcmp(c->app_id, id) != 0;
    strlcpy(c->app_id, id, sizeof(c->app_id));
    strlcpy(c->app_secret, secret, sizeof(c->app_secret));
    if (account_changed) { c->open_id[0] = 0; c->chat_id[0] = 0; }
    cfg_save();
    feishu_cfg_t cfg = {0};
    strlcpy(cfg.app_id, c->app_id, sizeof(cfg.app_id));
    strlcpy(cfg.app_secret, c->app_secret, sizeof(cfg.app_secret));
    strlcpy(cfg.open_id, c->open_id, sizeof(cfg.open_id));
    strlcpy(cfg.chat_id, c->chat_id, sizeof(cfg.chat_id));
    feishu_init(&cfg);
    if (wifi_connected() && time_synced()) feishu_ws_start();
    printf("\nfeishu configured & saved: %s\n", id);
}

static void cmd_fsend(const char *text)
{
    if (!feishu_configured()) { printf("feishu 未配置\n"); return; }
    char mid[64] = "";
    esp_err_t e = feishu_send_text(text, mid, sizeof(mid));
    printf("\nfsend: %s msg_id=%s\n", esp_err_to_name(e), mid);
}

static void cmd_peer(const char *args)
{
    char oid[64] = {0}, cid[64] = {0};
    const char *p = args;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(oid)) oid[i++] = *p++;
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(cid)) cid[i++] = *p++;
    if (!oid[0]) { printf("usage: peer <open_id> [chat_id]\n"); return; }
    app_cfg_t *c = cfg_mut();
    strlcpy(c->open_id, oid, sizeof(c->open_id));
    if (cid[0]) strlcpy(c->chat_id, cid, sizeof(c->chat_id));
    cfg_save();
    feishu_cfg_t f = {0};
    strlcpy(f.app_id, c->app_id, sizeof(f.app_id));
    strlcpy(f.app_secret, c->app_secret, sizeof(f.app_secret));
    strlcpy(f.open_id, c->open_id, sizeof(f.open_id));
    strlcpy(f.chat_id, c->chat_id, sizeof(f.chat_id));
    feishu_init(&f);
    printf("\npeer saved\n");
}

static void cmd_wifi(const char *args)
{
    char ssid[64] = {0}, pass[64] = {0};
    const char *p = args;
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(ssid)) ssid[i++] = *p++;
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i + 1 < sizeof(pass)) pass[i++] = *p++;
    if (!ssid[0]) { printf("usage: wifi <ssid> <pass>\n"); return; }
    app_cfg_t *c = cfg_mut();
    strlcpy(c->wifi_ssid, ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, pass, sizeof(c->wifi_pass));
    cfg_save();
    wifi_reconfigure(c);
    printf("\nwifi saved: %s\n", ssid);
}

static void cmd_net(void)
{
    const app_cfg_t *c = cfg_get();
    char ssid[40] = "", ip[20] = "";
    int rssi = 0;
    wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    printf("\n--- net ---\n");
    printf("ssid   : %s (%d dBm)\n", ssid[0] ? ssid : c->wifi_ssid, rssi);
    printf("ip     : %s\n", ip);
    printf("time   : %s\n", time_synced() ? "synced" : "not synced");
    printf("feishu : %s, ws=%s\n", feishu_configured() ? "configured" : "not configured",
           feishu_ws_connected() ? "online" : "offline");
    printf("peer   : %s\n", c->open_id[0] ? "configured" : "(none)");
    printf("-----------\n");
}

static void cmd_help(void)
{
    printf("\n"
           "info            设备信息\n"
           "power           电源策略及当前时钟\n"
           "m1 <AT>         向模组1发 AT\n"
           "m2 <AT>         向模组2发 AT\n"
           "p1 / p2 / pa    探活序列（pa=两张卡）\n"
           "wifi <ssid> <pass>        设置 Wi-Fi\n"
           "fset <app_id> <app_secret> 设置飞书应用\n"
           "r1 / r2         硬件复位模组\n"
           "sms1 <num> <txt> 用模组1发短信（后续版本）\n"
           "\n");
}

static void handle_line(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        cmd_help();
    } else if (!strcmp(line, "info")) {
        cmd_info();
    } else if (!strcmp(line, "power")) {
        char status[96];
        power_policy_status(status, sizeof(status));
        printf("%s\n", status);
#if CONFIG_PM_PROFILING
        esp_err_t err = esp_pm_dump_locks(stdout);
        if (err != ESP_OK) printf("PM profile unavailable: %s\n", esp_err_to_name(err));
#else
        printf("PM profiling disabled\n");
#endif
    } else if (!strncmp(line, "m1 ", 3)) {
        run_at(&s_modems[0], line + 3, 5000);
    } else if (!strncmp(line, "m2 ", 3)) {
        run_at(&s_modems[1], line + 3, 5000);
    } else if (!strcmp(line, "p1")) {
        cmd_probe(&s_modems[0]);
    } else if (!strcmp(line, "p2")) {
        cmd_probe(&s_modems[1]);
    } else if (!strcmp(line, "pa")) {
        cmd_probe(&s_modems[0]);
        cmd_probe(&s_modems[1]);
    } else if (!strcmp(line, "smsinit")) {
        cmd_smsinit();
    } else if (!strncmp(line, "sendt ", 6)) {
        cmd_sendt(line + 6);
    } else if (!strncmp(line, "send ", 5)) {
        cmd_send(line + 5);
    } else if (!strncmp(line, "peer ", 5)) {
        cmd_peer(line + 5);
    } else if (!strncmp(line, "wifi ", 5)) {
        cmd_wifi(line + 5);
    } else if (!strcmp(line, "net")) {
        cmd_net();
    } else if (!strcmp(line, "save")) {
        cfg_save();
        printf("\nsaved\n");
    } else if (!strcmp(line, "reboot")) {
        if (!firmware_restart_reserve()) {
            printf("\nfirmware operation in progress\n");
            return;
        }
        printf("\nrebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(line, "cmdtest")) {
        cmd_cmdtest();
    } else if (!strncmp(line, "fset ", 5)) {
        cmd_fset(line + 5);
    } else if (!strncmp(line, "fsend ", 6)) {
        cmd_fsend(line + 6);
    } else if (!strncmp(line, "fcfg ", 5)) {
        cmd_fset(line + 5);
    } else if (!strcmp(line, "wsstart")) {
        esp_err_t e = feishu_ws_start();
        printf("\nwsstart: %s\n", esp_err_to_name(e));
    } else if (!strcmp(line, "wsstop")) {
        feishu_ws_stop();
        printf("\nws stopped\n");
    } else if (!strcmp(line, "pbbptest")) {
        cmd_pbbptest();
    } else if (!strcmp(line, "pdutest")) {
        cmd_pdutest();
    } else if (!strncmp(line, "pdudec ", 7)) {
        cmd_pdudec(line + 7);
    } else if (!strncmp(line, "pdusub ", 7)) {
        cmd_pdusub(line + 7);
    } else if (!strcmp(line, "r1")) {
        modem_reset_hw(&s_modems[0]);
    } else if (!strcmp(line, "r2")) {
        modem_reset_hw(&s_modems[1]);
    } else {
        printf("?\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[256];
    size_t len = 0;
    uint8_t buf[64];

    printf("> ");
    fflush(stdout);

    while (1) {
        /* read_bytes waits for any queued USB data (not for the full buffer),
         * so block indefinitely instead of polling the console ten times a
         * second while the user is idle. */
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                len = 0;
                printf("\n");
                handle_line(line);
                printf("> ");
                fflush(stdout);
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }
    }
}

void console_start(modem_t *modems)
{
    s_modems = modems;
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 2048,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
    }
    xTaskCreate(console_task, "console", 16384, NULL, 5, NULL);
}
