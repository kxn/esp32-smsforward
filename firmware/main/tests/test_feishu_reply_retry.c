#include "../feishu_retry_policy.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    if (!feishu_retry_policy(true,0)) return 1;
    if (!feishu_retry_policy(false,500) || !feishu_retry_policy(false,503)) return 2;
    if (feishu_retry_policy(false,400) || feishu_retry_policy(false,401)) return 3;
    if (feishu_retry_policy(false,200) || feishu_retry_policy(true,429)) return 4;
    char uuid[51];
    if (!feishu_reply_uuid("om_event123",uuid) || strcmp(uuid,"reply_om_event123")) return 5;
    if (strncmp(uuid,"sms_",4)==0 || feishu_reply_uuid("bad/id",uuid)) return 6;
    char long_id[61], long_uuid[51]; memset(long_id,'a',60); long_id[60]=0;
    if (!feishu_reply_uuid(long_id,uuid) || strlen(uuid)>50) return 7;
    if (!feishu_reply_uuid(long_id,long_uuid) || strcmp(uuid,long_uuid)) return 8;
    puts("PASS Feishu reply retry policy");
    return 0;
}
