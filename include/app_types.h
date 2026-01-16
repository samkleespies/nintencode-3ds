/*
 * app_types.h - Core data types for chat messages and history
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Chat history limits */
#define MAX_MESSAGES    20
#define MAX_TEXT        4096
#define MAX_TOOL_INPUT  4096
#define MAX_TOOL_ID     128
#define MAX_TOOL_NAME   128

typedef enum {
  MSG_ROLE_USER = 0,
  MSG_ROLE_ASSISTANT = 1
} MessageRole;

typedef enum {
  MSG_KIND_TEXT = 0,
  MSG_KIND_TOOL_USE = 1,
  MSG_KIND_TOOL_RESULT = 2
} MessageKind;

typedef struct {
  MessageRole role;
  MessageKind kind;
  char text[MAX_TEXT];
  char tool_use_id[MAX_TOOL_ID];
  char tool_name[MAX_TOOL_NAME];
  char tool_input[MAX_TOOL_INPUT];
} ChatMessage;

typedef struct {
  ChatMessage items[MAX_MESSAGES];
  int count;
} ChatHistory;
