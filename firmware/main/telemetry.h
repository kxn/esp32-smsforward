#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_MAX_CORES 2
#define TELEMETRY_PERIOD_US UINT64_C(5000000)
#define TELEMETRY_LEASE_US UINT64_C(15000000)
#define TELEMETRY_CPU_MIN_WINDOW_US UINT64_C(4500000)
#define TELEMETRY_CPU_MAX_WINDOW_US UINT64_C(5500000)

typedef enum {
    TELEMETRY_UNAVAILABLE,
    TELEMETRY_WARMING_UP,
    TELEMETRY_OK,
    TELEMETRY_UNSUPPORTED,
    TELEMETRY_OUT_OF_RANGE,
} telemetry_state_t;

typedef struct {
    uint64_t time_us;
    uint64_t idle_us;
} telemetry_cpu_counter_t;

typedef struct {
    telemetry_state_t state;
    unsigned cores;
    unsigned window_ms;
    unsigned age_ms;
    double used_pct;
    double core_pct[TELEMETRY_MAX_CORES];
} telemetry_cpu_t;

typedef struct {
    telemetry_cpu_t cpu;
    telemetry_state_t temperature_state;
    float temperature_c;
    unsigned temperature_age_ms;
    uint64_t uptime_s;
    size_t heap_free_bytes;
    size_t heap_total_bytes;
    size_t heap_min_free_bytes;
    size_t psram_free_bytes;
    size_t psram_total_bytes;
} telemetry_snapshot_t;

typedef enum {
    TELEMETRY_MONITOR_SAMPLE,
    TELEMETRY_MONITOR_WAIT,
    TELEMETRY_MONITOR_EXPIRE,
} telemetry_monitor_event_t;

telemetry_monitor_event_t telemetry_monitor_event(uint64_t now, uint64_t due,
                                                   uint64_t last_request, uint64_t *wait_us);
uint64_t telemetry_next_sample(uint64_t due, uint64_t finished_at);

bool telemetry_cpu_window(const telemetry_cpu_counter_t *start,
                          const telemetry_cpu_counter_t *end, unsigned cores,
                          telemetry_cpu_t *result);

/* Called in task context, serialized by the RPC mutex. */
void telemetry_read(telemetry_snapshot_t *snapshot);
