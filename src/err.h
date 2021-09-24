#pragma once

#include <errno.h>
#include <stdio.h>

static inline int throw_err(const char *str)
{
    fprintf(stderr, "%s (errno=%d)\n", str, errno);
    return -1;
}
