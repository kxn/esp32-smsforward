#include "../feishu_config_policy.h"

int main(void)
{
    /* First boot restores stored peer IDs even though static globals were empty. */
    if (feishu_credentials_changed(false, "", "", "cli_a", "secret_a")) return 1;
    if (feishu_credentials_changed(true, "cli_a", "secret_a", "cli_a", "secret_a")) return 2;
    /* App changes and secret rotations invalidate the old recipient. */
    if (!feishu_credentials_changed(true, "cli_a", "secret_a", "cli_b", "secret_a")) return 3;
    if (!feishu_credentials_changed(true, "cli_a", "secret_a", "cli_a", "secret_b")) return 4;
    return 0;
}
