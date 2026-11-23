#include "../cmd_policy.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    if (cmd_policy_find_local_slot("13800138000","+8613800138000","13900139000")!=0) return 1;
    if (cmd_policy_find_local_slot("+8613900139000","13800138000","13900139000")!=1) return 2;
    if (cmd_policy_find_local_slot("8613800138000","13800138000","13900139000")!=0) return 3;
    if (cmd_policy_find_local_slot("3800138000","13800138000","13900139000")!=-1) return 4;
    if (cmd_policy_find_local_slot("13800000000","13800138000","13900139000")!=-1) return 5;
    if (cmd_policy_find_local_slot("卡一","13800138000","13900139000")!=-1) return 6;
    if (cmd_policy_find_local_slot("+8613800138000","13800138000","13800138000")!=-2) return 7;
    if (cmd_policy_find_local_slot("+12025550123","13800138000","13900139000")!=-1) return 16;

    char dial[40], display[40], at[96];
    if (!cmd_policy_dial_number("13800138000", dial, sizeof(dial), display, sizeof(display)) ||
        strcmp(dial, "+8613800138000") != 0 || strcmp(display, "13800138000") != 0) return 8;
    snprintf(at, sizeof(at), "AT+CCFC=0,3,\"%s\",145,1", dial);
    if (strcmp(at, "AT+CCFC=0,3,\"+8613800138000\",145,1") != 0) return 9;
    if (!cmd_policy_dial_number("+8613800138000", dial, sizeof(dial), display, sizeof(display)) ||
        strcmp(dial, "+8613800138000") != 0 || strcmp(display, "13800138000") != 0) return 10;
    if (!cmd_policy_dial_number("+12025550123", dial, sizeof(dial), display, sizeof(display)) ||
        strcmp(dial, "+12025550123") != 0) return 11;
    char receipt[192];
    if(cmd_policy_sms_receipt(receipt,sizeof(receipt),"本机号 13800138000（个人卡）",
       "+12025550123",CMD_SMS_RECEIPT_SENT,0,-1,0,1)<0 ||
       !strstr(receipt,"本机号 13800138000") || !strstr(receipt,"+12025550123")) return 12;
    if(cmd_policy_sms_receipt(receipt,sizeof(receipt),"本机号 13800138000（个人卡）",
       "+12025550123",CMD_SMS_RECEIPT_UNKNOWN,0x7104,-1,0,1)<0 ||
       !strstr(receipt,"结果不确定") || !strstr(receipt,"错误码") ||
       strstr(receipt,"+12025550123")) return 13;
    if(cmd_policy_sms_receipt(receipt,sizeof(receipt),"本机号 13800138000（个人卡）",
       "+12025550123",CMD_SMS_RECEIPT_PARTIAL,-1,30,1,2)<0 ||
       !strstr(receipt,"1/2 段") || !strstr(receipt,"错误码 30") ||
       strstr(receipt,"+12025550123")) return 14;
    if(cmd_policy_sms_receipt(receipt,sizeof(receipt),"本机号 13800138000（个人卡）",
       "+12025550123",CMD_SMS_RECEIPT_PARTIAL_UNKNOWN,0x7104,-1,1,2)<0 ||
       !strstr(receipt,"下一段结果不确定") || !strstr(receipt,"请勿立即重发")) return 15;
    if (!cmd_policy_dial_number("13800138000", dial, sizeof(dial), display, sizeof(display))) return 14;
    if (!cmd_policy_ccfc_voice("+CCFC: 1,1,\"+8613800138000\",145",1,dial)) return 8;
    if (cmd_policy_ccfc_voice("+CCFC: 1,1,\"+8613999999999\",145",1,dial)) return 9;
    if (!cmd_policy_ccfc_voice("+CCFC: 0,1",0,NULL)) return 10;
    if (cmd_policy_ccfc_voice("OK",0,NULL)) return 11;
    if (cmd_policy_ccfc_voice("+CCFC: 1,2,\"+8613800138000\",145",1,dial)) return 12;
    if (cmd_policy_cme_code("+CME ERROR: 30") != 30 ||
        cmd_policy_cme_code("+CMS ERROR: 515") != 515 ||
        cmd_policy_cme_code("+CME ERROR: operation not allowed") >= 0) return 13;
    puts("PASS Feishu command parsing and dial target");
    return 0;
}
