/*
 * lua_runtime.h - Lua game runtime with citro2d graphics
 *
 * Provides a sandboxed Lua environment for running games created by Claude.
 * Games define update() and draw() functions and run until SELECT is pressed.
 */

#pragma once

#include <stddef.h>

int lua_runtime_init(void);
void lua_runtime_exit(void);

/*
 * Run a Lua script as a game.
 * script_path: Relative path within workdir (e.g., "pong.lua")
 * Returns 1 on success, 0 on error (fills error buffer with message)
 */
int lua_runtime_run(const char *script_path, char *error, size_t error_size);
