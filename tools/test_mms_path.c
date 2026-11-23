#include "pdu.h"
#include "mms_policy.h"
#include "wap_mms.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A push arriving as binary SMS, followed by a binary MM1 HTTP response. The
 * wire constants are from the OMA WSP and MMS encoding registries. */
int main(void)
{
    static const uint8_t push[] =
        "\x01\x06\x01\xbe"
        "\x8c\x82\x98" "tx1\0" "\x8d\x91"
        "\x89\x0d\x80" "13900000000\0"
        "\x8a\x80\x8e\x02\x01\x00\x88\x03\x81\x01\x3c"
        "\x83" "http://mmsc.myuni.com.cn/m/one\0";
    static const uint8_t sms_header[] = {
        0x00,0x40, 0x05,0x81,0x01,0x90,0xf6, 0x00,0x04,
        0x62,0x11,0x32,0x11,0x22,0x33,0x23
    };
    static const uint8_t udh[] = {6,5,4,0x0b,0x84,0x23,0xf0};
    uint8_t sms[256], raw[160];
    size_t n = sizeof(sms_header);
    memcpy(sms,sms_header,n);
    sms[n++] = sizeof(udh) + sizeof(push) - 1;
    memcpy(sms+n,udh,sizeof(udh)); n += sizeof(udh);
    memcpy(sms+n,push,sizeof(push)-1); n += sizeof(push)-1;
    sms_deliver_t d;
    assert(pdu_decode_deliver(sms,n,&d)==0 && d.mms_notification);
    size_t size = 0;
    assert(wap_mms_hex_decode(d.text,strlen(d.text),raw,sizeof(raw),&size)==WAP_MMS_OK);
    assert(size==sizeof(push)-1 && !memcmp(raw,push,size));
    wap_mms_info_t info;
    assert(wap_mms_parse_push(raw,size,&info)==WAP_MMS_OK);
    assert(!strcmp(info.sender,"13900000000") && info.has_expiry && info.expiry_relative &&
           info.expiry_seconds==60 && info.has_message_size && info.message_size_bytes==256);
    const mms_profile_t *profile=mms_profile_from_imsi("460010123456789\nOK");
    assert(mms_home_registered("+CEREG: 0,1") && mms_serving_home(profile,"+COPS: 0,2,\"46001\",7"));
    char get[256];
    assert(mms_http_request(profile,info.content_location,get,sizeof(get))>0);
    assert(strstr(get,"GET http://mmsc.myuni.com.cn/m/one HTTP/1.1\r\n"));

    /* Multipart/related (0x33), text/plain with UTF-8 charset and PNG (0x20). */
    static const uint8_t entity[] = {
        0x8c,0x84,0x8d,0x91,0x85,1,1,0x84,0xb3,2,
        4,3,3,0x83,0x81,0xea,0xe5,0xad,0x97,
        1,8,0xa0,0x89,'P','N','G',13,10,26,10
    };
    uint8_t wire[256];
    int h=snprintf((char*)wire,sizeof(wire),"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",sizeof(entity));
    assert(h>0 && (size_t)h+sizeof(entity)<sizeof(wire));
    memcpy(wire+h,entity,sizeof(entity));
    assert(mms_http_decode(wire,h+sizeof(entity),false,&size)==1 && size==sizeof(entity));
    wap_mms_retrieve_t doc;
    assert(wap_mms_parse_retrieve_mutable(wire,size,&doc)==WAP_MMS_OK && doc.part_count==2);
    assert(!strcmp(doc.parts[0].mime,"text/plain") && doc.parts[0].len==3 &&
           !memcmp(doc.parts[0].data,"\xe5\xad\x97",3));
    assert(!strcmp(doc.parts[1].mime,"image/png") && doc.parts[1].len==8);
    puts("SMS-DELIVER to MM1 request and HTTP response to text/image verified");
    return 0;
}
