#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_types.h"
#include "debug_log.h"
#include "json.h"
#include "lua_runtime.h"
#include "net.h"
#include "tools.h"
#include "ui.h"

#define REQUEST_BUFFER_SIZE 131072
#define RESPONSE_BUFFER_SIZE 131072
#define TOOL_BUFFER_SIZE 8192
#define MAX_TOOL_CALLS 8
#define MAX_TOOL_LOOPS 20
#define MAX_TOOL_INPUT_EXEC 32768

typedef struct {
  char id[MAX_TOOL_ID];
  char name[MAX_TOOL_NAME];
  char *input;
} ToolUse;

static const char *SYSTEM_PROMPT = 
  "You are Nintencode, an AI coding assistant on Nintendo 3DS. "
  "You can read/write files and CREATE PLAYABLE GAMES using run_lua. "
  "IMPORTANT: When asked to make a game, IMMEDIATELY write the .lua file, then call run_lua. Do NOT explain first. "
  "Lua Graphics API: "
  "clear(color) - fill screen with color. "
  "rect(x,y,w,h,color) - draw filled rectangle. "
  "circle(x,y,r,color) - draw filled circle. "
  "line(x1,y1,x2,y2,thickness,color) - draw line. "
  "text(x,y,str,size,color) - draw text, size is scale factor (use 0.5-1.0 for normal text, 1.5-2.0 for large). "
  "Lua Input API: "
  "key_held(name) - true if key held (a,b,x,y,l,r,up,down,left,right,start,select). "
  "key_down(name) - true if key just pressed this frame. "
  "Helpers: screen_width()=400, screen_height()=240, random(min,max), quit(). "
  "Colors are hex: 0xRRGGBB (e.g. 0xFF0000=red, 0x00FF00=green, 0xFFFFFF=white). "
  "Games must define update() and/or draw() functions. User presses SELECT to exit. "
  "Be concise. Execute tools immediately without lengthy explanations.";

static const char *TOOLS_JSON = "["
  "{\"name\":\"read_file\",\"description\":\"Read a file from the working directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"write_file\",\"description\":\"Write a file in the working directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}},"
  "{\"name\":\"list_files\",\"description\":\"List files in a directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}},"
  "{\"name\":\"file_info\",\"description\":\"Show file size information\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"create_directory\",\"description\":\"Create a directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"run_lua\",\"description\":\"Run a Lua game/script. The script must define update() and/or draw() functions. User presses SELECT to exit.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"script\":{\"type\":\"string\",\"description\":\"Path to .lua file to run\"}},\"required\":[\"script\"]}}"
  "]";

static ChatHistory g_history;

static void history_init(ChatHistory *history) {
  history->count = 0;
}

static void history_shift(ChatHistory *history) {
  if (history->count < MAX_MESSAGES) {
    return;
  }
  for (int i = 1; i < MAX_MESSAGES; i++) {
    history->items[i - 1] = history->items[i];
  }
  history->count = MAX_MESSAGES - 1;
}

static void history_add(ChatHistory *history, MessageRole role, MessageKind kind, const char *text, const char *tool_id, const char *tool_name, const char *tool_input) {
  history_shift(history);
  ChatMessage *msg = &history->items[history->count++];
  memset(msg, 0, sizeof(ChatMessage));
  msg->role = role;
  msg->kind = kind;
  if (text) {
    strncpy(msg->text, text, sizeof(msg->text) - 1);
  }
  if (tool_id) {
    strncpy(msg->tool_use_id, tool_id, sizeof(msg->tool_use_id) - 1);
  }
  if (tool_name) {
    strncpy(msg->tool_name, tool_name, sizeof(msg->tool_name) - 1);
  }
  if (tool_input) {
    strncpy(msg->tool_input, tool_input, sizeof(msg->tool_input) - 1);
  }
}

static void json_escape(const char *input, char *out, size_t out_size) {
  size_t o = 0;
  for (size_t i = 0; input[i] != '\0' && o + 2 < out_size; i++) {
    char c = input[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = c;
    } else if (c == '\n') {
      out[o++] = '\\';
      out[o++] = 'n';
    } else if (c == '\r') {
      out[o++] = '\\';
      out[o++] = 'r';
    } else if (c == '\t') {
      out[o++] = '\\';
      out[o++] = 't';
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

static int append_json(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
  size_t len = strlen(text);
  if (*offset + len + 1 >= buffer_size) {
    return 0;
  }
  memcpy(buffer + *offset, text, len);
  *offset += len;
  buffer[*offset] = '\0';
  return 1;
}

static int build_request_json(const ChatHistory *history, char *out, size_t out_size) {
  char escaped_system[2048];
  json_escape(SYSTEM_PROMPT, escaped_system, sizeof(escaped_system));

  size_t offset = 0;
  out[0] = '\0';
  if (!append_json(out, out_size, &offset, "{\"model\":\"")) return 0;
  if (!append_json(out, out_size, &offset, NINTENCODE_MODEL_ID)) return 0;
  if (!append_json(out, out_size, &offset, "\",\"max_tokens\":")) return 0;
  char max_tokens_buf[16];
  snprintf(max_tokens_buf, sizeof(max_tokens_buf), "%d", NINTENCODE_MAX_TOKENS);
  if (!append_json(out, out_size, &offset, max_tokens_buf)) return 0;
  if (!append_json(out, out_size, &offset, ",\"system\":\"")) return 0;
  if (!append_json(out, out_size, &offset, escaped_system)) return 0;
  if (!append_json(out, out_size, &offset, "\",\"tools\":")) return 0;
  if (!append_json(out, out_size, &offset, TOOLS_JSON)) return 0;
  if (!append_json(out, out_size, &offset, ",\"messages\":[")) return 0;

  for (int i = 0; i < history->count; i++) {
    const ChatMessage *msg = &history->items[i];
    if (i > 0) {
      if (!append_json(out, out_size, &offset, ",")) return 0;
    }
    if (msg->kind == MSG_KIND_TEXT) {
      char escaped_text[MAX_TEXT];
      json_escape(msg->text, escaped_text, sizeof(escaped_text));
      const char *role = (msg->role == MSG_ROLE_USER) ? "user" : "assistant";
      if (!append_json(out, out_size, &offset, "{\"role\":\"")) return 0;
      if (!append_json(out, out_size, &offset, role)) return 0;
      if (!append_json(out, out_size, &offset, "\",\"content\":[{\"type\":\"text\",\"text\":\"")) return 0;
      if (!append_json(out, out_size, &offset, escaped_text)) return 0;
      if (!append_json(out, out_size, &offset, "\"}]}") ) return 0;
    } else if (msg->kind == MSG_KIND_TOOL_USE) {
      char escaped_name[MAX_TOOL_NAME];
      char escaped_id[MAX_TOOL_ID];
      json_escape(msg->tool_name, escaped_name, sizeof(escaped_name));
      json_escape(msg->tool_use_id, escaped_id, sizeof(escaped_id));
      if (!append_json(out, out_size, &offset, "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"")) return 0;
      if (!append_json(out, out_size, &offset, escaped_id)) return 0;
      if (!append_json(out, out_size, &offset, "\",\"name\":\"")) return 0;
      if (!append_json(out, out_size, &offset, escaped_name)) return 0;
      if (!append_json(out, out_size, &offset, "\",\"input\":")) return 0;
      if (!append_json(out, out_size, &offset, msg->tool_input)) return 0;
      if (!append_json(out, out_size, &offset, "}]}")) return 0;
    } else if (msg->kind == MSG_KIND_TOOL_RESULT) {
      char escaped_text[MAX_TEXT];
      char escaped_id[MAX_TOOL_ID];
      json_escape(msg->text, escaped_text, sizeof(escaped_text));
      json_escape(msg->tool_use_id, escaped_id, sizeof(escaped_id));
      if (!append_json(out, out_size, &offset, "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"")) return 0;
      if (!append_json(out, out_size, &offset, escaped_id)) return 0;
      if (!append_json(out, out_size, &offset, "\",\"content\":\"")) return 0;
      if (!append_json(out, out_size, &offset, escaped_text)) return 0;
      if (!append_json(out, out_size, &offset, "\"}]}") ) return 0;
    }
  }

  if (!append_json(out, out_size, &offset, "]}")) return 0;
  return 1;
}

static int skip_token(const jsmntok_t *tokens, int index) {
  int i = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int j = 0; j < tokens[index].size; j++) {
      i = skip_token(tokens, i);
      i = skip_token(tokens, i);
    }
    return i;
  }
  if (tokens[index].type == JSMN_ARRAY) {
    for (int j = 0; j < tokens[index].size; j++) {
      i = skip_token(tokens, i);
    }
    return i;
  }
  return i;
}

static int parse_response(const char *json, ChatHistory *history, ToolUse *tools, int *tool_count) {
  jsmn_parser parser;
  jsmntok_t tokens[512];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, json, strlen(json), tokens, 512);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    return 0;
  }

  int content_index = json_get_object_value(json, tokens, 0, "content");
  if (content_index < 0 || tokens[content_index].type != JSMN_ARRAY) {
    return 0;
  }

  int idx = content_index + 1;
  int tool_idx = 0;
  for (int i = 0; i < tokens[content_index].size; i++) {
    int item_index = idx;
    if (tokens[item_index].type == JSMN_OBJECT) {
      int type_index = json_get_object_value(json, tokens, item_index, "type");
      if (type_index >= 0 && json_token_streq(json, &tokens[type_index], "text")) {
        char text[MAX_TEXT] = {0};
        json_get_string(json, tokens, item_index, "text", text, sizeof(text));
        if (strlen(text) > 0) {
          history_add(history, MSG_ROLE_ASSISTANT, MSG_KIND_TEXT, text, NULL, NULL, NULL);
          ui_add_message("Claude:", text);
        }
      } else if (type_index >= 0 && json_token_streq(json, &tokens[type_index], "tool_use")) {
        if (tool_idx < MAX_TOOL_CALLS) {
          ToolUse *tool = &tools[tool_idx++];
          memset(tool, 0, sizeof(ToolUse));
          json_get_string(json, tokens, item_index, "id", tool->id, sizeof(tool->id));
          json_get_string(json, tokens, item_index, "name", tool->name, sizeof(tool->name));
          int input_index = json_get_object_value(json, tokens, item_index, "input");
          if (input_index >= 0) {
            int start = tokens[input_index].start;
            int end = tokens[input_index].end;
            int len = end - start;
            if (len >= MAX_TOOL_INPUT_EXEC) {
              len = MAX_TOOL_INPUT_EXEC - 1;
            }
            tool->input = (char *)malloc(len + 1);
            if (tool->input && len > 0) {
              memcpy(tool->input, json + start, len);
              tool->input[len] = '\0';
            }
          }
          if (!tool->input || tool->input[0] == '\0') {
            if (!tool->input) tool->input = (char *)malloc(3);
            if (tool->input) strcpy(tool->input, "{}");
          }
          history_add(history, MSG_ROLE_ASSISTANT, MSG_KIND_TOOL_USE, "", tool->id, tool->name, "{}");
        }
      }
    }
    idx = skip_token(tokens, item_index);
  }

  *tool_count = tool_idx;
  return 1;
}

static int execute_tools(ChatHistory *history, ToolUse *tools, int tool_count) {
  for (int i = 0; i < tool_count; i++) {
    char output[TOOL_BUFFER_SIZE] = {0};
    tool_execute(tools[i].name, tools[i].input, output, sizeof(output));
    history_add(history, MSG_ROLE_USER, MSG_KIND_TOOL_RESULT, output, tools[i].id, tools[i].name, NULL);
    ui_add_message("Tool:", output);
    if (tools[i].input) {
      free(tools[i].input);
      tools[i].input = NULL;
    }
  }
  return tool_count;
}

static int run_chat_loop(ChatHistory *history) {
  char *request = (char *)malloc(REQUEST_BUFFER_SIZE);
  char *response = (char *)malloc(RESPONSE_BUFFER_SIZE);
  char error[128] = {0};

  if (!request || !response) {
    ui_add_message("Error:", "Out of memory");
    free(request);
    free(response);
    return 0;
  }

  for (int loop = 0; loop < MAX_TOOL_LOOPS; loop++) {
    if (!build_request_json(history, request, REQUEST_BUFFER_SIZE)) {
      ui_add_message("Error:", "Request too large");
      free(request);
      free(response);
      return 0;
    }

    ui_set_status("Contacting Claude...");
    ui_render();

    if (!net_post_json(NINTENCODE_API_URL, request, response, RESPONSE_BUFFER_SIZE, error, sizeof(error))) {
      ui_add_message("Error:", error);
      ui_set_status("Ready");
      ui_render();
      free(request);
      free(response);
      return 0;
    }

    ToolUse tools[MAX_TOOL_CALLS];
    int tool_count = 0;
    if (!parse_response(response, history, tools, &tool_count)) {
      ui_add_message("Error:", "Failed to parse response");
      ui_set_status("Ready");
      ui_render();
      free(request);
      free(response);
      return 0;
    }

    if (tool_count == 0) {
      ui_set_status("Ready");
      ui_render();
      free(request);
      free(response);
      return 1;
    }

    execute_tools(history, tools, tool_count);
    ui_set_status("Running tools...");
    ui_render();
  }

  ui_add_message("System:", "Tool loop limit reached");
  ui_set_status("Ready");
  ui_render();
  free(request);
  free(response);
  return 0;
}

int main(int argc, char **argv) {
  gfxInitDefault();
  fsInit();
  
  debug_log_init();
  debug_log("main: started");
  
  ui_init();
  debug_log("main: ui_init done");
  ui_render();

  char error[128] = {0};
  
  ui_set_status("Initializing network...");
  ui_render();
  
  if (!net_init(error, sizeof(error))) {
    ui_add_message("Error:", error);
    ui_set_status("Network failed - Press START");
    ui_render();
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    net_exit();
    fsExit();
    gfxExit();
    return 0;
  }

  ui_set_status("Initializing tools...");
  ui_render();
  
  if (!tools_init(error, sizeof(error))) {
    ui_add_message("Warning:", error);
  }

  ui_set_status("Initializing Lua...");
  ui_render();
  
  if (!lua_runtime_init()) {
    ui_add_message("Warning:", "Lua init failed");
  }

  history_init(&g_history);
  
  ui_set_status("Ready - Press A to chat");
  ui_render();

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    
    if (kDown & KEY_START) {
      break;
    }
    
    if (kDown & KEY_A) {
      char input[512] = {0};
      if (ui_prompt(input, sizeof(input))) {
        ui_add_message("You:", input);
        history_add(&g_history, MSG_ROLE_USER, MSG_KIND_TEXT, input, NULL, NULL, NULL);
        ui_set_status("Sending...");
        ui_render();
        run_chat_loop(&g_history);
      } else {
        ui_set_status("Ready - Press A to chat");
        ui_render();
      }
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  lua_runtime_exit();
  net_exit();
  ui_shutdown();
  fsExit();
  gfxExit();
  return 0;
}
