#include "../cmd_policy.h"

#include <stdio.h>
#include <string.h>

static void expected_time(char *out, size_t cap, time_t ts)
{
    struct tm tmv;
    localtime_r(&ts, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

int main(void)
{
    char text[384], expected[384], timebuf[32];
    const time_t ts = 1763900000;
    expected_time(timebuf, sizeof(timebuf), ts);

    int n = cmd_policy_format_message_notice(text, sizeof(text), "短信",
        "+8613900139000", "+8613800138000", ts, true, "第一行\n\n 第二行 ");
    snprintf(expected, sizeof(expected),
        "【收到来自13900139000的短信】\n接收号码：13800138000\n接收时间：%s\n\n第一行\n\n 第二行 ",
        timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 1;

    n = cmd_policy_format_message_notice(text, sizeof(text), "彩信",
        "+12025550123", "8613800138000", ts, true, NULL);
    snprintf(expected, sizeof(expected),
        "【收到来自+12025550123的彩信】\n接收号码：13800138000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0 ||
        strstr(text, "\n\n") != NULL) return 2;

    n = cmd_policy_format_call_notice(text, sizeof(text), 0, "个人卡",
        "13800138000", "10086", ts, true);
    snprintf(expected, sizeof(expected),
        "【接到来自10086的来电】\n接收号码：13800138000\n接收时间：%s", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0 ||
        strstr(text, "卡槽") || strstr(text, "挂断") || strstr(text, "响铃")) return 3;

    n = cmd_policy_format_call_notice(text, sizeof(text), 1, "工作卡",
        "未知", NULL, ts, false);
    snprintf(expected, sizeof(expected),
        "【接到来自未知的来电】\n接收号码：未知\n接收时间：%s?", timebuf);
    if (n <= 0 || (size_t)n >= sizeof(text) || strcmp(text, expected) != 0) return 4;

    puts("PASS exact SMS/MMS/call notice headings, lines, normalization, body, and unknowns");
    return 0;
}
