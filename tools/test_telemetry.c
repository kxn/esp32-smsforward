#include "telemetry.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint64_t wait_us = 0;
    assert(telemetry_monitor_event(100, 100, 100, &wait_us) == TELEMETRY_MONITOR_SAMPLE);
    uint64_t due = telemetry_next_sample(100, 2000);
    assert(due == 100 + TELEMETRY_PERIOD_US);
    assert(telemetry_monitor_event(3000000, due, 2500000, &wait_us) == TELEMETRY_MONITOR_WAIT);
    assert(wait_us == due - 3000000);
    assert(telemetry_monitor_event(due, due, 4000000, &wait_us) == TELEMETRY_MONITOR_SAMPLE);
    assert(telemetry_next_sample(due, due + 10000) == due + TELEMETRY_PERIOD_US);
    assert(telemetry_next_sample(due, due + 9000000) == due + 9000000 + TELEMETRY_PERIOD_US);

    uint64_t expires = 2500000 + TELEMETRY_LEASE_US;
    assert(telemetry_monitor_event(expires - 1, expires + 10, 2500000, &wait_us) == TELEMETRY_MONITOR_WAIT);
    assert(wait_us == 1);
    assert(telemetry_monitor_event(expires, expires, 2500000, &wait_us) == TELEMETRY_MONITOR_EXPIRE);
    assert(telemetry_monitor_event(expires, expires, expires - 1, &wait_us) == TELEMETRY_MONITOR_SAMPLE);

    /* Both cores' lifetime counters have exceeded 32 bits. */
    const telemetry_cpu_counter_t start[] = {
        { .time_us = UINT64_C(900000000000), .idle_us = UINT64_C(800000000000) },
        { .time_us = UINT64_C(900000000200), .idle_us = UINT64_C(700000000000) },
    };
    telemetry_cpu_counter_t end[] = {
        { .time_us = start[0].time_us + 5000000, .idle_us = start[0].idle_us + 5000000 },
        { .time_us = start[1].time_us + 5000000, .idle_us = start[1].idle_us },
    };
    telemetry_cpu_t result;
    assert(telemetry_cpu_window(start, end, 2, &result));
    assert(result.state == TELEMETRY_OK && result.cores == 2 && result.window_ms == 5000);
    assert(result.used_pct == 50.0 && result.core_pct[0] == 0.0 && result.core_pct[1] == 100.0);

    /* Light sleep and uninterrupted idle both accrue to the idle tasks. */
    end[1].idle_us += 5000000;
    assert(telemetry_cpu_window(start, end, 2, &result) && result.used_pct == 0.0);
    end[1].idle_us += 1;
    assert(telemetry_cpu_window(start, end, 2, &result) && result.used_pct == 0.0);
    end[1].idle_us += 2000;
    assert(!telemetry_cpu_window(start, end, 2, &result));
    assert(telemetry_cpu_window(start, end, 1, &result) && result.cores == 1);

    end[1].idle_us = start[1].idle_us - 1;
    assert(!telemetry_cpu_window(start, end, 2, &result));
    end[1].idle_us = start[1].idle_us;
    end[1].time_us = start[1].time_us - 1;
    assert(!telemetry_cpu_window(start, end, 2, &result));
    end[1].time_us = start[1].time_us + TELEMETRY_CPU_MIN_WINDOW_US - 1;
    assert(!telemetry_cpu_window(start, end, 2, &result));
    end[1].time_us = start[1].time_us + TELEMETRY_CPU_MAX_WINDOW_US + 1;
    assert(!telemetry_cpu_window(start, end, 2, &result));
    assert(!telemetry_cpu_window(start, end, 0, &result));
    puts("Telemetry cadence, lease, dual-core CPU window, sleep and counter validation passed");
    return 0;
}
