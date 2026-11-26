#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "phone_origin.h"

static inline bool cmd_policy_dial_number(const char *raw, char *dial, size_t dial_len,
                                          char *display, size_t display_len)
{
    if (!raw || !dial || !dial_len || !display || !display_len) return false;
    bool international = raw[0] == '+';
    const char *p = international ? raw + 1 : raw;
    char digits[24];
    size_t n = 0;
    while (*p && n + 1 < sizeof(digits)) {
        if (!isdigit((unsigned char)*p)) return false;
        digits[n++] = *p++;
    }
    digits[n] = '\0';
    if (*p || n < 7 || n > 15) return false;
    if (n == 11 && !international) {
        snprintf(dial, dial_len, "+86%s", digits);
        snprintf(display, display_len, "%s", digits);
    } else if (n == 13 && strncmp(digits, "86", 2) == 0) {
        snprintf(dial, dial_len, "+%s", digits);
        snprintf(display, display_len, "%s", digits + 2);
    } else {
        snprintf(dial, dial_len, "%s%s", international ? "+" : "", digits);
        snprintf(display, display_len, "%s%s", international ? "+" : "", digits);
    }
    return true;
}

/* Return domestic SIM identity without country prefix. No partial aliases. */
static inline bool cmd_policy_local_number(const char *raw, char *key, size_t key_len)
{
    if(!raw || !key || key_len<12) return false;
    bool explicit_plus=raw[0]=='+';
    const char *p=raw;
    if(*p=='+') p++;
    bool china_prefix=strlen(p)==13 && strncmp(p,"86",2)==0;
    if(explicit_plus && !china_prefix) return false;
    if(china_prefix) p+=2;
    if(strlen(p)!=11 || p[0]!='1') return false;
    for(size_t i=0;i<11;i++) if(!isdigit((unsigned char)p[i])) return false;
    memcpy(key,p,11); key[11]='\0';
    return true;
}

/* -1 invalid/unmatched; -2 ambiguous; otherwise zero-based SIM slot. */
static inline int cmd_policy_find_local_slot(const char *number,
                                             const char *slot0_number,
                                             const char *slot1_number)
{
    char key[12], a[12], b[12];
    if(!cmd_policy_local_number(number,key,sizeof(key))) return -1;
    bool m0=cmd_policy_local_number(slot0_number,a,sizeof(a)) && strcmp(a,key)==0;
    bool m1=cmd_policy_local_number(slot1_number,b,sizeof(b)) && strcmp(b,key)==0;
    if(m0 && m1) return -2;
    return m0?0:(m1?1:-1);
}

/* Parse a CCFC query without treating a missing/malformed response as disabled.
 * The returned line must explicitly include the voice bearer class. */
static inline bool cmd_policy_ccfc_voice(const char *response, int wanted_status,
                                         const char *expected_number)
{
    if (!response || (wanted_status != 0 && wanted_status != 1)) return false;
    const char *p=response;
    while ((p=strstr(p,"+CCFC:")) != NULL) {
        p+=6;
        while (*p==' ' || *p=='\t') p++;
        if (!isdigit((unsigned char)*p)) continue;
        int status=*p++-'0';
        if (*p++!=',') continue;
        while (*p==' ') p++;
        if (!isdigit((unsigned char)*p)) continue;
        int service_class=0;
        while (isdigit((unsigned char)*p)) {
            if (service_class<10000) service_class=service_class*10+(*p-'0');
            p++;
        }
        if (status!=wanted_status || (service_class&1)==0) continue;
        if (!expected_number) return true;
        const char *q=strchr(p,'"');
        if (!q) continue;
        const char *end=strchr(++q,'"');
        if (!end) continue;
        char got[24];
        size_t n=(size_t)(end-q);
        if (n>=sizeof(got)) continue;
        memcpy(got,q,n); got[n]='\0';
        const char *a=got, *b=expected_number;
        if (*a=='+') a++;
        if (*b=='+') b++;
        if (strcmp(a,b)==0) return true;
        /* Treat national and +86 representations as the same address. */
        if (strlen(a)==13 && strncmp(a,"86",2)==0) a+=2;
        if (strlen(b)==13 && strncmp(b,"86",2)==0) b+=2;
        if (strcmp(a,b)==0) return true;
    }
    return false;
}

static inline int cmd_policy_cme_code(const char *response)
{
    if (!response) return -1;
    const char *p=strstr(response,"+CME ERROR:");
    if (!p) p=strstr(response,"+CMS ERROR:");
    if (!p) return -1;
    p=strchr(p,':')+1;
    while (*p==' ' || *p=='\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    int value=0, digits=0;
    while (isdigit((unsigned char)*p) && digits<4) { value=value*10+(*p++-'0'); digits++; }
    if (isdigit((unsigned char)*p)) return -1;
    return value;
}

/* +CEREG query replies normally use <n>,<stat>[,...]. Some modems return
 * only <stat>; read that as a fallback, and never mistake LAC/CI for stat. */
static inline int cmd_policy_cereg_stat(const char *response)
{
    if (!response) return -1;
    const char *p=strstr(response,"+CEREG:");
    if (!p) return -1;
    p+=7;
    while (*p==' ' || *p=='\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) p++;
    while (*p==' ' || *p=='\t') p++;
    if (*p==',') {
        p++;
        while (*p==' ' || *p=='\t') p++;
        if (!isdigit((unsigned char)*p)) return -1;
    } else {
        p=strstr(response,"+CEREG:")+7;
        while (*p==' ' || *p=='\t') p++;
    }
    int stat=0;
    while (isdigit((unsigned char)*p)) {
        if (stat>100) return -1;
        stat=stat*10+(*p++-'0');
    }
    if (*p!='\0' && *p!='\r' && *p!='\n' && *p!=',' && *p!=' ' && *p!='\t') return -1;
    return stat;
}

static inline void cmd_policy_display_phone(const char *raw, char *out, size_t outlen)
{
    if (!out || !outlen) return;
    const char *unknown="未知";
    if (!raw || !raw[0] || strcmp(raw,unknown)==0) {
        snprintf(out,outlen,"%s",unknown);
        return;
    }
    const char *digits=raw;
    bool plus=*digits=='+';
    if (plus) digits++;
    size_t n=strlen(digits);
    bool numeric=n>0;
    for (size_t i=0;i<n;i++) if (!isdigit((unsigned char)digits[i])) numeric=false;
    if (numeric && n==13 && strncmp(digits,"86",2)==0 && digits[2]=='1')
        snprintf(out,outlen,"%s",digits+2);
    else if (numeric && !plus && n==11 && digits[0]=='1')
        snprintf(out,outlen,"%s",digits);
    else
        snprintf(out,outlen,"%s",raw);
}

/* region_peer is the original phone address, or NULL if the origin is unknown. */
static inline int cmd_policy_format_message_notice(char *out, size_t outlen,
                                                    const char *kind,
                                                    const char *peer,
                                                    const char *region_peer,
                                                    const char *local_number,
                                                    time_t ts, bool time_synced,
                                                    const char *body)
{
    if (!out || !outlen || !kind) return -1;
    char peer_text[48], local_text[48], timebuf[32];
    cmd_policy_display_phone(peer,peer_text,sizeof(peer_text));
    cmd_policy_display_phone(local_number,local_text,sizeof(local_text));
    const char *region = phone_origin_label(region_peer);
    struct tm tmv;
    if (!localtime_r(&ts,&tmv) || !strftime(timebuf,sizeof(timebuf),"%Y-%m-%d %H:%M:%S",&tmv)) return -1;
    return snprintf(out,outlen,"【收到来自%s%s%s%s的%s】\n接收号码：%s\n接收时间：%s%s%s%s",
                    peer_text,region?"（":"",region?region:"",region?"）":"",
                    kind,local_text,timebuf,time_synced?"":"?",
                    body && body[0]?"\n\n":"",body?body:"");
}

/* label is the caller's origin text (address, and optionally an online mark);
 * it is computed by the caller so this formatter stays free of lookups. */
static inline int cmd_policy_format_call_notice(char *out, size_t outlen, int slot,
                                                const char *slot_name,
                                                const char *local_number,
                                                const char *caller,
                                                const char *label,
                                                time_t ring_time,
                                                bool time_synced)
{
    (void)slot_name;
    if (!out || !outlen || slot<0 || slot>1) return -1;
    char caller_text[48], local_text[48], timebuf[32];
    cmd_policy_display_phone(caller,caller_text,sizeof(caller_text));
    cmd_policy_display_phone(local_number,local_text,sizeof(local_text));
    const char *region = (label && label[0]) ? label : NULL;
    struct tm tmv;
    if (!localtime_r(&ring_time,&tmv) || !strftime(timebuf,sizeof(timebuf),"%Y-%m-%d %H:%M:%S",&tmv)) return -1;
    return snprintf(out,outlen,"【接到来自%s%s%s%s的来电】\n接收号码：%s\n接收时间：%s%s",
                    caller_text,region?"（":"",region?region:"",region?"）":"",
                    local_text,timebuf,time_synced?"":"?");
}
enum {
    CMD_SMS_RECEIPT_SENT=0,
    CMD_SMS_RECEIPT_INVALID_BODY,
    CMD_SMS_RECEIPT_TOO_LONG,
    CMD_SMS_RECEIPT_ENCODING,
    CMD_SMS_RECEIPT_INVALID_DEST,
    CMD_SMS_RECEIPT_BUSY,
    CMD_SMS_RECEIPT_DESYNC,
    CMD_SMS_RECEIPT_PROMPT_TIMEOUT,
    CMD_SMS_RECEIPT_LOCK_TIMEOUT,
    CMD_SMS_RECEIPT_UNKNOWN,
    CMD_SMS_RECEIPT_OVERFLOW,
    CMD_SMS_RECEIPT_MODEM_REJECT,
    CMD_SMS_RECEIPT_PARTIAL,
    CMD_SMS_RECEIPT_PARTIAL_UNKNOWN,
    CMD_SMS_RECEIPT_NOT_REGISTERED,
    CMD_SMS_RECEIPT_REG_TIMEOUT,
    CMD_SMS_RECEIPT_REG_ERROR,
    CMD_SMS_RECEIPT_REG_PARSE,
};

static inline int cmd_policy_sms_receipt(char *out,size_t outlen,const char *local,
                                         const char *destination,int result,
                                         int modem_code,int cme_code,
                                         int submitted,int total)
{
    if(!out || !outlen) return -1;
    const char *id=local?local:"本机号码未知";
    switch(result) {
    case CMD_SMS_RECEIPT_SENT:
        return snprintf(out,outlen,"✓ %s：短信已提交至 %s",id,destination?destination:"目标号码");
    case CMD_SMS_RECEIPT_INVALID_BODY:
        return snprintf(out,outlen,"✗ %s：正文无效，未发送",id);
    case CMD_SMS_RECEIPT_TOO_LONG:
        return snprintf(out,outlen,"✗ %s：正文过长，未发送",id);
    case CMD_SMS_RECEIPT_ENCODING:
        return snprintf(out,outlen,"✗ %s：短信编码异常，未发送",id);
    case CMD_SMS_RECEIPT_INVALID_DEST:
        return snprintf(out,outlen,"✗ %s：目标号码格式无效，未发送",id);
    case CMD_SMS_RECEIPT_BUSY:
        return snprintf(out,outlen,"✗ %s：模组忙，未发送（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_DESYNC:
        return snprintf(out,outlen,"✗ %s：模组状态待恢复，未发送（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_PROMPT_TIMEOUT:
        return snprintf(out,outlen,"✗ %s：发送提示超时，短信正文未提交（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_LOCK_TIMEOUT:
        return snprintf(out,outlen,"✗ %s：模组占用超时，未提交短信（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_UNKNOWN:
        return snprintf(out,outlen,"✗ %s：短信提交结果不确定，请勿立即重发（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_OVERFLOW:
        return snprintf(out,outlen,"✗ %s：模组响应过长，提交结果不确定（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_MODEM_REJECT:
        if(cme_code>=0) return snprintf(out,outlen,"✗ %s：模组拒绝发送（错误码 %d）",id,cme_code);
        return snprintf(out,outlen,"✗ %s：模组拒绝发送（错误码 %d）",id,modem_code);
    case CMD_SMS_RECEIPT_PARTIAL: {
        int code=cme_code>=0?cme_code:modem_code;
        return snprintf(out,outlen,"✗ %s：仅提交 %d/%d 段，余下部分未提交（错误码 %d）；请勿整条重发",id,submitted,total,code);
    }
    case CMD_SMS_RECEIPT_PARTIAL_UNKNOWN: {
        int code=cme_code>=0?cme_code:modem_code;
        return snprintf(out,outlen,"✗ %s：已提交 %d/%d 段，下一段结果不确定（错误码 %d）；请勿立即重发",id,submitted,total,code);
    }
    case CMD_SMS_RECEIPT_NOT_REGISTERED:
        return snprintf(out,outlen,"✗ %s：未注册网络，未发送",id);
    case CMD_SMS_RECEIPT_REG_TIMEOUT:
        return snprintf(out,outlen,"✗ %s：注册状态查询超时（错误码 %d），未发送",id,modem_code);
    case CMD_SMS_RECEIPT_REG_ERROR:
        if(cme_code>=0) return snprintf(out,outlen,"✗ %s：注册状态读取失败（错误码 %d），未发送",id,cme_code);
        return snprintf(out,outlen,"✗ %s：注册状态读取失败（错误码 %d），未发送",id,modem_code);
    default:
        return snprintf(out,outlen,"✗ %s：注册状态解析异常，未发送",id);
    }
}
