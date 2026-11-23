#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/sms_assembly.h"

static void test_out_of_order_and_duplicate(void)
{
    sms_assembly_pool_t p;
    sms_assembly_init(&p);
    int ai = -1;
    assert(sms_assembly_add(&p, 0, "+100", 9, 8, 3, 2, 100, false, false, 20, 20,
                            "B", 0, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 0, "+100", 9, 8, 3, 2, 101, false, false, 21, 20,
                            "B", 1, 60000, &ai) == SMS_ASSEMBLY_DUPLICATE);
    assert(p.slots[ai].timestamp == 100); /* seq 1 timestamp will replace this later */
    assert(p.slots[ai].parts[1].index_count == 2);
    assert(sms_assembly_add(&p, 0, "+100", 9, 8, 3, 1, 102, false, false, 22, 10,
                            "A", 2, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(p.slots[ai].timestamp == 102);
    assert(sms_assembly_add(&p, 0, "+100", 9, 8, 3, 3, 103, false, false, 23, 30,
                            "C", 3, 60000, &ai) == SMS_ASSEMBLY_COMPLETE);
    char out[64];
    int got, total;
    assert(sms_assembly_build(&p.slots[ai], false, out, sizeof(out), &got, &total) == 1);
    assert(strcmp(out, "ABC") == 0 && got == 3 && total == 3);
    assert(p.slots[ai].parts[1].sim_refs[1].index == 21);
    sms_assembly_destroy(&p);
}

static void test_timeout_partial(void)
{
    sms_assembly_pool_t p;
    sms_assembly_init(&p);
    int ai = -1;
    assert(sms_assembly_add(&p, 1, "+200", 7, 16, 3, 3, 200, false, false, 30, 30,
                            "C", 10, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 1, "+200", 7, 16, 3, 1, 201, false, false, 31, 31,
                            "A", 11, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_build(&p.slots[ai], false, (char[64]){0}, 64, NULL, NULL) == 0);
    char out[64];
    int got, total;
    assert(sms_assembly_build(&p.slots[ai], true, out, sizeof(out), &got, &total) == 1);
    assert(strcmp(out, "AC[2/3]") == 0 && got == 2 && total == 3);
    sms_assembly_destroy(&p);
}

static void test_reference_width_and_slot_isolation(void)
{
    sms_assembly_pool_t p;
    sms_assembly_init(&p);
    int a, b, c;
    assert(sms_assembly_add(&p, 0, "+300", 5, 8, 2, 1, 1, false, false, 1, 1,
                            "a", 0, 60000, &a) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 0, "+300", 5, 16, 2, 1, 1, false, false, 2, 2,
                            "b", 0, 60000, &b) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 1, "+300", 5, 8, 2, 1, 1, false, false, 3, 3,
                            "c", 0, 60000, &c) == SMS_ASSEMBLY_ADDED);
    assert(a != b && a != c && b != c);
    sms_assembly_destroy(&p);
}

static void test_binary_transport_and_port_flag(void)
{
    sms_assembly_pool_t p;
    sms_assembly_init(&p);
    int ai = -1;
    /* Port IE may repeat on segments or appear only on the first one. */
    assert(sms_assembly_add(&p, 0, "+400", 11, 8, 2, 1, 1, true, true, 1, 1,
                            "010203", 0, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 0, "+400", 11, 8, 2, 2, 2, false, true, 2, 2,
                            "0405", 1, 60000, &ai) == SMS_ASSEMBLY_COMPLETE);
    assert(p.slots[ai].mms_notification && p.slots[ai].binary_payload_hex);
    sms_assembly_clear(&p.slots[ai]);

    /* An ordinary 8-bit concatenation has no port IE but every segment stays hex-safe. */
    assert(sms_assembly_add(&p, 1, "+500", 12, 16, 2, 2, 3, false, true, 3, 3,
                            "4142", 2, 60000, &ai) == SMS_ASSEMBLY_ADDED);
    assert(sms_assembly_add(&p, 1, "+500", 12, 16, 2, 1, 4, false, true, 4, 4,
                            "4344", 3, 60000, &ai) == SMS_ASSEMBLY_COMPLETE);
    assert(!p.slots[ai].mms_notification && p.slots[ai].binary_payload_hex);
    sms_assembly_destroy(&p);
}

int main(void)
{
    test_out_of_order_and_duplicate();
    test_timeout_partial();
    test_reference_width_and_slot_isolation();
    test_binary_transport_and_port_flag();
    puts("SMS assembly tests passed");
    return 0;
}
