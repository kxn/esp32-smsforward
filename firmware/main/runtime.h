#pragma once
#include <stdbool.h>

/* Wake the network manager after a local configuration update. */
void network_config_changed(void);
/* True while boot recovery intentionally suppresses normal radio workers. */
bool device_local_recovery_mode(void);
