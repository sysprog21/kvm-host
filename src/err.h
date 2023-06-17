#pragma once

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

static inline int throw_err(const char *str, ...)
{
    va_list arg;
    va_start(arg, str);
    vfprintf(stderr, str, arg);
    fprintf(stderr, " (errno=%d)\n", errno);
    va_end(arg);
    return -1;
}
