#pragma once
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void fake_critical_enter(portMUX_TYPE *mux);
void fake_critical_exit(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) fake_critical_enter(mux)
#define taskEXIT_CRITICAL(mux) fake_critical_exit(mux)
