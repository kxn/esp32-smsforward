#include "telemetry.h"

telemetry_monitor_event_t telemetry_monitor_event(uint64_t now, uint64_t due,
                                                   uint64_t last_request, uint64_t *wait_us)
{
    uint64_t expires = last_request + TELEMETRY_LEASE_US;
    if (now >= expires) return TELEMETRY_MONITOR_EXPIRE;
    if (now >= due) return TELEMETRY_MONITOR_SAMPLE;
    uint64_t wake = expires < due ? expires : due;
    *wait_us = wake - now;
    return TELEMETRY_MONITOR_WAIT;
}

uint64_t telemetry_next_sample(uint64_t due, uint64_t finished_at)
{
    uint64_t next = due + TELEMETRY_PERIOD_US;
    return next > finished_at ? next : finished_at + TELEMETRY_PERIOD_US;
}

bool telemetry_cpu_window(const telemetry_cpu_counter_t *start,
                          const telemetry_cpu_counter_t *end, unsigned cores,
                          telemetry_cpu_t *result)
{
    if (!cores || cores > TELEMETRY_MAX_CORES) return false;
    telemetry_cpu_t sample = { .state = TELEMETRY_OK, .cores = cores };
    uint64_t elapsed_sum = 0, busy_sum = 0;
    for (unsigned core = 0; core < cores; ++core) {
        if (end[core].time_us < start[core].time_us || end[core].idle_us < start[core].idle_us)
            return false;
        uint64_t elapsed = end[core].time_us - start[core].time_us;
        uint64_t idle = end[core].idle_us - start[core].idle_us;
        if (elapsed < TELEMETRY_CPU_MIN_WINDOW_US || elapsed > TELEMETRY_CPU_MAX_WINDOW_US)
            return false;
        /* Timer and idle task counters are read a few microseconds apart. */
        if (idle > elapsed + UINT64_C(1000)) return false;
        uint64_t busy = idle < elapsed ? elapsed - idle : 0;
        sample.core_pct[core] = 100.0 * (double)busy / (double)elapsed;
        elapsed_sum += elapsed;
        busy_sum += busy;
    }
    sample.used_pct = 100.0 * (double)busy_sum / (double)elapsed_sum;
    sample.window_ms = (unsigned)(elapsed_sum / cores / 1000);
    *result = sample;
    return true;
}

#ifdef ESP_PLATFORM
#include <math.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 && \
    CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP
#define HAVE_IDLE_COUNTERS 1
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#include "esp_ipc.h"
#endif
#endif

#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

#define TELEMETRY_FRESH_US UINT64_C(7500000)

static portMUX_TYPE s_monitor_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_monitor_running;
static uint64_t s_last_request_us;
static telemetry_cpu_t s_cpu;
static uint64_t s_cpu_sample_us;
static telemetry_state_t s_temperature_state;
static float s_temperature_c;
static uint64_t s_temperature_sample_us;

#if HAVE_IDLE_COUNTERS
_Static_assert(CONFIG_FREERTOS_NUMBER_OF_CORES <= TELEMETRY_MAX_CORES, "unsupported core count");

static void capture_current_core(void *arg)
{
    telemetry_cpu_counter_t *counter = arg;
    uint64_t before = (uint64_t)esp_timer_get_time();
    counter->idle_us = ulTaskGetIdleRunTimeCounterForCore(xPortGetCoreID());
    counter->time_us = (uint64_t)esp_timer_get_time();
    if (counter->time_us - before > UINT64_C(1000)) counter->time_us = 0;
}

static bool telemetry_capture_core(unsigned core, telemetry_cpu_counter_t *counter)
{
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    /* FreeRTOS records idle time on a context switch. Run the callback in
     * each core's IPC task so that even uninterrupted idle time is recorded. */
    if (esp_ipc_call_blocking(core, capture_current_core, counter) != ESP_OK) return false;
#else
    (void)core;
    capture_current_core(counter);
#endif
    return counter->time_us != 0;
}

static bool capture_cpu(telemetry_cpu_counter_t *current)
{
    for (unsigned core = 0; core < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core) {
        if (!telemetry_capture_core(core, &current[core])) return false;
    }
    return current[CONFIG_FREERTOS_NUMBER_OF_CORES - 1].time_us - current[0].time_us <= UINT64_C(25000);
}
#endif

#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
static telemetry_state_t take_temperature(temperature_sensor_handle_t *sensor, float *celsius)
{
    if (!*sensor) {
        /* The initial range has the best accuracy at typical die temperatures.
         * ESP-IDF switches to a wider range when the die leaves this band. */
        const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t installed = temperature_sensor_install(&config, sensor);
        if (installed == ESP_ERR_NOT_SUPPORTED) return TELEMETRY_UNSUPPORTED;
        if (installed != ESP_OK) return TELEMETRY_UNAVAILABLE;
    }
    if (temperature_sensor_enable(*sensor) != ESP_OK) return TELEMETRY_UNAVAILABLE;
    esp_err_t reading = temperature_sensor_get_celsius(*sensor, celsius);
    if (temperature_sensor_disable(*sensor) != ESP_OK) return TELEMETRY_UNAVAILABLE;
    if (reading == ESP_ERR_INVALID_STATE && isfinite(*celsius)) return TELEMETRY_OUT_OF_RANGE;
    if (reading == ESP_OK && isfinite(*celsius)) return TELEMETRY_OK;
    return TELEMETRY_UNAVAILABLE;
}
#endif

static void monitor_task(void *arg)
{
    (void)arg;
    uint64_t due = (uint64_t)esp_timer_get_time();
#if HAVE_IDLE_COUNTERS
    telemetry_cpu_counter_t previous[CONFIG_FREERTOS_NUMBER_OF_CORES] = {0};
#endif
#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_handle_t sensor = NULL;
#endif
    for (;;) {
        uint64_t now = (uint64_t)esp_timer_get_time();
        taskENTER_CRITICAL(&s_monitor_lock);
        uint64_t last_request = s_last_request_us;
        taskEXIT_CRITICAL(&s_monitor_lock);
        uint64_t wait_us;
        telemetry_monitor_event_t event = telemetry_monitor_event(now, due, last_request, &wait_us);
        if (event == TELEMETRY_MONITOR_EXPIRE) {
#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
            if (sensor && temperature_sensor_uninstall(sensor) == ESP_OK) sensor = NULL;
#endif
            taskENTER_CRITICAL(&s_monitor_lock);
            bool expired = now >= s_last_request_us + TELEMETRY_LEASE_US;
            if (expired) s_monitor_running = false;
            taskEXIT_CRITICAL(&s_monitor_lock);
            if (expired) break;
            continue;
        }
        if (event == TELEMETRY_MONITOR_WAIT) {
            TickType_t delay = pdMS_TO_TICKS((wait_us + 999) / 1000);
            vTaskDelay(delay ? delay : 1);
            continue;
        }

        telemetry_cpu_t cpu = { .state = TELEMETRY_UNAVAILABLE };
        uint64_t cpu_sample_us = 0;
#if HAVE_IDLE_COUNTERS
        telemetry_cpu_counter_t current[CONFIG_FREERTOS_NUMBER_OF_CORES];
        if (capture_cpu(current)) {
            if (!previous[0].time_us) cpu.state = TELEMETRY_WARMING_UP;
            else if (telemetry_cpu_window(previous, current, CONFIG_FREERTOS_NUMBER_OF_CORES, &cpu))
                cpu_sample_us = current[CONFIG_FREERTOS_NUMBER_OF_CORES - 1].time_us;
            for (unsigned core = 0; core < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core)
                previous[core] = current[core];
        } else {
            previous[0].time_us = 0;
        }
#endif

        float celsius = NAN;
        telemetry_state_t thermal = TELEMETRY_UNSUPPORTED;
#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
        thermal = take_temperature(&sensor, &celsius);
#endif
        uint64_t thermal_sample_us = (uint64_t)esp_timer_get_time();
        taskENTER_CRITICAL(&s_monitor_lock);
        if (cpu.state != TELEMETRY_WARMING_UP || s_cpu.state != TELEMETRY_OK) {
            s_cpu = cpu;
            s_cpu_sample_us = cpu_sample_us;
        }
        s_temperature_state = thermal;
        s_temperature_c = celsius;
        s_temperature_sample_us = thermal_sample_us;
        taskEXIT_CRITICAL(&s_monitor_lock);

        /* Keep the phase independent of request and measurement time. */
        due = telemetry_next_sample(due, thermal_sample_us);
    }
    vTaskDelete(NULL);
}

void telemetry_read(telemetry_snapshot_t *snapshot)
{
    *snapshot = (telemetry_snapshot_t){0};
    uint64_t now = (uint64_t)esp_timer_get_time();
    taskENTER_CRITICAL(&s_monitor_lock);
    s_last_request_us = now;
    bool start = !s_monitor_running;
    if (start) {
        s_monitor_running = true;
#if HAVE_IDLE_COUNTERS
        if (s_cpu.state != TELEMETRY_OK) s_cpu.state = TELEMETRY_WARMING_UP;
#endif
#if !CONFIG_SOC_TEMP_SENSOR_SUPPORTED
        s_temperature_state = TELEMETRY_UNSUPPORTED;
#endif
    }
    taskEXIT_CRITICAL(&s_monitor_lock);
    if (start && xTaskCreate(monitor_task, "telemetry", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_monitor_lock);
        s_monitor_running = false;
        s_cpu.state = TELEMETRY_UNAVAILABLE;
        taskEXIT_CRITICAL(&s_monitor_lock);
    }

    uint64_t cpu_sample_us, thermal_sample_us;
    taskENTER_CRITICAL(&s_monitor_lock);
    snapshot->cpu = s_cpu;
    snapshot->temperature_state = s_temperature_state;
    snapshot->temperature_c = s_temperature_c;
    cpu_sample_us = s_cpu_sample_us;
    thermal_sample_us = s_temperature_sample_us;
    taskEXIT_CRITICAL(&s_monitor_lock);
    now = (uint64_t)esp_timer_get_time();
    if (snapshot->cpu.state == TELEMETRY_OK) {
        if (now - cpu_sample_us > TELEMETRY_FRESH_US) snapshot->cpu.state = TELEMETRY_WARMING_UP;
        else snapshot->cpu.age_ms = (unsigned)((now - cpu_sample_us) / 1000);
    }
    if (snapshot->temperature_state == TELEMETRY_OK) {
        if (now - thermal_sample_us > TELEMETRY_FRESH_US) snapshot->temperature_state = TELEMETRY_UNAVAILABLE;
        else snapshot->temperature_age_ms = (unsigned)((now - thermal_sample_us) / 1000);
    }
    snapshot->uptime_s = now / UINT64_C(1000000);
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    snapshot->heap_free_bytes = heap_caps_get_free_size(internal);
    snapshot->heap_total_bytes = heap_caps_get_total_size(internal);
    snapshot->heap_min_free_bytes = heap_caps_get_minimum_free_size(internal);
    snapshot->psram_free_bytes = heap_caps_get_free_size(external);
    snapshot->psram_total_bytes = heap_caps_get_total_size(external);
}
#endif
