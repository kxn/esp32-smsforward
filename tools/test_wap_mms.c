#include "../firmware/main/wap_mms.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *name)
{
    fprintf(stderr, "FAIL %s\n", name);
    return 1;
}

int main(void)
{
    /* Legacy WSP Push + compact MMS well-known header encodings. */
    const uint8_t push[] = {
        0x01, 0x06, 0x03, 0xbe, 0x81, 0xea,
        0x8c, 0x82, 0x8d, 0x92, 0x98, 'T','X','1',0,
        0x89, 0x80, '+','1','9','9','9','5','5','5','0','1','0','0','0',0,
        0x8a, 0x80, 0x88, 0x03, 0x81, 0x01, 0x01,
        0x83, 'h','t','t','p',':','/','/','m','m','s','/','1',0
    };
    wap_mms_info_t info;
    if (wap_mms_parse_push(push, sizeof(push), &info) != WAP_MMS_OK)
        return fail("legacy notification parse");
    if (info.message_type != 0x82 || strcmp(info.sender, "+199955501000") ||
        strcmp(info.transaction_id, "TX1") || strcmp(info.content_location, "http://mms/1"))
        return fail("notification fields");

    /* M-Retrieve.conf with text, image and an unsupported executable part. */
    static const uint8_t retrieve[] = {
        0x8c,0x84, 0x8d,0x92, 0x98,'R','1',0,
        0x99,0x81, /* failed retrieve status must be rejected */
        0x84,0xb3,
        0x03,
        0x1e,0x05, 0x03,0x83,0x81,0xea, 'C','o','n','t','e','n','t','-','L','o','c','a','t','i','o','n',0,
            'n','o','t','e','.','t','x','t',0, 'H','e','l','l','o',
        0x1c,0x04, 0x9e, 'C','o','n','t','e','n','t','-','L','o','c','a','t','i','o','n',0,
            'p','h','o','t','o','.','j','p','g',0, 0xff,0xd8,0xff,0xd9,
        0x12,0x03, 'a','p','p','l','i','c','a','t','i','o','n','/','x','-','e','x','e',0,
            'R','U','N'
    };
    wap_mms_retrieve_t result;
    if (wap_mms_parse_retrieve(retrieve, sizeof(retrieve), &result) != WAP_MMS_RETRIEVE_FAILED)
        return fail("failed retrieve status");

    /* Same well-formed multipart with success status. */
    uint8_t success[sizeof(retrieve)];
    memcpy(success, retrieve, sizeof(retrieve));
    success[9] = 0x80;
    if (wap_mms_parse_retrieve(success, sizeof(success), &result) != WAP_MMS_OK)
        return fail("retrieve multipart parse");
    if (result.part_count != 2 || result.ignored_parts != 1 ||
        strcmp(result.parts[0].mime, "text/plain") || strcmp(result.parts[0].charset, "UTF-8") ||
        strcmp(result.parts[0].filename, "note.txt") || result.parts[0].len != 5 ||
        memcmp(result.parts[0].data, "Hello", 5) ||
        strcmp(result.parts[1].mime, "image/jpeg") || strcmp(result.parts[1].filename, "photo.jpg") ||
        result.parts[1].len != 4 || result.parts[1].data[0] != 0xff)
        return fail("retrieve part fields/bounds");

    uint8_t decoded[8]; size_t decoded_len = 0;
    if (wap_mms_hex_decode("01aBff", 6, decoded, sizeof(decoded), &decoded_len) != WAP_MMS_OK ||
        decoded_len != 3 || decoded[0] != 1 || decoded[1] != 0xab || decoded[2] != 0xff)
        return fail("hex decode");
    if (wap_mms_hex_decode("01x", 3, decoded, sizeof(decoded), &decoded_len) != WAP_MMS_MALFORMED || decoded_len)
        return fail("hex malformed rejection");
    if (wap_mms_parse_retrieve(retrieve, sizeof(retrieve), &result) != WAP_MMS_RETRIEVE_FAILED)
        return fail("repeat deterministic parse");

    uint8_t encoded[] = {
        0x8c,0x84,0x8d,0x92,0x84,0xb3,0x01,
        0x22,0x08,0x83,
        'C','o','n','t','e','n','t','-','T','r','a','n','s','f','e','r','-','E','n','c','o','d','i','n','g',0,
        'b','a','s','e','6','4',0,
        'S','G','V','s','b','G','8','='
    };
    if (wap_mms_parse_retrieve(encoded, sizeof(encoded), &result) != WAP_MMS_OK ||
        result.part_count != 0 || result.ignored_parts != 1)
        return fail("const parser skips transfer-encoded text");
    if (wap_mms_parse_retrieve_mutable(encoded, sizeof(encoded), &result) != WAP_MMS_OK ||
        result.part_count != 1 || result.parts[0].len != 5 ||
        memcmp(result.parts[0].data, "Hello", 5) ||
        result.parts[0].transfer_encoding != WAP_MMS_ENC_NONE)
        return fail("mutable Base64 text decode");

    uint8_t gateway_base64[] =
        "\x8c\x84\x84\xb3\x01\x22\x05\x83"
        "Content-Transfer-Encoding\0base64\0"
        "SGVsb";
    if (wap_mms_parse_retrieve_mutable(gateway_base64, sizeof(gateway_base64) - 1, &result) != WAP_MMS_OK ||
        result.part_count != 1 || result.parts[0].len != 3 || memcmp(result.parts[0].data, "Hel", 3))
        return fail("Base64 with trailing sextet");

    uint8_t qp[] =
        "\x8c\x84" "X-Mms-Extra\0extra\0" "\xa1\x1f\x01\xff\x84\xb3\x01"
        "\x49\x09\x06\x83\x85" "t\0" "\x81\xea"
        "\x8e" "note.txt\0" "\xc0" "\"<root>\0"
        "\xa5\x02\x01\x80" "content-transfer-encoding\0quoted-printable\0"
        "a=3D=\r\nb!";
    if (wap_mms_parse_retrieve_mutable(qp, sizeof(qp) - 1, &result) != WAP_MMS_OK ||
        result.part_count != 1 || result.parts[0].len != 4 || memcmp(result.parts[0].data, "a=b!", 4) ||
        strcmp(result.parts[0].charset, "UTF-8") || strcmp(result.parts[0].filename, "note.txt"))
        return fail("quoted-printable and extension headers");

    const uint8_t single[] = { 0x8c,0x84,0x84,0x1f,0x05,0x83,0x81,0x02,0x03,0xf5,'A',0 };
    if (wap_mms_parse_retrieve(single, sizeof(single), &result) != WAP_MMS_OK ||
        result.part_count != 1 || strcmp(result.parts[0].charset, "UTF-16BE") || result.parts[0].len != 2)
        return fail("single part general content type and long charset");

    const uint8_t failed_no_body[] = { 0x8c,0x84,0x92,0xe2 };
    if (wap_mms_parse_retrieve(failed_no_body, sizeof(failed_no_body), &result) != WAP_MMS_RETRIEVE_FAILED)
        return fail("failed response without a content type");
    const uint8_t duplicate_status[] = { 0x8c,0x84,0x99,0xe2,0x99,0x80,0x84,0x83,'x' };
    if (wap_mms_parse_retrieve(duplicate_status, sizeof(duplicate_status), &result) == WAP_MMS_OK)
        return fail("duplicate retrieve status rejection");

    /* Truncated length fields and unsupported HTML never produce content. */
    const uint8_t truncated[] = { 0x8c,0x84,0x84,0xb3,0x81 };
    if (wap_mms_parse_retrieve(truncated, sizeof(truncated), &result) == WAP_MMS_OK)
        return fail("truncated multipart rejection");
    puts("PASS WAP push, MMS retrieve bounds and safe part filtering");
    return 0;
}
