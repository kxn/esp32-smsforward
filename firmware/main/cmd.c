#include "cmd.h"
#include "sms.h"
#include "modem.h"
#include "feishu.h"
#include "cmd_policy.h"
#include "lan_web.h"
#include "map_store.h"
#include "modem_timing.h"
#include "sim_identity_policy.h"
#include "sms_text.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "cmd";

static modem_t *s_modems;
static char s_slot_num[2][32];
static char s_slot_name[2][24] = { "卡1", "卡2" };

static SemaphoreHandle_t s_map_lock;

/* ---------------- 号码归一化 ---------------- */

static int slot_registration(int slot, char *response, size_t response_len,
                             esp_err_t *error_out)
{
    if(error_out) *error_out=ESP_OK;
    if (!s_modems || slot<0 || slot>=2) return -1;
    char local_resp[128]={0};
    char *resp=(response && response_len)?response:local_resp;
    size_t resp_len=(response && response_len)?response_len:sizeof(local_resp);
    esp_err_t err=modem_cmd(&s_modems[slot],"AT+CEREG?",resp,resp_len,AT_REGISTRATION_MS);
    if(error_out) *error_out=err;
    if (err!=ESP_OK) return -1;
    int stat=cmd_policy_cereg_stat(resp);
    if (stat<0) return -2;
    return (stat==1 || stat==5) ? 1 : 0;
}


/* ---------------- 映射表 ---------------- */

void map_note(const char *msg_id, int slot, const char *peer)
{
    map_note_record(msg_id, slot, peer, "");
}

void map_note_record(const char *msg_id, int slot, const char *peer, const char *record_id)
{
    (void)map_note_persisted(msg_id,slot,peer,record_id);
}

static bool parse_iccid(const char *response, char out[24])
{
    if (!response || !out) return false;
    const char *p=strchr(response, ':');
    if (p) p++; else p=response;
    size_t n=0;
    while (*p && !isdigit((unsigned char)*p)) p++;
    while (isdigit((unsigned char)*p) && n+1<24) out[n++]=*p++;
    out[n]='\0';
    return n>=10;
}

bool cmd_current_iccid(int slot, char out[24])
{
    char resp[96] = {0};
    if (!s_modems || slot < 0 || slot >= 2 ||
        modem_cmd(&s_modems[slot], "AT+ICCID", resp, sizeof(resp), AT_SIM_IDENTITY_MS) != ESP_OK)
        return false;
    return parse_iccid(resp,out);
}

bool map_note_persisted(const char *msg_id, int slot, const char *peer, const char *record_id)
{
    if (!msg_id || !msg_id[0] || slot < 0 || slot >= 2 || !peer || !peer[0] || !s_map_lock) return false;
    char iccid[24] = {0};
    /* Do not create a legacy slot-only mapping when the SIM query is busy/fails.
       The SMS delivery gate will retain and retry this record instead. */
    if (!cmd_current_iccid(slot,iccid)) return false;
    xSemaphoreTake(s_map_lock, portMAX_DELAY);
    bool ok=map_store_note_sim(msg_id,slot,peer,record_id,iccid);
    xSemaphoreGive(s_map_lock);
    return ok;
}

bool map_find_notice(const char *record_id, char *msg_id, size_t capacity)
{
    if (!s_map_lock) return false;
    xSemaphoreTake(s_map_lock, portMAX_DELAY);
    bool found = map_store_find_message(record_id, msg_id, capacity);
    xSemaphoreGive(s_map_lock);
    return found;
}

bool map_note_child(const char *parent_id, const char *child_id)
{
    if (!s_map_lock || !child_id || !*child_id) return false;
    char iccid[24] = "", peer[32] = "";
    int slot = -1;
    xSemaphoreTake(s_map_lock, portMAX_DELAY);
    bool ok = map_store_lookup_sim(parent_id, &slot, peer, sizeof(peer), NULL, 0, iccid, sizeof(iccid)) &&
              iccid[0] && map_store_note_sim(child_id, slot, peer, child_id, iccid);
    xSemaphoreGive(s_map_lock);
    return ok;
}

bool map_lookup(const char *msg_id, int *slot, char *peer, size_t peerlen)
{
    return map_lookup_record(msg_id,slot,peer,peerlen,NULL,0);
}

bool map_lookup_record(const char *msg_id, int *slot, char *peer, size_t peerlen,
                       char *record_id, size_t record_id_len)
{
    if (!msg_id || !msg_id[0]) return false;
    if (s_map_lock) xSemaphoreTake(s_map_lock, portMAX_DELAY);
    bool found=map_store_lookup(msg_id,slot,peer,peerlen,record_id,record_id_len);
    if (s_map_lock) xSemaphoreGive(s_map_lock);
    return found;
}

bool map_lookup_sim(const char *msg_id, int *slot, char *peer, size_t peerlen,
                    char *record_id, size_t record_id_len,
                    char *iccid, size_t iccid_len)
{
    if (!msg_id || !msg_id[0]) return false;
    if (s_map_lock) xSemaphoreTake(s_map_lock, portMAX_DELAY);
    bool found=map_store_lookup_sim(msg_id,slot,peer,peerlen,record_id,record_id_len,iccid,iccid_len);
    if (s_map_lock) xSemaphoreGive(s_map_lock);
    return found;
}

/* ---------------- 格式化 ---------------- */

int fmt_incoming(char *out, size_t outlen, int slot, const char *slot_name,
                 const char *local_num, const char *peer, bool sender_is_phone,
                 time_t ts, bool time_synced,
                 const char *text)
{
    (void)slot;
    (void)slot_name;
    return cmd_policy_format_message_notice(out,outlen,"短信",peer,
            sender_is_phone ? peer : NULL,local_num,ts,time_synced,text);
}

/* ---------------- 指令 ---------------- */

static int find_slot_by_number(const char *num)
{
    return cmd_policy_find_local_slot(num,s_slot_num[0],s_slot_num[1]);
}

static esp_err_t refresh_local_numbers(void)
{
    esp_err_t first_error = ESP_OK;
    for (int slot = 0; slot < 2; ++slot) {
        char refreshed[32];
        esp_err_t e = cmd_refresh_slot_number(slot, refreshed, sizeof(refreshed));
        if (e != ESP_OK && e != ESP_ERR_NOT_FOUND && first_error == ESP_OK) first_error = e;
    }
    return first_error;
}

static bool command_token(const char **cursor, char *out, size_t outlen)
{
    if(!cursor || !*cursor || !out || outlen<2) return false;
    const char *p=*cursor;
    while(*p==' ' || *p=='\t') p++;
    if(!*p || *p=='\r' || *p=='\n') return false;
    size_t n=0;
    while(*p && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') {
        if(n+1>=outlen) return false;
        out[n++]=*p++;
    }
    out[n]='\0'; *cursor=p;
    return true;
}

static void format_local_id(int slot, char *out, size_t outlen)
{
    snprintf(out,outlen,"本机号 %s（%s）",
             s_slot_num[slot][0]?s_slot_num[slot]:"未知",
             s_slot_name[slot][0]?s_slot_name[slot]:(slot==0?"卡1":"卡2"));
}

static void reply(const char *msg_id, const char *text)
{
    if (msg_id && msg_id[0]) {
        feishu_reply_text(msg_id, text, NULL, 0);
    } else {
        feishu_send_text(text, NULL, 0);
    }
}

static int send_sms_receipt(int slot, const char *dest, const char *body, const char *msg_id)
{
    char local[72],line[224];
    format_local_id(slot,local,sizeof(local));
    char target[40],shown[40];
    if (!cmd_policy_dial_number(dest,target,sizeof(target),shown,sizeof(shown))) {
        cmd_policy_sms_receipt(line,sizeof(line),local,NULL,CMD_SMS_RECEIPT_INVALID_DEST,0,-1,0,0);
        reply(msg_id,line); return -1;
    }
    int units=sms_text_ucs2_units(body?body:"");
    int segments=sms_text_segment_count(body?body:"");
    if(units<0 || segments<0) {
        cmd_policy_sms_receipt(line,sizeof(line),local,NULL,CMD_SMS_RECEIPT_ENCODING,0,-1,0,0);
        reply(msg_id,line); return -1;
    }
    if(units==0) {
        cmd_policy_sms_receipt(line,sizeof(line),local,NULL,CMD_SMS_RECEIPT_INVALID_BODY,0,-1,0,0);
        reply(msg_id,line); return -1;
    }
    if(segments<1 || segments>8) {
        cmd_policy_sms_receipt(line,sizeof(line),local,NULL,CMD_SMS_RECEIPT_TOO_LONG,0,-1,0,0);
        reply(msg_id,line); return -2;
    }
    char resp[128]={0}; esp_err_t reg_error=ESP_OK;
    int registration=slot_registration(slot,resp,sizeof(resp),&reg_error);
    if(registration<=0) {
        int kind=registration==0?CMD_SMS_RECEIPT_NOT_REGISTERED:
                 registration==-2?CMD_SMS_RECEIPT_REG_PARSE:
                 reg_error==ESP_ERR_TIMEOUT?CMD_SMS_RECEIPT_REG_TIMEOUT:CMD_SMS_RECEIPT_REG_ERROR;
        cmd_policy_sms_receipt(line,sizeof(line),local,NULL,kind,(int)reg_error,
                               cmd_policy_cme_code(resp),0,0);
        reply(msg_id,line); return -1;
    }
    char err[96]=""; int rc=sms_send(&s_modems[slot],target,body,err,sizeof(err));
    int kind=CMD_SMS_RECEIPT_MODEM_REJECT,modem_code=0,cme=cmd_policy_cme_code(err),done=0,total=segments;
    if(rc==0) kind=CMD_SMS_RECEIPT_SENT;
    else if(rc==-1) kind=CMD_SMS_RECEIPT_INVALID_BODY;
    else if(rc==-2) kind=CMD_SMS_RECEIPT_TOO_LONG;
    else if(rc==-3) kind=CMD_SMS_RECEIPT_ENCODING;
    else if(rc==MODEM_ERR_BUSY) { kind=CMD_SMS_RECEIPT_BUSY; modem_code=rc; }
    else if(rc==MODEM_ERR_DESYNC) { kind=CMD_SMS_RECEIPT_DESYNC; modem_code=rc; }
    else if(rc==MODEM_ERR_PROMPT_TIMEOUT) { kind=CMD_SMS_RECEIPT_PROMPT_TIMEOUT; modem_code=rc; }
    else if(rc==MODEM_ERR_SUBMIT_UNKNOWN) { kind=CMD_SMS_RECEIPT_UNKNOWN; modem_code=rc; }
    else if(rc==MODEM_ERR_OVERFLOW) { kind=CMD_SMS_RECEIPT_OVERFLOW; modem_code=rc; }
    else if(rc==ESP_ERR_TIMEOUT) { kind=CMD_SMS_RECEIPT_LOCK_TIMEOUT; modem_code=rc; }
    else if(rc==SMS_SEND_ERR_MODEM || rc==SMS_SEND_ERR_PARTIAL) {
        int parsed=sscanf(err,"modem=%d;cme=%d;done=%d;total=%d",&modem_code,&cme,&done,&total);
        if(parsed<1) modem_code=ESP_FAIL;
        if(rc==SMS_SEND_ERR_PARTIAL) {
            kind=(modem_code==MODEM_ERR_SUBMIT_UNKNOWN || modem_code==MODEM_ERR_OVERFLOW) ?
                  CMD_SMS_RECEIPT_PARTIAL_UNKNOWN : CMD_SMS_RECEIPT_PARTIAL;
            if(parsed<4) done=0;
        }
    } else modem_code=rc;
    cmd_policy_sms_receipt(line,sizeof(line),local,shown,kind,modem_code,cme,done,total);
    reply(msg_id,line); return rc;
}

static void handle_forward(int slot, const char *argument, const char *msg_id)
{
    char local[72],line[192],resp[256]={0};
    format_local_id(slot,local,sizeof(local));
    esp_err_t reg_error=ESP_OK;
    int registration=slot_registration(slot,resp,sizeof(resp),&reg_error);
    if(registration<=0) {
        if(registration==0) snprintf(line,sizeof(line),"✗ %s：未注册网络",local);
        else if(registration==-2) snprintf(line,sizeof(line),"✗ %s：注册状态解析异常",local);
        else if(reg_error==ESP_ERR_TIMEOUT) snprintf(line,sizeof(line),"✗ %s：注册状态查询超时",local);
        else {
            int cme=cmd_policy_cme_code(resp);
            if(cme>=0) snprintf(line,sizeof(line),"✗ %s：模组状态查询错误（码 %d）",local,cme);
            else snprintf(line,sizeof(line),"✗ %s：模组状态查询失败",local);
        }
        reply(msg_id,line); return;
    }
    if(!argument || !argument[0]) {
        esp_err_t e=modem_cmd(&s_modems[slot],"AT+CCFC=0,2,,,1",resp,sizeof(resp),8000);
        if(e!=ESP_OK) {
            int cme=cmd_policy_cme_code(resp);
            snprintf(line,sizeof(line),"✗ %s：转移状态查询失败（错误码 %d）",local,cme>=0?cme:(int)e);
        }
        else if(cmd_policy_ccfc_voice(resp,1,NULL)) {
            char *q=strchr(resp,'"'),*end=q?strchr(q+1,'"'):NULL;
            if(q && end) { *end='\0'; snprintf(line,sizeof(line),"%s：无条件转移至 %s",local,q+1); }
            else snprintf(line,sizeof(line),"✗ %s：转移状态解析异常",local);
        } else if(cmd_policy_ccfc_voice(resp,0,NULL))
            snprintf(line,sizeof(line),"%s：无条件转移已关闭",local);
        else snprintf(line,sizeof(line),"✗ %s：转移状态解析异常",local);
        reply(msg_id,line); return;
    }
    if(strcmp(argument,"off")==0) {
        esp_err_t e=modem_cmd(&s_modems[slot],"AT+CCFC=0,0,,,1",resp,sizeof(resp),8000);
        if(e==ESP_OK) {
            resp[0]='\0';
            e=modem_cmd(&s_modems[slot],"AT+CCFC=0,2,,,1",resp,sizeof(resp),5000);
        }
        if(e==ESP_OK && cmd_policy_ccfc_voice(resp,0,NULL))
            snprintf(line,sizeof(line),"✓ %s：无条件转移已关闭",local);
        else if(e==ESP_OK) snprintf(line,sizeof(line),"✗ %s：关闭状态未确认",local);
        else {
            int cme=cmd_policy_cme_code(resp);
            snprintf(line,sizeof(line),"✗ %s：关闭转移失败（错误码 %d）",local,cme>=0?cme:(int)e);
        }
        reply(msg_id,line); return;
    }
    char target[40],shown[40];
    if(!cmd_policy_dial_number(argument,target,sizeof(target),shown,sizeof(shown))) {
        snprintf(line,sizeof(line),"✗ %s：目标号码格式无效",local); reply(msg_id,line); return;
    }
    esp_err_t pre=modem_cmd(&s_modems[slot],"AT+CCFC=0,2,,,1",resp,sizeof(resp),5000);
    bool active=pre==ESP_OK && cmd_policy_ccfc_voice(resp,1,NULL);
    bool inactive=pre==ESP_OK && cmd_policy_ccfc_voice(resp,0,NULL);
    if(pre!=ESP_OK || (!active && !inactive)) {
        snprintf(line,sizeof(line),"✗ %s：转移状态解析异常",local); reply(msg_id,line); return;
    }
    if(active && cmd_policy_ccfc_voice(resp,1,target)) {
        snprintf(line,sizeof(line),"✓ %s：无条件转移已指向 %s",local,shown); reply(msg_id,line); return;
    }
    (void)modem_cmd(&s_modems[slot],"AT+CMEE=1",resp,sizeof(resp),2000);
    char cmd[96];
    snprintf(cmd,sizeof(cmd),"AT+CCFC=0,3,\"%s\",145,1",target);
    esp_err_t e=modem_cmd(&s_modems[slot],cmd,resp,sizeof(resp),8000);
    if(e!=ESP_OK) {
        snprintf(cmd,sizeof(cmd),"AT+CCFC=0,1,\"%s\",145,1",target);
        e=modem_cmd(&s_modems[slot],cmd,resp,sizeof(resp),8000);
    }
    int cme=cmd_policy_cme_code(resp);
    resp[0]='\0';
    esp_err_t qe=modem_cmd(&s_modems[slot],"AT+CCFC=0,2,,,1",resp,sizeof(resp),5000);
    if(qe==ESP_OK && cmd_policy_ccfc_voice(resp,1,target))
        snprintf(line,sizeof(line),"✓ %s：无条件转移至 %s",local,shown);
    else if(qe!=ESP_OK) {
        int q_cme=cmd_policy_cme_code(resp);
        snprintf(line,sizeof(line),"✗ %s：转移目标查询失败（错误码 %d）",local,q_cme>=0?q_cme:(int)qe);
    } else if(e!=ESP_OK)
        snprintf(line,sizeof(line),"✗ %s：模组拒绝转移（错误码 %d）",local,cme>=0?cme:(int)e);
    else snprintf(line,sizeof(line),"✗ %s：转移目标未确认，转移状态解析异常",local);
    reply(msg_id,line);
}

static void send_help(const char *msg_id)
{
    char out[640];
    char local[12];
    const char *example = "<本机号>";
    for (int i = 0; i < MODEM_COUNT; ++i) {
        if (cmd_policy_local_number(s_slot_num[i], local, sizeof(local))) {
            example = local;
            break;
        }
    }
    snprintf(out, sizeof(out),
             "/help  查看帮助\n"
             "/web  获取本机局域网管理页链接\n"
             "/sims  查看两张卡的完整本机号\n"
             "/sms <本机号> <目标号码> <正文>  发送短信\n"
             "/forward <本机号>  查询呼转\n"
             "/forward <本机号> <目标号码>  开启呼转\n"
             "/forward <本机号> off  关闭呼转\n\n"
             "本机号可输入 11 位，或带 +86、86。引用收信通知可用原卡回复。\n"
             "例如查询呼转：/forward %s", example);
    reply(msg_id,out);
}

static void send_sims(const char *msg_id)
{
    char out[256]; size_t used=0;
    for(int i=0;i<2;i++) {
        char refreshed[32];
        esp_err_t refresh = cmd_refresh_slot_number(i, refreshed, sizeof(refreshed));
        const char *status = refresh == ESP_OK ? "" :
                             refresh == ESP_ERR_NOT_FOUND ? "（未配置本机号码）" : "（刷新失败，未使用旧号码）";
        int n=snprintf(out+used,sizeof(out)-used,"卡%d（%s）：%s%s\n",i+1,
                       s_slot_name[i][0]?s_slot_name[i]:(i==0?"卡1":"卡2"),
                       s_slot_num[i][0]?s_slot_num[i]:"未知",
                       status);
        if(n<0 || (size_t)n>=sizeof(out)-used) break;
        used+=(size_t)n;
        if (refresh != ESP_OK && refresh != ESP_ERR_NOT_FOUND && used < sizeof(out))
            used += snprintf(out + used, sizeof(out) - used, "  刷新错误码：%d\n", (int)refresh);
    }
    reply(msg_id,out);
}

void cmd_handle(const char *open_id, const char *chat_id, const char *text,
                const char *msg_id, const char *parent_id)
{
    (void)open_id; (void)chat_id;

    if (!text || !text[0]) return;

    /* 1) 引用回复：查映射表 */
    if (parent_id && parent_id[0]) {
        int slot = -1;
        char peer[32] = "", old_iccid[24] = "";
        if (map_lookup_sim(parent_id, &slot, peer, sizeof(peer), NULL, 0,
                           old_iccid, sizeof(old_iccid))) {
            if (!old_iccid[0]) {
                reply(msg_id, "引用记录没有可核实的 SIM 身份，未发送；请用 /sms 明确指定本机号和目标号码");
                return;
            }
            char current_id[2][24] = {{0}};
            const char *current[2] = {current_id[0], current_id[1]};
            bool readable[2] = {false, false};
            for (int i = 0; i < 2; ++i) {
                readable[i] = cmd_current_iccid(i, current_id[i]);
            }
            int matched_slot = -1;
            int identity_result = sim_identity_match(old_iccid, current, readable, &matched_slot);
            if (identity_result == SIM_IDENTITY_UNREADABLE) {
                reply(msg_id, "无法核实两张 SIM 的身份，引用短信未发送；请稍后重试");
                return;
            }
            if (identity_result != SIM_IDENTITY_UNIQUE) {
                reply(msg_id, identity_result == SIM_IDENTITY_AMBIGUOUS ?
                              "引用短信对应的 SIM 身份不唯一，未发送" :
                              "引用短信对应的 SIM 当前未插入设备，未发送");
                return;
            }
            send_sms_receipt(matched_slot, peer, text, msg_id);
            return;
        }
    }

    while (*text == ' ' || *text == '\r' || *text == '\n') text++;

    if(strcmp(text,"/help")==0) { send_help(msg_id); return; }
    if(strcmp(text,"/web")==0) {
        char url[128];
        if (lan_web_management_url(url, sizeof(url))) {
            char message[192];
            snprintf(message, sizeof(message), "【设备管理】\n%s", url);
            reply(msg_id, message);
        } else {
            reply(msg_id, "设备当前没有可用的局域网地址；请确认 Wi-Fi 已连接后重试 /web");
        }
        return;
    }
    if(strcmp(text,"/sims")==0) { send_sims(msg_id); return; }

    if(strncmp(text,"/sms",4)==0 && (text[4]==' ' || text[4]=='\t')) {
        const char *p=text+4;
        char local[32],dest[32];
        if(!command_token(&p,local,sizeof(local)) || !command_token(&p,dest,sizeof(dest))) {
            reply(msg_id,"格式：/sms <完整本机号码> <目标号码> <正文>"); return;
        }
        if(*p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') {
            reply(msg_id,"格式：/sms <完整本机号码> <目标号码> <正文>"); return;
        }
        if(*p=='\r' && p[1]=='\n') p+=2;
        else if(*p) p++;
        if(!*p) { reply(msg_id,"短信正文不能为空"); return; }
        esp_err_t refresh=refresh_local_numbers();
        if(refresh!=ESP_OK) {
            char line[128]; snprintf(line,sizeof(line),"本机号码刷新失败（错误码 %d），短信未发送",(int)refresh);
            reply(msg_id,line); return;
        }
        int slot=find_slot_by_number(local);
        if(slot==-2) { reply(msg_id,"本机号码配置重复，请联系管理员；可用 /sims 核对"); return; }
        if(slot<0) { reply(msg_id,"请提供 /sims 中的完整本机号码（11 位或 +86/86 前缀）"); return; }
        char checked[40],shown[40];
        if(!cmd_policy_dial_number(dest,checked,sizeof(checked),shown,sizeof(shown))) {
            char id[72]; format_local_id(slot,id,sizeof(id));
            char line[120]; snprintf(line,sizeof(line),"✗ %s：目标号码格式无效，未发送",id); reply(msg_id,line); return;
        }
        send_sms_receipt(slot,dest,p,msg_id); return;
    }

    if(strncmp(text,"/forward",8)==0 && (text[8]==' ' || text[8]=='\t')) {
        if(strchr(text,'\r') || strchr(text,'\n')) {
            reply(msg_id,"/forward 不接受多行参数，请仅发送一行命令"); return;
        }
        const char *p=text+8; char local[32],argument[32]="";
        if(!command_token(&p,local,sizeof(local))) {
            reply(msg_id,"格式：/forward <完整本机号码> [目标号码|off]"); return;
        }
        while(*p==' ' || *p=='\t') p++;
        if(*p && *p!='\r' && *p!='\n') {
            if(!command_token(&p,argument,sizeof(argument))) {
                reply(msg_id,"格式：/forward <完整本机号码> [目标号码|off]"); return;
            }
            while(*p==' ' || *p=='\t') p++;
            if(*p && *p!='\r' && *p!='\n') {
                reply(msg_id,"格式：/forward <完整本机号码> [目标号码|off]"); return;
            }
        }
        esp_err_t refresh=refresh_local_numbers();
        if(refresh!=ESP_OK) {
            char line[128]; snprintf(line,sizeof(line),"本机号码刷新失败（错误码 %d），未执行转移操作",(int)refresh);
            reply(msg_id,line); return;
        }
        int slot=find_slot_by_number(local);
        if(slot==-2) { reply(msg_id,"本机号码配置重复，请联系管理员；可用 /sims 核对"); return; }
        if(slot<0) { reply(msg_id,"请提供 /sims 中的完整本机号码（11 位或 +86/86 前缀）"); return; }
        handle_forward(slot,argument,msg_id); return;
    }

    if(text[0]=='/') reply(msg_id,"指令未识别，请发送 /help 查看用法");

    /* 未知指令：静默 */
    ESP_LOGD(TAG, "unknown command");
}

/* ---------------- 自测 ---------------- */

int cmd_selftest(char *report, size_t report_len)
{
    int used = 0, fails = 0;
    char buf[512];

    int n = fmt_incoming(buf, sizeof(buf), 0, "卡1", "+8615550000001",
                         "10690000", true, 1763900000, true, "第一行\n第二行");
    bool ok = strstr(buf, "【收到来自10690000的短信】") &&
              strstr(buf, "接收号码：15550000001") && strstr(buf, "接收时间：") &&
              strstr(buf, "第一行\n第二行");
    if (!ok) fails++;
    int k = snprintf(report + used, report_len - used, "[%s] 收信格式\n", ok ? "PASS" : "FAIL");
    used += k > 0 ? k : 0;

    /* Do not mutate the persistent production map from self-test. */

    char key[12], dial[40], shown[40];
    ok=cmd_policy_local_number("+8615550000001",key,sizeof(key)) && strcmp(key,"15550000001")==0 &&
       cmd_policy_find_local_slot("13800138000","+8613800138000","13900139000")==0 &&
       cmd_policy_find_local_slot("+8613800138000","13800138000","13800138000")==-2 &&
       cmd_policy_find_local_slot("3800138000","13800138000","13900139000")==-1 &&
       cmd_policy_dial_number("13800138000",dial,sizeof(dial),shown,sizeof(shown)) &&
       strcmp(dial,"+8613800138000")==0 && strcmp(shown,"13800138000")==0 &&
       cmd_policy_dial_number("+12025550123",dial,sizeof(dial),shown,sizeof(shown)) &&
       strcmp(dial,"+12025550123")==0;
    if (!ok) fails++;
    k = snprintf(report + used, report_len - used, "[%s] 号码归一化\n", ok ? "PASS" : "FAIL");
    used += k > 0 ? k : 0;

    if(!ok) fails++;
    k=snprintf(report+used,report_len-used,"[%s] 完整号码选卡与目标归一化\n",ok?"PASS":"FAIL");
    used+=k>0?k:0;

    fmt_incoming(buf,sizeof(buf),0,"卡1","15550000001","+12025550123",true,1763900000,true,"x");
    struct tm local_tm; char expected_time[32]; time_t test_ts=1763900000;
    localtime_r(&test_ts,&local_tm); strftime(expected_time,sizeof(expected_time),"%Y-%m-%d %H:%M:%S",&local_tm);
    ok=strstr(buf,"【收到来自+12025550123的短信】")!=NULL &&
       strstr(buf,"接收号码：15550000001")!=NULL && strstr(buf,expected_time)!=NULL &&
       strstr(buf,"****")==NULL;
    if(!ok) fails++;
    k=snprintf(report+used,report_len-used,"[%s] 短信标题和接收信息\n",ok?"PASS":"FAIL");
    used+=k>0?k:0;

    ok=cmd_policy_format_call_notice(buf,sizeof(buf),1,"工作卡","+8615550000002",
                                     "10086",1763900000,true)>0 &&
       strstr(buf,"【接到来自10086的来电】") && strstr(buf,"接收号码：15550000002") &&
       strstr(buf,"接收时间：") && !strstr(buf,"卡槽") && !strstr(buf,"自动挂断") && !strstr(buf,"呼转");
    if(!ok) fails++;
    k=snprintf(report+used,report_len-used,"[%s] 来电通知字段\n",ok?"PASS":"FAIL");
    used+=k>0?k:0;

    (void)n;
    return fails;
}

void cmd_init(void *modems)
{
    s_modems = (modem_t *)modems;
    if (!s_map_lock) s_map_lock=xSemaphoreCreateMutex();
    if (!map_store_init("/data/map.dat")) ESP_LOGE(TAG,"map store init failed");
}

void cmd_set_slot(int slot, const char *name, const char *number)
{
    if (slot < 0 || slot > 1) return;
    if (name && name[0]) strlcpy(s_slot_name[slot], name, sizeof(s_slot_name[slot]));
    if (number) strlcpy(s_slot_num[slot], number, sizeof(s_slot_num[slot]));
}

const char *cmd_slot_number(int slot)
{
    return (slot >= 0 && slot < 2) ? s_slot_num[slot] : "";
}

const char *cmd_slot_name(int slot)
{
    return (slot >= 0 && slot < 2) ? s_slot_name[slot] : "";
}
