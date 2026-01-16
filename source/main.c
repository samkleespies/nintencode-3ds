/*
 * main.c - Nintencode 3DS application entry point
 *
 * Implements the main chat loop with the Claude API.
 * Handles user input, API communication, tool execution, and conversation history.
 */

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
  "You are Nintencode, an AI game creator running on Nintendo 3DS. "
  "Your purpose: BUILD AND RUN playable games for the user.\n\n"
  
  "## ACTION-FIRST RULE\n"
  "When user asks for a game: write_file -> run_lua. NO explanations first.\n"
  "When user asks to fix/change: edit_file -> run_lua. Show results, not plans.\n\n"
  
  "## GAME STRUCTURE\n"
  "```lua\n"
  "-- Game state variables at top\n"
  "local x, y = 200, 120\n\n"
  "function update()\n"
  "  -- Handle input, update state (called every frame)\n"
  "  if key_held('left') then x = x - 2 end\n"
  "end\n\n"
  "function draw()\n"
  "  -- Render graphics (called every frame after update)\n"
  "  clear(0x000000)\n"
  "  circle(x, y, 10, 0xFF0000)\n"
  "end\n"
  "```\n\n"
  
  "## GRAPHICS API\n"
  "clear(color) - fill screen. "
  "rect(x,y,w,h,color) - filled rectangle. "
  "circle(x,y,r,color) - filled circle. "
  "line(x1,y1,x2,y2,thickness,color) - line. "
  "text(x,y,str,size,color) - text (size 0.5-1.0 normal, 1.5+ large).\n\n"
  
  "## INPUT API\n"
  "key_held(name) - true while held. "
  "key_down(name) - true on press frame only. "
  "Keys: a,b,x,y,l,r,up,down,left,right,start,select.\n\n"
  
  "## HELPERS\n"
  "screen_width()=400, screen_height()=240, random(min,max), quit().\n"
  "Colors: 0xRRGGBB (0xFF0000=red, 0x00FF00=green, 0x0000FF=blue, 0xFFFFFF=white, 0x000000=black).\n\n"
  
  "## FILE TOOLS\n"
  "Use grep_files to search code. Use edit_file for changes (not rewriting whole file). "
  "User presses SELECT to exit game and return to chat.\n\n"
  
  "## RESPONSE STYLE\n"
  "- Execute tools immediately, report results briefly\n"
  "- After run_lua: 'Game running! Use arrows to move, A to jump.'\n"
  "- On errors: fix and retry, don't just explain the problem";

static const char *TOOLS_JSON = "["
  "{\"name\":\"read_file\",\"description\":\"Read a file from the working directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"write_file\",\"description\":\"Write a file in the working directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}},"
  "{\"name\":\"edit_file\",\"description\":\"Edit a file using search/replace. Provide old_text to find and new_text to replace with. Use replace_all:true for multiple occurrences.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_text\"]}},"
  "{\"name\":\"list_files\",\"description\":\"List files in a directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}},"
  "{\"name\":\"grep_files\",\"description\":\"Search file contents for a pattern. Returns matching lines with file:line: prefix.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Text pattern to search for\"},\"path\":{\"type\":\"string\",\"description\":\"Directory to search (default: current)\"}},\"required\":[\"pattern\"]}},"
  "{\"name\":\"file_info\",\"description\":\"Show file size information\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"create_directory\",\"description\":\"Create a directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"delete_file\",\"description\":\"Delete a file or empty directory\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
  "{\"name\":\"move_file\",\"description\":\"Move or rename a file\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},\"required\":[\"source\",\"destination\"]}},"
  "{\"name\":\"copy_file\",\"description\":\"Copy a file to a new location\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},\"required\":[\"source\",\"destination\"]}},"
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

/*
 * Score a message's importance for pruning decisions.
 * Higher scores mean more important (less likely to be pruned).
 * Inspired by boing-code's smartPruning.ts
 */
static int score_message(const ChatMessage *msg, int index, int total) {
  int score = 0;
  
  /* Recency bonus: more recent = more important */
  /* Score from 0-50 based on position */
  score += ((index + 1) * 50) / total;
  
  /* Tool usage bonus: tool interactions are important context */
  if (msg->kind == MSG_KIND_TOOL_USE || msg->kind == MSG_KIND_TOOL_RESULT) {
    score += 30;
  }
  
  /* Error/warning bonus: critical context */
  if (strstr(msg->text, "error") != NULL || strstr(msg->text, "Error") != NULL ||
      strstr(msg->text, "fail") != NULL || strstr(msg->text, "Fail") != NULL) {
    score += 20;
  }
  
  /* Code block bonus */
  if (strstr(msg->text, "```") != NULL || strstr(msg->text, "function") != NULL) {
    score += 10;
  }
  
  /* User questions are important */
  if (msg->role == MSG_ROLE_USER && strchr(msg->text, '?') != NULL) {
    score += 15;
  }
  
  return score;
}

/*
 * Check if message at index is part of a tool call/result pair.
 * Returns: 1 if this is a tool_use that has a matching tool_result after it
 *          2 if this is a tool_result that has a matching tool_use before it
 *          0 otherwise
 */
static int is_tool_pair_member(const ChatHistory *history, int index) {
  const ChatMessage *msg = &history->items[index];
  
  if (msg->kind == MSG_KIND_TOOL_USE) {
    /* Look for matching tool_result after this message */
    for (int j = index + 1; j < history->count; j++) {
      if (history->items[j].kind == MSG_KIND_TOOL_RESULT &&
          strcmp(history->items[j].tool_use_id, msg->tool_use_id) == 0) {
        return 1;
      }
      /* Stop if we hit another user message */
      if (history->items[j].role == MSG_ROLE_USER && 
          history->items[j].kind == MSG_KIND_TEXT) {
        break;
      }
    }
  }
  
  if (msg->kind == MSG_KIND_TOOL_RESULT) {
    /* Look for matching tool_use before this message */
    for (int j = index - 1; j >= 0; j--) {
      if (history->items[j].kind == MSG_KIND_TOOL_USE &&
          strcmp(history->items[j].tool_use_id, msg->tool_use_id) == 0) {
        return 2;
      }
      /* Stop if we hit a user message */
      if (history->items[j].role == MSG_ROLE_USER && 
          history->items[j].kind == MSG_KIND_TEXT) {
        break;
      }
    }
  }
  
  return 0;
}

/*
 * Prune conversation history to reduce context size.
 * Removes lowest-scoring messages while keeping tool_call/result pairs together.
 * Called when API returns context_length_exceeded error.
 */
static void history_prune(ChatHistory *history) {
  if (history->count <= 4) return; /* Keep at least 4 messages */
  
  /* Find lowest scoring message that's safe to remove */
  int lowest_score = 9999;
  int lowest_index = -1;
  
  for (int i = 0; i < history->count; i++) {
    /* Never remove first or last 2 messages */
    if (i == 0 || i >= history->count - 2) continue;
    
    /* Skip tool pair members - they must be removed together */
    int pair_status = is_tool_pair_member(history, i);
    if (pair_status != 0) continue;
    
    int score = score_message(&history->items[i], i, history->count);
    if (score < lowest_score) {
      lowest_score = score;
      lowest_index = i;
    }
  }
  
  /* If no safe single message found, try to remove a tool pair */
  if (lowest_index < 0) {
    /* Find lowest scoring tool_use that has a result */
    for (int i = 1; i < history->count - 2; i++) {
      if (history->items[i].kind == MSG_KIND_TOOL_USE) {
        int pair_status = is_tool_pair_member(history, i);
        if (pair_status == 1) {
          int score = score_message(&history->items[i], i, history->count);
          if (score < lowest_score) {
            lowest_score = score;
            lowest_index = i;
          }
        }
      }
    }
  }
  
  if (lowest_index < 0) return;
  
  /* Remove the message (and its pair if it's a tool_use) */
  if (history->items[lowest_index].kind == MSG_KIND_TOOL_USE) {
    /* Find and remove the matching result first */
    for (int j = lowest_index + 1; j < history->count; j++) {
      if (history->items[j].kind == MSG_KIND_TOOL_RESULT &&
          strcmp(history->items[j].tool_use_id, 
                 history->items[lowest_index].tool_use_id) == 0) {
        /* Shift messages after j */
        for (int k = j; k < history->count - 1; k++) {
          history->items[k] = history->items[k + 1];
        }
        history->count--;
        break;
      }
    }
  }
  
  /* Remove the message at lowest_index */
  for (int k = lowest_index; k < history->count - 1; k++) {
    history->items[k] = history->items[k + 1];
  }
  history->count--;
}

/*
 * Aggressively prune history - remove multiple messages.
 * Called when context is way too long.
 */
static void history_prune_aggressive(ChatHistory *history) {
  /* Remove up to half the messages */
  int target = history->count / 2;
  if (target < 4) target = 4;
  
  while (history->count > target) {
    int old_count = history->count;
    history_prune(history);
    if (history->count == old_count) break; /* Safety: couldn't prune more */
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
      /* Check for context length error - prune and retry */
      if (strstr(error, "Context too long") != NULL || strstr(error, "context_length") != NULL) {
        ui_add_message("System:", "Context too long, pruning history...");
        history_prune_aggressive(history);
        ui_set_status("Retrying with pruned context...");
        ui_render();
        continue; /* Retry the loop with pruned history */
      }
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
