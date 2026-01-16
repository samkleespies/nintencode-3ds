/*
 * json.c - Minimal JSON parser based on JSMN
 *
 * JSMN (Jasmine) is a minimalist JSON parser that doesn't allocate memory.
 * This file includes the core parser and helper functions for extracting values.
 */

#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   unsigned int num_tokens) {
  if (parser->toknext >= num_tokens) {
    return NULL;
  }
  jsmntok_t *tok = &tokens[parser->toknext++];
  tok->start = -1;
  tok->end = -1;
  tok->size = 0;
  tok->parent = -1;
  tok->type = JSMN_UNDEFINED;
  return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0;
  parser->toknext = 0;
  parser->toksuper = -1;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len,
                                jsmntok_t *tokens, unsigned int num_tokens) {
  int start = parser->pos;
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    if (c == '\t' || c == '\r' || c == '\n' || c == ' ' ||
        c == ',' || c == ']' || c == '}') {
      break;
    }
    if (c < 32) {
      return -1;
    }
  }
  if (tokens != NULL) {
    jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
    if (tok == NULL) {
      return -1;
    }
    jsmn_fill_token(tok, JSMN_PRIMITIVE, start, parser->pos);
    tok->parent = parser->toksuper;
  }
  parser->pos--;
  return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len,
                             jsmntok_t *tokens, unsigned int num_tokens) {
  int start = parser->pos;
  parser->pos++;
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    if (c == '"') {
      if (tokens != NULL) {
        jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
        if (tok == NULL) {
          return -1;
        }
        jsmn_fill_token(tok, JSMN_STRING, start + 1, parser->pos);
        tok->parent = parser->toksuper;
      }
      return 0;
    }
    if (c == '\\') {
      parser->pos++;
      if (parser->pos >= len) {
        return -1;
      }
    }
  }
  return -1;
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned int num_tokens) {
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    switch (c) {
      case '{':
      case '[': {
        jsmntok_t *tok = NULL;
        if (tokens != NULL) {
          tok = jsmn_alloc_token(parser, tokens, num_tokens);
          if (tok == NULL) {
            return -1;
          }
          tok->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
          tok->start = parser->pos;
          tok->parent = parser->toksuper;
        }
        if (parser->toksuper != -1 && tokens != NULL) {
          tokens[parser->toksuper].size++;
        }
        parser->toksuper = parser->toknext - 1;
        break;
      }
      case '}':
      case ']': {
        jsmntype_t type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
        if (tokens != NULL) {
          int i = parser->toknext - 1;
          for (;; i--) {
            if (i < 0) {
              return -1;
            }
            if (tokens[i].start != -1 && tokens[i].end == -1) {
              if (tokens[i].type != type) {
                return -1;
              }
              tokens[i].end = parser->pos + 1;
              parser->toksuper = tokens[i].parent;
              break;
            }
          }
        }
        break;
      }
      case '"': {
        int r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
        if (r < 0) return r;
        if (parser->toksuper != -1 && tokens != NULL) {
          tokens[parser->toksuper].size++;
        }
        break;
      }
      case '\t':
      case '\r':
      case '\n':
      case ' ':
      case ':':
      case ',':
        break;
      default: {
        int r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
        if (r < 0) return r;
        if (parser->toksuper != -1 && tokens != NULL) {
          tokens[parser->toksuper].size++;
        }
        break;
      }
    }
  }
  
  /* Check for unclosed structures */
  if (tokens != NULL) {
    for (unsigned int i = parser->toknext; i > 0; i--) {
      if (tokens[i - 1].start != -1 && tokens[i - 1].end == -1) {
        return -1;
      }
    }
  }
  return (int)parser->toknext;
}

int json_token_streq(const char *json, const jsmntok_t *token, const char *value) {
  size_t len = token->end - token->start;
  return strlen(value) == len && strncmp(json + token->start, value, len) == 0;
}

static int json_skip_token(const jsmntok_t *tokens, int index) {
  int i = index + 1;
  if (tokens[index].type == JSMN_OBJECT) {
    for (int j = 0; j < tokens[index].size; j++) {
      i = json_skip_token(tokens, i);
      i = json_skip_token(tokens, i);
    }
    return i;
  }
  if (tokens[index].type == JSMN_ARRAY) {
    for (int j = 0; j < tokens[index].size; j++) {
      i = json_skip_token(tokens, i);
    }
    return i;
  }
  return i;
}

int json_get_object_value(const char *json, const jsmntok_t *tokens,
                          int object_index, const char *key) {
  if (tokens[object_index].type != JSMN_OBJECT) {
    return -1;
  }
  int count = tokens[object_index].size;
  int i = object_index + 1;
  for (int j = 0; j < count; j++) {
    int key_index = i;
    int value_index = i + 1;
    if (json_token_streq(json, &tokens[key_index], key)) {
      return value_index;
    }
    i = json_skip_token(tokens, value_index);
  }
  return -1;
}

int json_unescape(const char *input, size_t len, char *out, size_t out_size) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < out_size; i++) {
    char c = input[i];
    if (c == '\\' && i + 1 < len) {
      char next = input[i + 1];
      switch (next) {
        case '"':
        case '\\':
        case '/':
          out[o++] = next;
          i++;
          break;
        case 'b': out[o++] = '\b'; i++; break;
        case 'f': out[o++] = '\f'; i++; break;
        case 'n': out[o++] = '\n'; i++; break;
        case 'r': out[o++] = '\r'; i++; break;
        case 't': out[o++] = '\t'; i++; break;
        case 'u':
          if (i + 5 < len) {
            char buf[5] = {0};
            memcpy(buf, input + i + 2, 4);
            unsigned int code = 0;
            if (sscanf(buf, "%x", &code) == 1) {
              out[o++] = (code < 128) ? (char)code : '?';
            }
            i += 5;
          }
          break;
        default:
          out[o++] = next;
          i++;
          break;
      }
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return (int)o;
}

int json_get_string(const char *json, const jsmntok_t *tokens,
                    int object_index, const char *key, char *out, size_t out_size) {
  int value_index = json_get_object_value(json, tokens, object_index, key);
  if (value_index < 0) {
    return 0;
  }
  const jsmntok_t *tok = &tokens[value_index];
  if (tok->type != JSMN_STRING && tok->type != JSMN_PRIMITIVE) {
    return 0;
  }
  size_t len = tok->end - tok->start;
  return json_unescape(json + tok->start, len, out, out_size) > 0;
}

int json_get_int(const char *json, const jsmntok_t *tokens,
                 int object_index, const char *key, int *out_value) {
  int value_index = json_get_object_value(json, tokens, object_index, key);
  if (value_index < 0) {
    return 0;
  }
  const jsmntok_t *tok = &tokens[value_index];
  if (tok->type != JSMN_PRIMITIVE) {
    return 0;
  }
  char buf[32] = {0};
  size_t len = tok->end - tok->start;
  if (len >= sizeof(buf)) {
    len = sizeof(buf) - 1;
  }
  memcpy(buf, json + tok->start, len);
  *out_value = atoi(buf);
  return 1;
}
