#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/main/sms_seen.h"

void test_log(const char *tag, const char *message, ...) { (void)tag; (void)message; }

typedef struct { bool send_ok, map_ok; int sends, maps, deletes; } fake_delivery_t;
static bool fake_send(void *p) { fake_delivery_t *f=p; f->sends++; return f->send_ok; }
static bool fake_map(void *p) { fake_delivery_t *f=p; f->maps++; return f->map_ok; }

int main(void)
{
    const char *path = "tools/sms_seen_fixture.dat";
    remove(path);
    sms_seen_store_t first;
    assert(sms_seen_init(&first, path));
    assert(!sms_seen_contains(&first, UINT64_C(0x123456789ABCDEF0)));
    assert(sms_seen_commit(&first, UINT64_C(0x123456789ABCDEF0)));
    assert(sms_seen_contains(&first, UINT64_C(0x123456789ABCDEF0)));

    /* Simulate reboot: only the synced journal record suppresses delivery. */
    sms_seen_store_t after_reboot;
    assert(sms_seen_init(&after_reboot, path));
    assert(sms_seen_contains(&after_reboot, UINT64_C(0x123456789ABCDEF0)));

    sms_seen_store_t failed;
    assert(sms_seen_init(&failed, "tools/no_such_dir/seen.dat"));
    /* A vanished file and a vanished directory both look like ENOENT here; only the
     * first write can tell them apart, and it marks the journal degraded. */
    assert(!sms_seen_commit(&failed, 7));
    assert(!failed.journal_ok);
    assert(!sms_seen_contains(&failed, 7));

    /* A send that never happened keeps the SIM copy and records nothing. */
    fake_delivery_t tx = { .send_ok = false, .map_ok = true };
    bool accepted = sms_seen_deliver(&first, 42, fake_send, fake_map, &tx);
    if (accepted) tx.deletes++;
    assert(!accepted && tx.sends == 1 && tx.maps == 0 && tx.deletes == 0);
    assert(!sms_seen_contains(&first, 42));

    /* A successful send is journaled even when the quote map fails: the message is
     * delivered exactly once and the SIM copy can be deleted. */
    tx.send_ok = tx.map_ok = true;
    tx.map_ok = false;
    accepted = sms_seen_deliver(&first, 42, fake_send, fake_map, &tx);
    if (accepted) tx.deletes++;
    assert(accepted && tx.sends == 2 && tx.maps == 1 && tx.deletes == 1);
    assert(sms_seen_contains(&first, 42));

    /* A journal write failure must not veto a delivery that already happened:
     * blocking it would keep the SIM copy and re-push the message every sweep. */
    accepted = sms_seen_deliver(&failed, 8, fake_send, fake_map, &tx);
    if (accepted) tx.deletes++;
    assert(accepted && tx.deletes == 2);
    assert(!sms_seen_contains(&failed, 8));

    /* A degraded journal must stay visible instead of silently disabling dedup. */
    assert(first.journal_ok);
    assert(sms_seen_state(&first, 42) == SMS_SEEN_ACKED);
    assert(sms_seen_state(&first, 43) == SMS_SEEN_PENDING);
    assert(!failed.journal_ok);
    assert(sms_seen_state(&failed, 43) == SMS_SEEN_UNKNOWN);

    sms_seen_store_t third_boot;
    assert(sms_seen_init(&third_boot, path));
    assert(sms_seen_contains(&third_boot, 42));
    tx.sends = tx.maps = 0;
    accepted = sms_seen_deliver(&third_boot, 42, fake_send, fake_map, &tx);
    assert(accepted && tx.sends == 0 && tx.maps == 0);
    remove(path);
    puts("SMS seen-journal tests passed");
    return 0;
}
