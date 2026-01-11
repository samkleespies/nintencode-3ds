#pragma once

#include <stddef.h>

// JSMN token types
typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT = 1,
  JSMN_ARRAY = 2,
  JSMN_STRING = 3,
  JSMN_PRIMITIVE = 4
} jsmntype_t;

typedef struct {
  jsmntype_t type;
  int start;
  int end;
  int size;
  int parent;
} jsmntok_t;

typedef struct {
  unsigned int pos;
  unsigned int toknext;
  int toksuper;
} jsmn_parser;

// Core JSMN functions
void jsmn_init(jsmn_parser *parser);
int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, unsigned int num_tokens);

// Helper functions for extracting values
int json_token_streq(const char *json, const jsmntok_t *token, const char *value);
int json_get_object_value(const char *json, const jsmntok_t *tokens, int object_index, const char *key);
int json_get_string(const char *json, const jsmntok_t *tokens, int object_index, const char *key, char *out, size_t out_size);
int json_get_int(const char *json, const jsmntok_t *tokens, int object_index, const char *key, int *out_value);
int json_unescape(const char *input, size_t len, char *out, size_t out_size);
