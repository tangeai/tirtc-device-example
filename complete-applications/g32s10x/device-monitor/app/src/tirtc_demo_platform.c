#include <driver/dtrng.h>

long HAL_Random(void)
{
    return (long)dtrng_read_random_data();
}
