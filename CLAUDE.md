# Nintencode 3DS - Development Guide

This is a Nintendo 3DS homebrew application that provides an AI coding assistant powered by Claude. The standout feature is that Claude can write and run Lua games directly on the 3DS hardware.

## Project Structure

```
nintencode-3ds/
  source/           - C source files
    main.c          - App entry point, chat loop, API communication
    ui.c            - Console-based chat UI using PrintConsole
    net.c           - Network layer using libcurl with mbedTLS
    tools.c         - Tool implementations (file ops, run_lua)
    lua_runtime.c   - Lua interpreter with citro2d graphics bindings
    json.c          - JSMN-based JSON parser and helpers
  include/          - Header files
    app_config.h    - API configuration (key, model, paths)
    app_types.h     - Chat message types and history structs
    debug_log.h     - SD card debug logging
  proxy/            - Node.js debug proxy for development
  romfs/            - Empty, reserved for bundled assets
  Makefile          - DevkitPro build configuration
```

## Building

Requires devkitPro with 3DS toolchain installed:

```bash
source /etc/profile.d/devkit-env.sh
make clean && make
```

This produces `Nintencode.3dsx` which can be run via Homebrew Launcher.

## Configuration

Before building, edit `include/app_config.h`:

- `NINTENCODE_API_KEY` - Your Anthropic API key
- `NINTENCODE_USE_PROXY` - Set to 1 to route through debug proxy
- `NINTENCODE_MODEL_ID` - Claude model to use

## Architecture Notes

### Display Modes

The 3DS cannot run console mode and citro2d graphics simultaneously. The app switches between modes:

1. **Console mode** - Used for chat UI. Uses `gfxInitDefault()` and `PrintConsole`.
2. **Graphics mode** - Used when running Lua games. Uses `C3D_Init()` and `C2D_Init()`.

When transitioning from graphics back to console, the app must:
- Call `C2D_Fini()` and `C3D_Fini()`
- Call `gfxExit()` then `gfxInitDefault()`
- Reinitialize the console with `ui_reinit_console()`

### Tool System

Claude has access to these tools:
- `read_file` - Read files from workdir with pagination
- `write_file` - Write files up to 32KB
- `list_files` - Directory listing
- `file_info` - File size info
- `create_directory` - Create directories
- `run_lua` - Execute a Lua script as a game

All file operations are sandboxed to `sdmc:/nintencode/workdir/`.

### Lua Graphics API

Games must define `update()` and/or `draw()` functions. Available APIs:

Graphics:
- `clear(color)` - Fill screen with hex color
- `rect(x, y, w, h, color)` - Filled rectangle
- `circle(x, y, r, color)` - Filled circle
- `line(x1, y1, x2, y2, thickness, color)` - Line
- `text(x, y, str, size, color)` - Text (size is scale, 0.5-2.0)

Input:
- `key_held(name)` - Check if button is held
- `key_down(name)` - Check if button was just pressed

Helpers:
- `screen_width()` / `screen_height()` - Returns 400x240
- `random(min, max)` - Random integer
- `quit()` - Exit game

Users press SELECT to exit games and return to chat.

## Known Issues

### Message History Structure

The API expects tool_use and tool_result blocks to be properly paired within messages. The current implementation creates separate messages for each, which can cause "unexpected tool_use_id" errors when the history gets long.

### Empty Tool Input

Claude occasionally sends empty tool input (`{}`). The code handles this gracefully but Claude may retry.

## Debug Logging

Debug logs are written to `sdmc:/nintencode/debug.log` on the SD card. The logging is enabled via `debug_log()` calls throughout the code.

## Testing Workflow

1. Build and deploy: `make && curl -T Nintencode.3dsx ftp://[3DS_IP]:5000/3ds/Nintencode.3dsx`
2. Launch Nintencode on 3DS
3. Press A to open keyboard, type prompt
4. Claude responds and can create/run games
5. Press SELECT to exit games, START to exit app
6. Check `sdmc:/nintencode/debug.log` for troubleshooting

## Proxy Development

For debugging API communication, use the debug proxy:

```bash
cd proxy
node debug-proxy.js
```

Set `NINTENCODE_USE_PROXY 1` in app_config.h and update the proxy IP.
Logs go to `/tmp/nintencode-debug.log`.
