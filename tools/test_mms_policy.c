#include "mms_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void decode(const char *text, bool eof, int expected, const char *body)
{
    uint8_t wire[1024];
    size_t n = 0, len = strlen(text);
    memcpy(wire, text, len);
    assert(mms_http_decode(wire, len, eof, &n) == expected);
    if (expected == 1) assert(strlen(body) == n && !memcmp(wire, body, n));
    else if (expected == 0) assert(!memcmp(wire, text, len));
}

int main(void)
{
    const mms_profile_t *u = mms_profile_from_imsi("AT+CIMI\n460010123456789\nOK\n");
    assert(u && !strcmp(u->apn, "3gwap"));
    assert(mms_serving_home(u, "+COPS: 0,2,\"46001\",7\nOK\n"));
    assert(!mms_serving_home(u, "+COPS: 0,2,\"46003\",7\nOK\n"));
    assert(!mms_serving_home(u, "+COPS: 0,2,\"45401\",7\nOK\n"));
    assert(!mms_serving_home(u, "+COPS: 0,0,\"UNICOM\"\nOK\n"));
    assert(!strcmp(mms_profile_from_imsi("460110123456789")->proxy, "10.0.0.200"));
    assert(!strcmp(mms_profile_from_imsi("460000123456789")->apn, "cmwap"));
    assert(!mms_profile_from_imsi("460010\nOK") && !mms_profile_from_imsi("310120123456789"));
    assert(!mms_profile_from_imsi("460010123456789\n460000123456789\n"));
    assert(mms_home_registered("+CEREG: 2,1,\"1234\",\"01234567\",7\nOK\n"));
    const char *deny[] = { "+CEREG: 0,5", "+CEREG: 0,0", "+CEREG: 1", "+CEREG: 1,10",
        "+CEREG: 2,1\n+CEREG: 5", "OK", "+CEREG: 0,2", "+CEREG: 0,3", "+CEREG: 0,4" };
    for (size_t i=0; i<sizeof(deny)/sizeof(*deny); ++i) assert(!mms_home_registered(deny[i]));
    char req[900];
    assert(mms_http_request(u,"http://mmsc.myuni.com.cn:80/p?id=1",req,sizeof(req)) > 0);
    assert(strstr(req,"GET http://mmsc.myuni.com.cn/p?id=1 HTTP/1.1\r\n"));
    assert(mms_http_request(u,"/p",req,sizeof(req)) > 0);
    const char *bad[] = {"//other/path","http://mmsc.myuni.com.cn.evil/a", "/req\r\nHost:x",
                         "https://mmsc.myuni.com.cn/a","http://mmsc.myuni.com.cn@other/a",
                         "http://mmsc.myuni.com.cn:8080/a", "http://mmsc.vnet.mobi/a"};
    for(size_t i=0; i<sizeof(bad)/sizeof(*bad); ++i) assert(mms_http_request(u,bad[i],req,sizeof(req))<0);
    decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc",false,1,"abc");
    decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na",false,0,NULL);
    decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na",true,-1,NULL);
    decode("HTTP/1.0 200 OK\r\n\r\nabc",true,1,"abc");
    decode("HTTP/1.1 302 Redirect\r\n\r\n",true,-1,NULL);
    decode("HTTP/1.1 200 OK\r\nContent-Length: 9999999999\r\n\r\n",false,-1,NULL);
    decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",true,-1,NULL);
    const char *chunk="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n1;x=y\r\nc\r\n0\r\nTrace: 1\r\n\r\n";
    for (size_t i=0;i<strlen(chunk);++i) {
        char partial[160];
        memcpy(partial,chunk,i);
        partial[i]=0;
        decode(partial,false,0,NULL);
    }
    decode(chunk,false,1,"abc");
    puts("MMS home-network policy, proxy target and HTTP framing passed");
    return 0;
}
