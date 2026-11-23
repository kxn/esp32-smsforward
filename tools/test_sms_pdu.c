#include <stdio.h>
#include "../firmware/main/pdu.h"

int main(void)
{
    char report[4096] = {0};
    int failed = pdu_selftest(report, sizeof(report));
    fputs(report, stdout);
    return failed ? 1 : 0;
}
