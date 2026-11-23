#include "../sim_number_policy.h"
#include <string.h>

int main(void)
{
    char number[32];
    if (!sim_number_parse_cnum("AT+CNUM\r\n+CNUM: ,\"+8615550000011\",145\r\nOK\r\n",
                               number, sizeof(number)) || strcmp(number, "+8615550000011")) return 1;
    if (!sim_number_parse_cnum("noise \"wrong\"\r\n+CNUM: \"Line 1\",\"15550000012\",129\r\n",
                               number, sizeof(number)) || strcmp(number, "15550000012")) return 2;
    if (!sim_number_parse_own_phonebook("+CPBR: 1,\"15550000013\",129,\"Own\"\r\nOK\r\n",
                                        number, sizeof(number)) || strcmp(number, "15550000013")) return 3;
    if (sim_number_parse_cnum("+ICCID: 8986001234567890123\r\nOK\r\n", number, sizeof(number))) return 4;
    if (sim_number_parse_own_phonebook("+CPBR: 1,\"\",129,\"Own\"\r\n", number, sizeof(number))) return 5;
    if (!sim_number_parse_cnum("+CNUM: ,\"\",129\r\n+CNUM: broken\r\n+CNUM: ,\"15550000014\",129\r\n",
                               number, sizeof(number)) || strcmp(number, "15550000014")) return 6;
    if (!sim_number_parse_cnum("+CNUM: ,\"12\",129\r\n+CNUM: ,\"15550000014\",129\r\n",
                               number, sizeof(number)) || strcmp(number, "15550000014")) return 16;
    if (!sim_number_parse_own_phonebook("+CPBR: broken\r\n+CPBR: 2,\"15550000015\",129,\"Own\"\r\n",
                                        number, sizeof(number)) || strcmp(number, "15550000015")) return 7;
    if (sim_number_parse_cnum("+CNUM: ,\"unterminated\r\nOK \"15550000016\"\r\n", number, sizeof(number))) return 8;
    const unsigned char fcp[] = {
        0x62,0x19,0x82,0x05,0x42,0x21,0x00,0x1c,0x02,
        0x83,0x02,0x6f,0x40,0x80,0x02,0x00,0x38,0x8a,0x01,0x05,
        0x8b,0x03,0x2f,0x06,0x03,0xc6,0x00
    };
    unsigned short record_len=0; unsigned char record_count=0;
    if (!sim_number_msisdn_layout(fcp,sizeof(fcp),&record_len,&record_count) ||
        record_len!=28 || record_count!=2) return 9;
    const unsigned char filled[28] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0x08,0x91,0x68,0x51,0x55,0x00,0x00,0x10,0xf7,0xff,0xff,0xff,0xff,0xff
    };
    if (!sim_number_decode_msisdn(filled,sizeof(filled),number,sizeof(number)) ||
        strcmp(number,"+8615550000017")) return 10;
    unsigned char empty[28]; memset(empty,0xff,sizeof(empty));
    if (sim_number_decode_msisdn(empty,sizeof(empty),number,sizeof(number))) return 11;
    unsigned char corrupt[28]; memcpy(corrupt,filled,sizeof(corrupt)); corrupt[14]=12;
    if (sim_number_decode_msisdn(corrupt,sizeof(corrupt),number,sizeof(number))) return 12;
    unsigned char parsed[32]; size_t parsed_len=0;
    if (!sim_number_parse_crsm("+CRSM: 144,0,\"6203820100\"\r\nOK\r\n",parsed,sizeof(parsed),&parsed_len) ||
        parsed_len!=5 || parsed[0]!=0x62) return 13;
    if (sim_number_parse_crsm("+CRSM: 106,130,\"FFFF\"\r\n",parsed,sizeof(parsed),&parsed_len)) return 14;
    if (sim_number_parse_crsm("+CRSM: 144,ok,\"FFFF\"\r\n",parsed,sizeof(parsed),&parsed_len)) return 17;
    if (!sim_number_response_field("AT+COPS?\r\n+COPS: 0,0,\"Synthetic Carrier\",7\r\nOK\r\n",
                                   "+COPS:",2,number,sizeof(number)) ||
        strcmp(number,"Synthetic Carrier")) return 15;
    unsigned char bad_fcp[sizeof(fcp)]; memcpy(bad_fcp,fcp,sizeof(fcp)); bad_fcp[3]=0x41;
    if (sim_number_msisdn_layout(bad_fcp,sizeof(bad_fcp),&record_len,&record_count) ||
        sim_number_msisdn_layout(fcp,sizeof(fcp)-1,&record_len,&record_count)) return 18;
    memcpy(corrupt,filled,sizeof(corrupt)); corrupt[16]=0x6f;
    if (sim_number_decode_msisdn(corrupt,sizeof(corrupt),number,sizeof(number))) return 19;
    memcpy(corrupt,filled,sizeof(corrupt)); corrupt[15]=0x82;
    if (sim_number_decode_msisdn(corrupt,sizeof(corrupt),number,sizeof(number))) return 20;
    return 0;
}
