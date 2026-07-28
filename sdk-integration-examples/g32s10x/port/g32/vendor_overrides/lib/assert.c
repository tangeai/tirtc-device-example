#include <assert.h>
#include <stdint.h>
#include "print.h"

void hang(void)
{
    uintptr_t return_address = (uintptr_t)__builtin_return_address(0);

    printf("hang entered! return_address:0x%08lx\n",
           (unsigned long)return_address);
    __builtin_trap();
    (*(volatile unsigned int *)0) = 0;
    while (1);
}

void __assert(const char *file, int line, const char *expr)
{
    printf("assert %s failed in %s %d\n", expr, file, line);
    hang();
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    printf("assert %s failed in %s %d %s\n", expr, file, line, func);
    hang();
}

void __assert_no_file(const char *func, int line, const char *expr)
{
    printf("assert %s failed in %s %d\n", expr, func, line);
    hang();
}
