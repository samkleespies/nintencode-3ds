/*
 * tools.h - Claude tool implementations
 *
 * Provides file operations and Lua execution that Claude can invoke.
 * All file operations are sandboxed to the workdir.
 */

#pragma once

#include <stddef.h>

int tools_init(char *error, size_t error_size);
int tool_execute(const char *name, const char *input_json,
                 char *output, size_t output_size);
