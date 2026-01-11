#include "lua_runtime.h"

#include "app_config.h"
#include "debug_log.h"
#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <lua5.1/lua.h>
#include <lua5.1/lauxlib.h>
#include <lua5.1/lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lua_State *L = NULL;
static C3D_RenderTarget *top_target = NULL;
static C3D_RenderTarget *bot_target = NULL;
static int game_running = 0;

static u32 hex_to_c2d(u32 hex) {
  u8 r = (hex >> 16) & 0xFF;
  u8 g = (hex >> 8) & 0xFF;
  u8 b = hex & 0xFF;
  return C2D_Color32(r, g, b, 255);
}

static int lua_clear(lua_State *L) {
  u32 color = luaL_optinteger(L, 1, 0x000000);
  C2D_TargetClear(top_target, hex_to_c2d(color));
  return 0;
}

static int lua_rect(lua_State *L) {
  float x = luaL_checknumber(L, 1);
  float y = luaL_checknumber(L, 2);
  float w = luaL_checknumber(L, 3);
  float h = luaL_checknumber(L, 4);
  u32 color = luaL_optinteger(L, 5, 0xFFFFFF);
  C2D_DrawRectSolid(x, y, 0, w, h, hex_to_c2d(color));
  return 0;
}

static int lua_circle(lua_State *L) {
  float x = luaL_checknumber(L, 1);
  float y = luaL_checknumber(L, 2);
  float r = luaL_checknumber(L, 3);
  u32 color = luaL_optinteger(L, 4, 0xFFFFFF);
  C2D_DrawCircleSolid(x, y, 0, r, hex_to_c2d(color));
  return 0;
}

static int lua_line(lua_State *L) {
  float x1 = luaL_checknumber(L, 1);
  float y1 = luaL_checknumber(L, 2);
  float x2 = luaL_checknumber(L, 3);
  float y2 = luaL_checknumber(L, 4);
  float thick = luaL_optnumber(L, 5, 1.0);
  u32 color = luaL_optinteger(L, 6, 0xFFFFFF);
  C2D_DrawLine(x1, y1, hex_to_c2d(color), x2, y2, hex_to_c2d(color), thick, 0);
  return 0;
}

static C2D_TextBuf g_textBuf = NULL;
static C2D_Font g_font = NULL;

static int lua_text(lua_State *L) {
  float x = luaL_checknumber(L, 1);
  float y = luaL_checknumber(L, 2);
  const char *str = luaL_checkstring(L, 3);
  float size = luaL_optnumber(L, 4, 0.5);
  u32 color = luaL_optinteger(L, 5, 0xFFFFFF);
  
  // Auto-convert pixel sizes to scale factor if value seems too large
  if (size > 10.0f) {
    size = size / 24.0f;
  }
  if (size < 0.1f) size = 0.1f;
  if (size > 3.0f) size = 3.0f;
  
  if (!g_textBuf) {
    g_textBuf = C2D_TextBufNew(4096);
  }
  C2D_TextBufClear(g_textBuf);
  
  C2D_Text text;
  C2D_TextParse(&text, g_textBuf, str);
  C2D_TextOptimize(&text);
  C2D_DrawText(&text, C2D_WithColor, x, y, 0, size, size, hex_to_c2d(color));
  return 0;
}

static u32 keys_held = 0;
static u32 keys_down = 0;

static int lua_key_down(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  u32 key = 0;
  
  if (strcmp(name, "a") == 0 || strcmp(name, "A") == 0) key = KEY_A;
  else if (strcmp(name, "b") == 0 || strcmp(name, "B") == 0) key = KEY_B;
  else if (strcmp(name, "x") == 0 || strcmp(name, "X") == 0) key = KEY_X;
  else if (strcmp(name, "y") == 0 || strcmp(name, "Y") == 0) key = KEY_Y;
  else if (strcmp(name, "l") == 0 || strcmp(name, "L") == 0) key = KEY_L;
  else if (strcmp(name, "r") == 0 || strcmp(name, "R") == 0) key = KEY_R;
  else if (strcmp(name, "up") == 0) key = KEY_UP;
  else if (strcmp(name, "down") == 0) key = KEY_DOWN;
  else if (strcmp(name, "left") == 0) key = KEY_LEFT;
  else if (strcmp(name, "right") == 0) key = KEY_RIGHT;
  else if (strcmp(name, "start") == 0) key = KEY_START;
  else if (strcmp(name, "select") == 0) key = KEY_SELECT;
  
  lua_pushboolean(L, (keys_down & key) != 0);
  return 1;
}

static int lua_key_held(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  u32 key = 0;
  
  if (strcmp(name, "a") == 0 || strcmp(name, "A") == 0) key = KEY_A;
  else if (strcmp(name, "b") == 0 || strcmp(name, "B") == 0) key = KEY_B;
  else if (strcmp(name, "x") == 0 || strcmp(name, "X") == 0) key = KEY_X;
  else if (strcmp(name, "y") == 0 || strcmp(name, "Y") == 0) key = KEY_Y;
  else if (strcmp(name, "l") == 0 || strcmp(name, "L") == 0) key = KEY_L;
  else if (strcmp(name, "r") == 0 || strcmp(name, "R") == 0) key = KEY_R;
  else if (strcmp(name, "up") == 0) key = KEY_UP;
  else if (strcmp(name, "down") == 0) key = KEY_DOWN;
  else if (strcmp(name, "left") == 0) key = KEY_LEFT;
  else if (strcmp(name, "right") == 0) key = KEY_RIGHT;
  else if (strcmp(name, "start") == 0) key = KEY_START;
  else if (strcmp(name, "select") == 0) key = KEY_SELECT;
  
  lua_pushboolean(L, (keys_held & key) != 0);
  return 1;
}

static int lua_quit(lua_State *L) {
  (void)L;
  game_running = 0;
  return 0;
}

static int lua_screen_width(lua_State *L) {
  lua_pushinteger(L, 400);
  return 1;
}

static int lua_screen_height(lua_State *L) {
  lua_pushinteger(L, 240);
  return 1;
}

static int lua_random(lua_State *L) {
  int min = luaL_checkinteger(L, 1);
  int max = luaL_checkinteger(L, 2);
  if (max < min) { int t = min; min = max; max = t; }
  lua_pushinteger(L, min + (rand() % (max - min + 1)));
  return 1;
}

static void register_lua_api(lua_State *L) {
  lua_register(L, "clear", lua_clear);
  lua_register(L, "rect", lua_rect);
  lua_register(L, "circle", lua_circle);
  lua_register(L, "line", lua_line);
  lua_register(L, "text", lua_text);
  
  lua_register(L, "key_down", lua_key_down);
  lua_register(L, "key_held", lua_key_held);
  
  lua_register(L, "quit", lua_quit);
  lua_register(L, "screen_width", lua_screen_width);
  lua_register(L, "screen_height", lua_screen_height);
  lua_register(L, "random", lua_random);
}

static int graphics_initialized = 0;

int lua_runtime_init(void) {
  return 1;
}

void lua_runtime_exit(void) {
  if (graphics_initialized) {
    if (g_textBuf) {
      C2D_TextBufDelete(g_textBuf);
      g_textBuf = NULL;
    }
    C2D_Fini();
    C3D_Fini();
    graphics_initialized = 0;
  }
}

static void init_graphics(void) {
  if (!graphics_initialized) {
    debug_log("init_graphics: starting");
    
    consoleClear();
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    
    debug_log("init_graphics: calling gfxExit");
    gfxExit();
    
    debug_log("init_graphics: calling gfxInitDefault");
    gfxInitDefault();
    gfxSet3D(false);
    
    debug_log("init_graphics: calling C3D_Init");
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    
    debug_log("init_graphics: calling C2D_Init");
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    
    debug_log("init_graphics: creating render targets");
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    graphics_initialized = 1;
    
    debug_log("init_graphics: clearing screens");
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_target, C2D_Color32(0, 0, 0, 255));
    C2D_TargetClear(bot_target, C2D_Color32(0, 0, 0, 255));
    C3D_FrameEnd(0);
    
    debug_log("init_graphics: done");
  }
}

static void deinit_graphics(void) {
  if (graphics_initialized) {
    debug_log("deinit_graphics: starting");
    
    if (g_textBuf) {
      debug_log("deinit_graphics: deleting text buffer");
      C2D_TextBufDelete(g_textBuf);
      g_textBuf = NULL;
    }
    
    debug_log("deinit_graphics: calling C2D_Fini");
    C2D_Fini();
    
    debug_log("deinit_graphics: calling C3D_Fini");
    C3D_Fini();
    
    debug_log("deinit_graphics: calling gfxExit");
    gfxExit();
    
    debug_log("deinit_graphics: calling gfxInitDefault");
    gfxInitDefault();
    
    debug_log("deinit_graphics: calling ui_reinit_console");
    ui_reinit_console();
    graphics_initialized = 0;
    
    debug_log("deinit_graphics: done");
  }
}

int lua_runtime_run(const char *script_path, char *error, size_t error_size) {
  debug_log("lua_runtime_run: starting with script=%s", script_path);
  
  char full_path[512];
  snprintf(full_path, sizeof(full_path), "%s/%s", NINTENCODE_WORKDIR, script_path);
  debug_log("lua_runtime_run: full_path=%s", full_path);
  
  debug_log("lua_runtime_run: creating Lua state");
  L = luaL_newstate();
  if (!L) {
    snprintf(error, error_size, "Failed to create Lua state");
    debug_log("lua_runtime_run: FAILED to create Lua state");
    return 0;
  }
  
  debug_log("lua_runtime_run: opening libs");
  luaL_openlibs(L);
  
  debug_log("lua_runtime_run: registering API");
  register_lua_api(L);
  
  debug_log("lua_runtime_run: loading script");
  if (luaL_loadfile(L, full_path) != 0) {
    snprintf(error, error_size, "Load error: %s", lua_tostring(L, -1));
    debug_log("lua_runtime_run: load error: %s", lua_tostring(L, -1));
    lua_close(L);
    L = NULL;
    return 0;
  }
  
  debug_log("lua_runtime_run: executing script");
  if (lua_pcall(L, 0, 0, 0) != 0) {
    snprintf(error, error_size, "Run error: %s", lua_tostring(L, -1));
    debug_log("lua_runtime_run: run error: %s", lua_tostring(L, -1));
    lua_close(L);
    L = NULL;
    return 0;
  }
  
  debug_log("lua_runtime_run: checking for update/draw functions");
  lua_getglobal(L, "update");
  int has_update = lua_isfunction(L, -1);
  lua_pop(L, 1);
  
  lua_getglobal(L, "draw");
  int has_draw = lua_isfunction(L, -1);
  lua_pop(L, 1);
  
  debug_log("lua_runtime_run: has_update=%d has_draw=%d", has_update, has_draw);
  
  if (!has_update && !has_draw) {
    snprintf(error, error_size, "Script must have update() or draw()");
    debug_log("lua_runtime_run: no update or draw function");
    lua_close(L);
    L = NULL;
    return 0;
  }
  
  debug_log("lua_runtime_run: calling init_graphics");
  init_graphics();
  
  debug_log("lua_runtime_run: entering game loop");
  game_running = 1;
  int frame_count = 0;
  
  while (game_running && aptMainLoop()) {
    hidScanInput();
    keys_held = hidKeysHeld();
    keys_down = hidKeysDown();
    
    if (keys_down & KEY_SELECT) {
      debug_log("lua_runtime_run: SELECT pressed, exiting");
      game_running = 0;
      break;
    }
    
    if (has_update) {
      lua_getglobal(L, "update");
      if (lua_pcall(L, 0, 0, 0) != 0) {
        snprintf(error, error_size, "update() error: %s", lua_tostring(L, -1));
        debug_log("lua_runtime_run: update error: %s", lua_tostring(L, -1));
        game_running = 0;
        break;
      }
    }
    
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_SceneBegin(top_target);
    
    C2D_TargetClear(top_target, C2D_Color32(0, 0, 0, 255));
    
    if (has_draw) {
      lua_getglobal(L, "draw");
      if (lua_pcall(L, 0, 0, 0) != 0) {
        snprintf(error, error_size, "draw() error: %s", lua_tostring(L, -1));
        debug_log("lua_runtime_run: draw error: %s", lua_tostring(L, -1));
        game_running = 0;
        break;
      }
    }
    
    C2D_SceneBegin(bot_target);
    C2D_TargetClear(bot_target, C2D_Color32(32, 32, 32, 255));
    
    if (!g_textBuf) {
      g_textBuf = C2D_TextBufNew(4096);
    }
    C2D_TextBufClear(g_textBuf);
    C2D_Text hint;
    C2D_TextParse(&hint, g_textBuf, "Press SELECT to return to chat");
    C2D_TextOptimize(&hint);
    C2D_DrawText(&hint, C2D_WithColor, 50, 110, 0, 0.5, 0.5, C2D_Color32(200, 200, 200, 255));
    
    C3D_FrameEnd(0);
    
    frame_count++;
    if (frame_count == 1) {
      debug_log("lua_runtime_run: first frame rendered successfully");
    }
  }
  
  debug_log("lua_runtime_run: exited game loop after %d frames", frame_count);
  
  debug_log("lua_runtime_run: calling deinit_graphics");
  deinit_graphics();
  
  debug_log("lua_runtime_run: deinit_graphics returned");
  
  int had_error = (error[0] != '\0');
  debug_log("lua_runtime_run: had_error=%d", had_error);
  
  lua_close(L);
  L = NULL;
  
  debug_log("lua_runtime_run: returning %d", had_error ? 0 : 1);
  return had_error ? 0 : 1;
}
