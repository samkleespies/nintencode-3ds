#include "tools.h"

#include "app_config.h"
#include "app_types.h"
#include "json.h"
#include "lua_runtime.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TOOL_OUTPUT_MAX 8192
#define PATH_MAX_LEN 512
#define LIST_MAX_ENTRIES 500
#define WRITE_FILE_MAX_CONTENT 32768

static int ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return 1;
    }
    return 0;
  }
  if (mkdir(path, 0777) != 0) {
    return 0;
  }
  return 1;
}

static int is_safe_path(const char *path) {
  if (path[0] == '\0') {
    return 0;
  }
  if (path[0] == '/' || strchr(path, ':') != NULL) {
    return 0;
  }
  if (strstr(path, "..") != NULL) {
    return 0;
  }
  return 1;
}

static int build_full_path(const char *relative, char *out, size_t out_size) {
  if (!is_safe_path(relative)) {
    return 0;
  }
  snprintf(out, out_size, "%s/%s", NINTENCODE_WORKDIR, relative);
  return 1;
}

int tools_init(char *error, size_t error_size) {
  if (!ensure_dir(NINTENCODE_BASE_PATH)) {
    snprintf(error, error_size, "base dir create failed");
    return 0;
  }
  if (!ensure_dir(NINTENCODE_WORKDIR)) {
    snprintf(error, error_size, "workdir create failed");
    return 0;
  }
  return 1;
}

static int tool_read_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    snprintf(output, output_size, "Missing path");
    return 0;
  }

  int offset = 0;
  int limit = 200;
  json_get_int(input_json, tokens, 0, "offset", &offset);
  json_get_int(input_json, tokens, 0, "limit", &limit);

  char full_path[PATH_MAX_LEN] = {0};
  if (!build_full_path(path, full_path, sizeof(full_path))) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  FILE *file = fopen(full_path, "r");
  if (!file) {
    snprintf(output, output_size, "File not found: %s", path);
    return 0;
  }

  char line[512];
  int line_num = 0;
  size_t written = 0;
  while (fgets(line, sizeof(line), file)) {
    if (line_num >= offset) {
      if (limit <= 0) {
        break;
      }
      int needed = snprintf(output + written, output_size - written, "%04d\t%s", line_num + 1, line);
      if (needed < 0 || written + (size_t)needed >= output_size) {
        break;
      }
      written += (size_t)needed;
      limit--;
    }
    line_num++;
  }
  fclose(file);
  if (written == 0) {
    snprintf(output, output_size, "No content");
  }
  return 1;
}

static int tool_write_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    snprintf(output, output_size, "Missing path");
    return 0;
  }
  
  char *content = (char *)malloc(WRITE_FILE_MAX_CONTENT);
  if (!content) {
    snprintf(output, output_size, "Out of memory");
    return 0;
  }
  memset(content, 0, WRITE_FILE_MAX_CONTENT);
  
  if (!json_get_string(input_json, tokens, 0, "content", content, WRITE_FILE_MAX_CONTENT)) {
    free(content);
    snprintf(output, output_size, "Missing content");
    return 0;
  }

  char full_path[PATH_MAX_LEN] = {0};
  if (!build_full_path(path, full_path, sizeof(full_path))) {
    free(content);
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  FILE *file = fopen(full_path, "w");
  if (!file) {
    free(content);
    snprintf(output, output_size, "Unable to write: %s", path);
    return 0;
  }
  size_t content_len = strlen(content);
  fwrite(content, 1, content_len, file);
  fclose(file);
  free(content);
  snprintf(output, output_size, "Wrote %s (%zu bytes)", path, content_len);
  return 1;
}

static int tool_list_files(const char *input_json, char *output, size_t output_size) {
  char path[PATH_MAX_LEN] = {0};
  if (input_json && strlen(input_json) > 0) {
    jsmn_parser parser;
    jsmntok_t tokens[128];
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
    if (count >= 1 && tokens[0].type == JSMN_OBJECT) {
      json_get_string(input_json, tokens, 0, "path", path, sizeof(path));
    }
  }

  char full_path[PATH_MAX_LEN] = {0};
  if (strlen(path) == 0) {
    snprintf(full_path, sizeof(full_path), "%s", NINTENCODE_WORKDIR);
  } else if (!build_full_path(path, full_path, sizeof(full_path))) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  DIR *dir = opendir(full_path);
  if (!dir) {
    snprintf(output, output_size, "Directory not found");
    return 0;
  }

  struct dirent *entry;
  size_t written = 0;
  int count = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    int needed = snprintf(output + written, output_size - written, "%s\n", entry->d_name);
    if (needed < 0 || written + (size_t)needed >= output_size) {
      break;
    }
    written += (size_t)needed;
    count++;
    if (count >= LIST_MAX_ENTRIES) {
      break;
    }
  }
  closedir(dir);
  if (written == 0) {
    snprintf(output, output_size, "No files");
  }
  return 1;
}

static int tool_file_info(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    snprintf(output, output_size, "Missing path");
    return 0;
  }

  char full_path[PATH_MAX_LEN] = {0};
  if (!build_full_path(path, full_path, sizeof(full_path))) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  struct stat st;
  if (stat(full_path, &st) != 0) {
    snprintf(output, output_size, "Not found");
    return 0;
  }

  snprintf(output, output_size, "Size: %lu bytes", (unsigned long)st.st_size);
  return 1;
}

static int tool_create_directory(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    snprintf(output, output_size, "Missing path");
    return 0;
  }

  char full_path[PATH_MAX_LEN] = {0};
  if (!build_full_path(path, full_path, sizeof(full_path))) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  if (mkdir(full_path, 0777) != 0) {
    snprintf(output, output_size, "mkdir failed: %d", errno);
    return 0;
  }
  snprintf(output, output_size, "Created %s", path);
  return 1;
}

static int tool_run_lua(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[128];
  jsmn_init(&parser);
  int count = jsmn_parse(&parser, input_json, strlen(input_json), tokens, 128);
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char script[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "script", script, sizeof(script))) {
    snprintf(output, output_size, "Missing script parameter");
    return 0;
  }

  size_t len = strlen(script);
  if (len < 5 || strcmp(script + len - 4, ".lua") != 0) {
    snprintf(output, output_size, "Script must be a .lua file");
    return 0;
  }

  if (!is_safe_path(script)) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  char full_path[PATH_MAX_LEN];
  snprintf(full_path, sizeof(full_path), "%s/%s", NINTENCODE_WORKDIR, script);
  FILE *f = fopen(full_path, "r");
  if (!f) {
    snprintf(output, output_size, "Script not found: %s", script);
    return 0;
  }
  fclose(f);

  char lua_error[512] = {0};
  int result = lua_runtime_run(script, lua_error, sizeof(lua_error));
  
  if (!result) {
    snprintf(output, output_size, "Lua error: %s", lua_error);
    return 0;
  }

  snprintf(output, output_size, "Game exited normally");
  return 1;
}

int tool_execute(const char *name, const char *input_json, char *output, size_t output_size) {
  if (strcmp(name, "read_file") == 0) {
    return tool_read_file(input_json, output, output_size);
  }
  if (strcmp(name, "write_file") == 0) {
    return tool_write_file(input_json, output, output_size);
  }
  if (strcmp(name, "list_files") == 0) {
    return tool_list_files(input_json, output, output_size);
  }
  if (strcmp(name, "file_info") == 0) {
    return tool_file_info(input_json, output, output_size);
  }
  if (strcmp(name, "create_directory") == 0) {
    return tool_create_directory(input_json, output, output_size);
  }
  if (strcmp(name, "run_lua") == 0) {
    return tool_run_lua(input_json, output, output_size);
  }
  snprintf(output, output_size, "Unknown tool: %s", name);
  return 0;
}
