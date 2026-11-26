#include "../firmware/main/phone_origin.h"
#include "../firmware/main/phone_region.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_kind(const char *number, phone_origin_kind_t want)
{
    const char *label = (const char *)0x1;
    phone_origin_kind_t got = phone_origin_lookup(number, &label);
    if (got != want) {
        printf("[FAIL] %s: kind %d, want %d\n", number ? number : "(null)", (int)got, (int)want);
        ++failures;
    } else {
        printf("[PASS] %s: kind %d\n", number ? number : "(null)", (int)got);
    }
}

static void expect_label(const char *number, phone_origin_kind_t want, const char *want_label)
{
    const char *label = NULL;
    phone_origin_kind_t got = phone_origin_lookup(number, &label);
    bool ok = got == want;
    if (want_label) {
        ok = ok && label && strcmp(label, want_label) == 0;
    } else {
        ok = ok && label == NULL;
    }
    if (!ok) {
        printf("[FAIL] %s: kind %d label %s, want kind %d label %s\n", number, (int)got,
               label ? label : "(null)", (int)want, want_label ? want_label : "(null)");
        ++failures;
    } else {
        printf("[PASS] %s: %d %s\n", number, (int)got, label ? label : "(null)");
    }
}

int main(void)
{
    /* Mainland landlines: three-digit and four-digit area codes. */
    expect_label("01021285387", PHONE_ORIGIN_LANDLINE, "北京");
    expect_label("010-21285387", PHONE_ORIGIN_LANDLINE, "北京");
    expect_label("(010) 2128 5387", PHONE_ORIGIN_LANDLINE, "北京");
    expect_label("02112345678", PHONE_ORIGIN_LANDLINE, "上海");
    expect_label("02088888888", PHONE_ORIGIN_LANDLINE, "广州");
    expect_label("075588887777", PHONE_ORIGIN_LANDLINE, "深圳");
    expect_label("057188887777", PHONE_ORIGIN_LANDLINE, "杭州");
    expect_label("089888888888", PHONE_ORIGIN_LANDLINE, "海南");
    expect_label("02888888888", PHONE_ORIGIN_LANDLINE, "成都");
    /* Unknown area code still counts as a landline, just without a label. */
    expect_label("07001234567", PHONE_ORIGIN_LANDLINE, NULL);
    expect_label("09999999999", PHONE_ORIGIN_LANDLINE, "伊犁");

    /* Mainland mobiles keep the prefix table's answer. */
    const char *mobile_region = phone_region_lookup("13800138000");
    if (!mobile_region) {
        printf("[FAIL] 13800138000 has no prefix region\n");
        ++failures;
    }
    expect_label("13800138000", PHONE_ORIGIN_MOBILE, mobile_region);
    expect_label("+8613800138000", PHONE_ORIGIN_MOBILE, mobile_region);
    expect_label("008613800138000", PHONE_ORIGIN_MOBILE, mobile_region);
    expect_label("8613800138000", PHONE_ORIGIN_MOBILE, mobile_region);

    /* Service numbers. */
    expect_label("95555", PHONE_ORIGIN_SERVICE, "招商银行");
    expect_label("95588", PHONE_ORIGIN_SERVICE, "中国工商银行");
    expect_label("10086", PHONE_ORIGIN_SERVICE, "中国移动");
    expect_label("10010", PHONE_ORIGIN_SERVICE, "中国联通");
    expect_label("10000", PHONE_ORIGIN_SERVICE, "中国电信");
    expect_label("110", PHONE_ORIGIN_SERVICE, "公安报警");
    expect_label("119", PHONE_ORIGIN_SERVICE, "火警");
    expect_label("120", PHONE_ORIGIN_SERVICE, "急救");
    expect_label("12345", PHONE_ORIGIN_SERVICE, "政务服务便民热线");
    expect_label("12306", PHONE_ORIGIN_SERVICE, "铁路12306");
    expect_label("95598", PHONE_ORIGIN_SERVICE, "国家电网");
    expect_label("4001234567", PHONE_ORIGIN_SERVICE, "企业客服");
    expect_label("8001234567", PHONE_ORIGIN_SERVICE, "企业客服");
    expect_label("40012345", PHONE_ORIGIN_UNKNOWN, NULL);

    /* International numbers, in all three shapes the network can send. */
    expect_label("+14155551234", PHONE_ORIGIN_ABROAD, "美国/加拿大");
    expect_label("+12025550123", PHONE_ORIGIN_ABROAD, "美国/加拿大");
    expect_label("0014155551234", PHONE_ORIGIN_ABROAD, "美国/加拿大");
    expect_label("+442071838750", PHONE_ORIGIN_ABROAD, "英国");
    expect_label("+85221234567", PHONE_ORIGIN_ABROAD, "香港");
    expect_label("+886212345678", PHONE_ORIGIN_ABROAD, "台湾");
    expect_label("+81312345678", PHONE_ORIGIN_ABROAD, "日本");
    expect_label("+74955555555", PHONE_ORIGIN_ABROAD, "俄罗斯/哈萨克斯坦");
    expect_label("+4955555555", PHONE_ORIGIN_ABROAD, "德国");
    expect_label("+80012345678", PHONE_ORIGIN_ABROAD, "国际免费电话");
    expect_label("+99912345678", PHONE_ORIGIN_ABROAD, NULL);

    /* Anything that is not a number we understand stays unknown. */
    expect_kind(NULL, PHONE_ORIGIN_UNKNOWN);
    expect_kind("", PHONE_ORIGIN_UNKNOWN);
    expect_kind("未知", PHONE_ORIGIN_UNKNOWN);
    expect_kind("21285387", PHONE_ORIGIN_UNKNOWN);
    expect_kind("12ab3456", PHONE_ORIGIN_UNKNOWN);
    expect_kind("+", PHONE_ORIGIN_UNKNOWN);
    expect_kind("13800138000123456789", PHONE_ORIGIN_UNKNOWN);

    /* The label helper agrees with the structured lookup. */
    const char *label = phone_origin_label("95555");
    if (!label || strcmp(label, "招商银行") != 0) {
        printf("[FAIL] phone_origin_label(95555) = %s\n", label ? label : "(null)");
        ++failures;
    }
    if (phone_origin_label("21285387") != NULL) {
        printf("[FAIL] phone_origin_label on a local number should be NULL\n");
        ++failures;
    }

    printf("[SUMMARY] %d failed\n", failures);
    if (failures == 0) printf("[PASS] Phone origin tests\n");
    return failures == 0 ? 0 : 1;
}
