#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../firmware/main/phone_region.h"

static void expect(const char *number, const char *region)
{
    const char *actual = phone_region_lookup(number);
    if (region) {
        assert(actual && strcmp(actual, region) == 0);
    } else {
        assert(actual == NULL);
    }
}

int main(void)
{
    expect("13213000000", "河南郑州");
    expect("13213009999", "河南郑州");
    expect("+8613213000000", "河南郑州");
    expect("008613213000000", "河南郑州");
    expect("8613213000000", "河南郑州");
    expect("13800138000", "北京");
    expect("16600000000", "北京");
    expect("19000030000", "河南濮阳");
    expect("19000000000", NULL);

    expect(NULL, NULL);
    expect("", NULL);
    expect("10086", NULL);
    expect("01012345678", NULL);
    expect("+85213213000000", NULL);
    expect("1321300000", NULL);
    expect("132130000000", NULL);
    expect("1321300000x", NULL);
    expect("1321300000 ", NULL);
    expect("132 13000000", NULL);
    expect("8132130000000", NULL);
    expect("0086132130000000", NULL);
    return 0;
}
