#pragma once

#include <stddef.h>

int tools_init(char *error, size_t error_size);
int tool_execute(const char *name, const char *input_json, char *output, size_t output_size);
