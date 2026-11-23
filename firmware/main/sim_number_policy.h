#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

static inline bool sim_number_csv_field(const char *p, const char *end, int wanted,
                                        char *out, size_t out_len)
{
    for (int field = 0; p <= end; ++field) {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        const char *start = p, *stop = p;
        if (p < end && *p == '"') {
            start = ++p;
            while (p < end && *p != '"') ++p;
            if (p == end) return false;
            stop = p++;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p < end && *p != ',') return false;
        } else {
            while (p < end && *p != ',') ++p;
            stop = p;
            while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t')) --stop;
        }
        if (field == wanted) {
            size_t n = (size_t)(stop - start);
            if (!n || n >= out_len) return false;
            memcpy(out, start, n); out[n] = '\0'; return true;
        }
        if (p == end) return false;
        ++p;
    }
    return false;
}

static inline bool sim_number_response_field(const char *response, const char *prefix,
                                             int field, char *out, size_t out_len)
{
    if (!response || !prefix || !out || !out_len) return false;
    size_t prefix_len = strlen(prefix);
    for (const char *line = response; *line;) {
        while (*line == '\r' || *line == '\n') ++line;
        const char *end = line + strcspn(line, "\r\n");
        if ((size_t)(end-line) >= prefix_len && !memcmp(line,prefix,prefix_len) &&
            sim_number_csv_field(line+prefix_len,end,field,out,out_len)) return true;
        line = end;
    }
    return false;
}

static inline bool sim_number_candidate(const char *number)
{
    if (!number) return false;
    const char *p = number;
    if (*p == '+') {
        if (strncmp(p, "+86", 3) != 0) return false;
        p += 3;
    } else if (strncmp(p, "86", 2) == 0 && strlen(p) == 13) {
        p += 2;
    }
    if (strlen(p) != 11 || *p != '1') return false;
    for (; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}

static inline bool sim_number_parse_records(const char *response, const char *prefix,
                                            char *out, size_t out_len)
{
    if (!response || !prefix) return false;
    size_t prefix_len = strlen(prefix);
    for (const char *line = response; *line;) {
        while (*line == '\r' || *line == '\n') ++line;
        const char *end = line + strcspn(line, "\r\n");
        if ((size_t)(end - line) >= prefix_len && !memcmp(line, prefix, prefix_len) &&
            sim_number_csv_field(line + prefix_len, end, 1, out, out_len) &&
            sim_number_candidate(out)) return true;
        line = end;
    }
    return false;
}

static inline bool sim_number_parse_cnum(const char *response, char *out, size_t out_len)
{ return sim_number_parse_records(response, "+CNUM:", out, out_len); }

static inline bool sim_number_parse_own_phonebook(const char *response, char *out, size_t out_len)
{ return sim_number_parse_records(response, "+CPBR:", out, out_len); }

static inline int sim_number_hex_nibble(char c)
{
    return c>='0'&&c<='9' ? c-'0' : c>='A'&&c<='F' ? c-'A'+10 : c>='a'&&c<='f' ? c-'a'+10 : -1;
}

static inline bool sim_number_parse_crsm(const char *response, uint8_t *data,
                                         size_t capacity, size_t *data_len)
{
    if (!response || !data || !data_len) return false;
    for (const char *line=response; *line;) {
        while (*line=='\r'||*line=='\n') ++line;
        const char *end=line+strcspn(line,"\r\n");
        if ((size_t)(end-line)>=6 && !memcmp(line,"+CRSM:",6)) {
            char sw1[8],sw2[8],hex[512];
            if (!sim_number_csv_field(line+6,end,0,sw1,sizeof(sw1)) ||
                !sim_number_csv_field(line+6,end,1,sw2,sizeof(sw2)) ||
                strcmp(sw1, "144") != 0 || strcmp(sw2, "0") != 0 ||
                !sim_number_csv_field(line+6,end,2,hex,sizeof(hex))) { line=end; continue; }
            size_t chars=strlen(hex);
            if (!chars || (chars&1) || chars/2>capacity) return false;
            for (size_t i=0;i<chars/2;i++) {
                int hi=sim_number_hex_nibble(hex[i*2]),lo=sim_number_hex_nibble(hex[i*2+1]);
                if (hi<0||lo<0) return false;
                data[i]=(uint8_t)((hi<<4)|lo);
            }
            *data_len=chars/2; return true;
        }
        line=end;
    }
    return false;
}

static inline bool sim_number_msisdn_layout(const uint8_t *fcp,size_t fcp_len,
                                            uint16_t *record_len,uint8_t *record_count)
{
    if (!fcp || fcp_len < 2 || fcp[0] != 0x62 || fcp[1] != fcp_len - 2) return false;
    size_t p=2,end=2+fcp[1];
    while (p+2<=end) {
        uint8_t tag=fcp[p++],len=fcp[p++];
        if (p+len>end) return false;
        if (tag==0x82) {
            if (len != 5 || (fcp[p] & 0x07) != 0x02) return false;
            uint16_t r=(uint16_t)((fcp[p+2]<<8)|fcp[p+3]);
            uint8_t count=fcp[p+4];
            if (r<14||r>255||!count) return false;
            *record_len=r; *record_count=count; return true;
        }
        p+=len;
    }
    return false;
}

static inline bool sim_number_decode_msisdn(const uint8_t *record,size_t record_len,
                                            char *out,size_t out_len)
{
    if (!record||record_len<14||!out||out_len<2) return false;
    const uint8_t *footer=record+record_len-14;
    uint8_t number_len=footer[0],ton=footer[1];
    if (number_len < 2 || number_len > 11 || (ton != 0x81 && ton != 0x91)) return false;
    bool international = ton == 0x91;
    size_t n = 0;
    if (international) out[n++]='+';
    for (uint8_t i=0;i<number_len-1;i++) {
        uint8_t b = footer[2 + i], low = b & 0x0f, high = b >> 4;
        if (low > 9 || n + 1 >= out_len) return false;
        out[n++] = (char)('0' + low);
        if (high == 0x0f) {
            if (i != number_len - 2) return false;
        } else {
            if (high > 9 || n + 1 >= out_len) return false;
            out[n++] = (char)('0' + high);
        }
    }
    out[n]='\0';
    return n>(international?1u:0u);
}
