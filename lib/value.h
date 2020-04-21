#pragma once

#include "scanner.h"
#include "value.h"
#include "varlink.h"

#include <stdio.h>

typedef struct {
        VarlinkValueKind kind;
        union {
                bool b;
                int64_t i;
                double f;
                char *s;
                VarlinkArray *array;
                VarlinkObject *object;
        };
} VarlinkValue;

long varlink_value_read_from_scanner(VarlinkValue *value, Scanner *scanner);
long varlink_value_write_json(VarlinkValue *value,
                              FILE *stream,
                              long indent,
                              const char *key_pre, const char *key_post,
                              const char *value_pre, const char *value_post);

void varlink_value_clear(VarlinkValue *value);
