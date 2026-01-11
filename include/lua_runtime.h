#pragma once

#include <stddef.h>

int lua_runtime_init(void);
void lua_runtime_exit(void);

// Run a Lua script from the workdir. Returns 1 on success, 0 on error.
int lua_runtime_run(const char *script_path, char *error, size_t error_size);
