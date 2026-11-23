#include "../cmd_policy.h"

int main(void)
{
    if (cmd_policy_cereg_stat("+CEREG: 2,1,\"1A2B\",\"03CD\",7\r\nOK\r\n") != 1) return 1;
    if (cmd_policy_cereg_stat("+CEREG: 2,5,\"1A2B\",\"03CD\",7\r\nOK\r\n") != 5) return 2;
    if (cmd_policy_cereg_stat("+CEREG: 0,0\r\nOK\r\n") != 0) return 3;
    if (cmd_policy_cereg_stat("+CEREG: 5\r\nOK\r\n") != 5) return 4;
    if (cmd_policy_cereg_stat("+CEREG: 2,,\"1A2B\",\"03CD\"\r\n") != -1) return 5;
    if (cmd_policy_cereg_stat("OK\r\n") != -1) return 6;
    return 0;
}
