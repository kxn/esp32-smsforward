#include "../cmd_policy.h"
#include "../phone_region.h"
#include "../phone_origin.h"
#include "../peer_mark_core.h"

#include <stdio.h>
#include <string.h>

static void expected_time(char *out, size_t cap, time_t ts)
{
    struct tm tmv;
    localtime_r(&ts, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static int format_call(char *text, size_t cap, int slot, const char *slot_name,
                       const char *local, const char *caller, const char *region,
                       const char *mark, time_t ts, bool synced)
{
    char label[96];
    peer_mark_join_label(label, sizeof(label), region, mark);
    return cmd_policy_format_call_notice(text, cap, slot, slot_name, local, caller,
                                         label[0] ? label : NULL, ts, synced);
}

int main(void)
{
    char text[384], expected[384], timebuf[32];
    const time_t ts = 1763900000;
    expected_time(timebuf, sizeof(timebuf), ts);

    const char *mobile = "+8613800138000";
    const char *region = phone_region_lookup("13800138000");
    if (!region) return 1;
    int n = cmd_policy_format_message_notice(text, sizeof(text), "短信",
        mobile, mobile, "+8613900139000", ts, true, "第一行\n\n 第二行 ");
    snprintf(expected, sizeof(expected),
        "【收到来自13800138000（%s）的短信】\n接收号码：13900139000\n接收时间：%s\n\n第一行\n\n 第二行 ",
        region, timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 1;

    n = cmd_policy_format_message_notice(text, sizeof(text), "彩信",
        "+12025550123", "+12025550123", "8613800138000", ts, true, NULL);
    snprintf(expected, sizeof(expected),
        "【收到来自+12025550123（美国/加拿大）的彩信】\n接收号码：13800138000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0 ||
        strstr(text, "\n\n") != NULL) return 2;

    n = format_call(text, sizeof(text), 0, "个人卡",
        "13800138000", "10086", phone_origin_label("10086"), NULL, ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自10086（中国移动）的来电】\n接收号码：13800138000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0 ||
        strstr(text, "卡槽") || strstr(text, "挂断") || strstr(text, "响铃")) return 3;

    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "01021285387", phone_origin_label("01021285387"), NULL, ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自01021285387（北京）的来电】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 9;

    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "95555", phone_origin_label("95555"), NULL, ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自95555（招商银行）的来电】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 10;

    n = format_call(text, sizeof(text), 1, "工作卡",
        "未知", NULL, NULL, NULL, ts, false);
    snprintf(expected, sizeof(expected),
        "【接到来自未知的来电】\n接收号码：未知\n接收时间：%s?", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 4;

    n = cmd_policy_format_message_notice(text, sizeof(text), "彩信",
        mobile, "13800138000", "13900139000", ts, true, NULL);
    snprintf(expected, sizeof(expected),
        "【收到来自13800138000（%s）的彩信】\n接收号码：13900139000\n接收时间：%s", region, timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 5;

    /* With no trustworthy MMS From, the displayed transport sender has no region. */
    n = cmd_policy_format_message_notice(text, sizeof(text), "彩信",
        mobile, NULL, "13900139000", ts, true, NULL);
    snprintf(expected, sizeof(expected),
        "【收到来自13800138000的彩信】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 6;

    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "8613800138000", phone_origin_label("8613800138000"), NULL, ts, false);
    snprintf(expected, sizeof(expected),
        "【接到来自13800138000（%s）的来电】\n接收号码：13900139000\n接收时间：%s?", region, timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 7;

    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "13800138000x", phone_origin_label("13800138000x"), NULL, ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自13800138000x的来电】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 8;

    /* A network mark is appended after the address label. */
    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "13800138000", "北京", "快递送餐", ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自13800138000（北京 · 快递送餐）的来电】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 11;

    /* A mark on a number the local tables cannot place stands alone. */
    n = format_call(text, sizeof(text), 0, "个人卡",
        "13900139000", "13800138000", NULL, "骚扰电话", ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自13800138000（骚扰电话）的来电】\n接收号码：13900139000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 12;

    puts("PASS exact SMS/MMS/call notice format, regions, marks, and fallback");
    return 0;
}
