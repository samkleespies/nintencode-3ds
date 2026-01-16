/*
 * tools.c - Claude tool implementations
 *
 * Provides file operations and Lua execution that Claude can invoke.
 * All file operations are sandboxed to NINTENCODE_WORKDIR.
 */

#include "tools.h"
#include "app_config.h"
#include "debug_log.h"
#include "json.h"
#include "lua_runtime.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Limits */
#define PATH_MAX_LEN          512
#define LIST_MAX_ENTRIES      500
#define WRITE_FILE_MAX        32768
#define JSON_MAX_TOKENS       256

/*
 * Helper to parse JSON input for tools.
 * Returns 1 on success, 0 on failure.
 */
static int parse_tool_input(const char *input_json, jsmn_parser *parser,
                            jsmntok_t *tokens, int max_tokens) {
  jsmn_init(parser);
  int count = jsmn_parse(parser, input_json, strlen(input_json), tokens, max_tokens);
  return (count >= 1 && tokens[0].type == JSMN_OBJECT);
}

static int ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return mkdir(path, 0777) == 0;
}

static int is_safe_path(const char *path) {
  if (path[0] == '\0') return 0;
  if (path[0] == '/' || strchr(path, ':') != NULL) return 0;
  if (strstr(path, "..") != NULL) return 0;
  return 1;
}

static int build_full_path(const char *relative, char *out, size_t out_size) {
  if (!is_safe_path(relative)) return 0;
  snprintf(out, out_size, "%s/%s", NINTENCODE_WORKDIR, relative);
  return 1;
}

int tools_init(char *error, size_t error_size) {
  if (!ensure_dir(NINTENCODE_BASE_PATH)) {
    snprintf(error, error_size, "failed to create base dir");
    return 0;
  }
  if (!ensure_dir(NINTENCODE_WORKDIR)) {
    snprintf(error, error_size, "failed to create workdir");
    return 0;
  }
  return 1;
}

static int tool_read_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    snprintf(output, output_size, "Missing path");
    return 0;
  }

  int offset = 0, limit = 200;
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

  /* Get file size for header */
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  /* Count total lines first */
  int total_lines = 0;
  int ch;
  while ((ch = fgetc(file)) != EOF) {
    if (ch == '\n') total_lines++;
  }
  if (file_size > 0) total_lines++; /* Count last line if no trailing newline */
  fseek(file, 0, SEEK_SET);

  /* Write header */
  size_t written = 0;
  int needed = snprintf(output, output_size, "File: %s (%d lines, %ld bytes)\n\n",
                        path, total_lines, file_size);
  if (needed > 0) written = (size_t)needed;

  char line[512];
  int line_num = 0;
  int lines_read = 0;
  int truncated = 0;
  
  while (fgets(line, sizeof(line), file)) {
    if (line_num >= offset) {
      if (lines_read >= limit) {
        truncated = 1;
        break;
      }
      
      /* Truncate very long lines (>200 chars) to save tokens */
      size_t line_len = strlen(line);
      if (line_len > 200) {
        line[197] = '.';
        line[198] = '.';
        line[199] = '.';
        line[200] = '\n';
        line[201] = '\0';
      }
      
      needed = snprintf(output + written, output_size - written,
                        "%4d\t%s", line_num + 1, line);
      if (needed < 0 || written + (size_t)needed >= output_size) {
        truncated = 1;
        break;
      }
      written += (size_t)needed;
      lines_read++;
    }
    line_num++;
  }
  fclose(file);
  
  /* Add truncation notice if needed */
  if (truncated && written + 30 < output_size) {
    written += snprintf(output + written, output_size - written,
                        "\n...(truncated, %d more lines)", total_lines - line_num);
  }
  
  if (lines_read == 0) {
    snprintf(output, output_size, "File: %s (empty)", path);
  }
  return 1;
}

static int tool_write_file(const char *input_json, char *output, size_t output_size) {
  debug_log("tool_write_file: starting");
  
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    debug_log("tool_write_file: invalid input");
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char path[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    debug_log("tool_write_file: missing path");
    snprintf(output, output_size, "Missing path");
    return 0;
  }
  
  debug_log("tool_write_file: path=%s", path);
  
  char *content = malloc(WRITE_FILE_MAX);
  if (!content) {
    debug_log("tool_write_file: malloc failed");
    snprintf(output, output_size, "Out of memory");
    return 0;
  }
  memset(content, 0, WRITE_FILE_MAX);
  
  if (!json_get_string(input_json, tokens, 0, "content", content, WRITE_FILE_MAX)) {
    free(content);
    debug_log("tool_write_file: missing content");
    snprintf(output, output_size, "Missing content");
    return 0;
  }

  debug_log("tool_write_file: content len=%d", (int)strlen(content));

  char full_path[PATH_MAX_LEN] = {0};
  if (!build_full_path(path, full_path, sizeof(full_path))) {
    free(content);
    debug_log("tool_write_file: blocked path");
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  debug_log("tool_write_file: opening %s", full_path);
  
  FILE *file = fopen(full_path, "w");
  if (!file) {
    free(content);
    debug_log("tool_write_file: fopen failed");
    snprintf(output, output_size, "Unable to write: %s", path);
    return 0;
  }
  
  size_t content_len = strlen(content);
  fwrite(content, 1, content_len, file);
  fclose(file);
  free(content);
  
  debug_log("tool_write_file: wrote %d bytes", (int)content_len);
  snprintf(output, output_size, "Wrote %s (%zu bytes)", path, content_len);
  return 1;
}

static int tool_list_files(const char *input_json, char *output, size_t output_size) {
  char path[PATH_MAX_LEN] = {0};
  
  if (input_json && strlen(input_json) > 0) {
    jsmn_parser parser;
    jsmntok_t tokens[JSON_MAX_TOKENS];
    if (parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
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
    int needed = snprintf(output + written, output_size - written,
                         "%s\n", entry->d_name);
    if (needed < 0 || written + (size_t)needed >= output_size) break;
    written += (size_t)needed;
    if (++count >= LIST_MAX_ENTRIES) break;
  }
  closedir(dir);
  
  if (written == 0) {
    snprintf(output, output_size, "No files");
  }
  return 1;
}

static int tool_file_info(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
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
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
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

/*
 * Delete a file or empty directory.
 */
static int tool_delete_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
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
    snprintf(output, output_size, "Not found: %s", path);
    return 0;
  }

  int result;
  if (S_ISDIR(st.st_mode)) {
    result = rmdir(full_path);
    if (result != 0) {
      snprintf(output, output_size, "rmdir failed (not empty?): %s", path);
      return 0;
    }
    snprintf(output, output_size, "Deleted directory: %s", path);
  } else {
    result = remove(full_path);
    if (result != 0) {
      snprintf(output, output_size, "remove failed: %s", path);
      return 0;
    }
    snprintf(output, output_size, "Deleted file: %s", path);
  }
  return 1;
}

/*
 * Move/rename a file or directory.
 */
static int tool_move_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char source[PATH_MAX_LEN] = {0};
  char dest[PATH_MAX_LEN] = {0};
  
  if (!json_get_string(input_json, tokens, 0, "source", source, sizeof(source))) {
    snprintf(output, output_size, "Missing source");
    return 0;
  }
  if (!json_get_string(input_json, tokens, 0, "destination", dest, sizeof(dest))) {
    snprintf(output, output_size, "Missing destination");
    return 0;
  }

  char full_source[PATH_MAX_LEN] = {0};
  char full_dest[PATH_MAX_LEN] = {0};
  
  if (!build_full_path(source, full_source, sizeof(full_source))) {
    snprintf(output, output_size, "Blocked source path");
    return 0;
  }
  if (!build_full_path(dest, full_dest, sizeof(full_dest))) {
    snprintf(output, output_size, "Blocked destination path");
    return 0;
  }

  if (rename(full_source, full_dest) != 0) {
    snprintf(output, output_size, "Move failed: %d", errno);
    return 0;
  }
  
  snprintf(output, output_size, "Moved %s -> %s", source, dest);
  return 1;
}

/*
 * Copy a file.
 */
static int tool_copy_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char source[PATH_MAX_LEN] = {0};
  char dest[PATH_MAX_LEN] = {0};
  
  if (!json_get_string(input_json, tokens, 0, "source", source, sizeof(source))) {
    snprintf(output, output_size, "Missing source");
    return 0;
  }
  if (!json_get_string(input_json, tokens, 0, "destination", dest, sizeof(dest))) {
    snprintf(output, output_size, "Missing destination");
    return 0;
  }

  char full_source[PATH_MAX_LEN] = {0};
  char full_dest[PATH_MAX_LEN] = {0};
  
  if (!build_full_path(source, full_source, sizeof(full_source))) {
    snprintf(output, output_size, "Blocked source path");
    return 0;
  }
  if (!build_full_path(dest, full_dest, sizeof(full_dest))) {
    snprintf(output, output_size, "Blocked destination path");
    return 0;
  }

  FILE *src_file = fopen(full_source, "rb");
  if (!src_file) {
    snprintf(output, output_size, "Source not found: %s", source);
    return 0;
  }

  FILE *dst_file = fopen(full_dest, "wb");
  if (!dst_file) {
    fclose(src_file);
    snprintf(output, output_size, "Cannot create: %s", dest);
    return 0;
  }

  char buffer[4096];
  size_t bytes_read;
  size_t total_bytes = 0;
  
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
    if (fwrite(buffer, 1, bytes_read, dst_file) != bytes_read) {
      fclose(src_file);
      fclose(dst_file);
      snprintf(output, output_size, "Write error during copy");
      return 0;
    }
    total_bytes += bytes_read;
  }

  fclose(src_file);
  fclose(dst_file);
  
  snprintf(output, output_size, "Copied %s -> %s (%zu bytes)", source, dest, total_bytes);
  return 1;
}

/*
 * Search file contents for a pattern (simple substring search).
 * Inspired by boing-code's grep_files.
 */
static int tool_grep_files(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char pattern[256] = {0};
  char path[PATH_MAX_LEN] = {0};
  
  if (!json_get_string(input_json, tokens, 0, "pattern", pattern, sizeof(pattern))) {
    snprintf(output, output_size, "Missing pattern");
    return 0;
  }
  
  /* Default to current directory */
  if (!json_get_string(input_json, tokens, 0, "path", path, sizeof(path))) {
    strcpy(path, ".");
  }

  char full_path[PATH_MAX_LEN] = {0};
  if (strcmp(path, ".") == 0) {
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

  size_t written = 0;
  int match_count = 0;
  int file_count = 0;
  struct dirent *entry;
  
  while ((entry = readdir(dir)) != NULL && written + 100 < output_size) {
    if (entry->d_name[0] == '.') continue;
    
    /* Build file path */
    char file_path[PATH_MAX_LEN];
    snprintf(file_path, sizeof(file_path), "%s/%s", full_path, entry->d_name);
    
    /* Skip directories */
    struct stat st;
    if (stat(file_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
    
    /* Open and search file */
    FILE *file = fopen(file_path, "r");
    if (!file) continue;
    
    char line[512];
    int line_num = 0;
    int file_had_match = 0;
    
    while (fgets(line, sizeof(line), file) && written + 200 < output_size) {
      line_num++;
      if (strstr(line, pattern) != NULL) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        
        /* Truncate long lines */
        if (strlen(line) > 80) {
          line[77] = '.';
          line[78] = '.';
          line[79] = '.';
          line[80] = '\0';
        }
        
        int needed = snprintf(output + written, output_size - written,
                              "%s:%d: %s\n", entry->d_name, line_num, line);
        if (needed > 0) written += (size_t)needed;
        match_count++;
        file_had_match = 1;
        
        /* Limit matches per file */
        if (match_count > 50) {
          fclose(file);
          closedir(dir);
          written += snprintf(output + written, output_size - written,
                              "\n...(stopped at 50 matches)");
          return 1;
        }
      }
    }
    fclose(file);
    if (file_had_match) file_count++;
  }
  closedir(dir);
  
  if (match_count == 0) {
    snprintf(output, output_size, "No matches for: %s", pattern);
  } else {
    /* Add summary at end */
    snprintf(output + written, output_size - written,
             "\n%d matches in %d files", match_count, file_count);
  }
  return 1;
}

/*
 * Edit file using search/replace.
 * Inspired by boing-code's edit_file implementation.
 */
static int tool_edit_file(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
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

  /* Read old_text and new_text */
  char *old_text = malloc(WRITE_FILE_MAX);
  char *new_text = malloc(WRITE_FILE_MAX);
  if (!old_text || !new_text) {
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "Out of memory");
    return 0;
  }
  memset(old_text, 0, WRITE_FILE_MAX);
  memset(new_text, 0, WRITE_FILE_MAX);

  if (!json_get_string(input_json, tokens, 0, "old_text", old_text, WRITE_FILE_MAX)) {
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "Missing old_text");
    return 0;
  }
  
  /* new_text is optional (can be empty to delete) */
  json_get_string(input_json, tokens, 0, "new_text", new_text, WRITE_FILE_MAX);

  /* Check replace_all flag */
  int replace_all = 0;
  json_get_int(input_json, tokens, 0, "replace_all", &replace_all);

  /* Read existing file content */
  FILE *file = fopen(full_path, "r");
  if (!file) {
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "File not found: %s", path);
    return 0;
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size > WRITE_FILE_MAX - 1) {
    fclose(file);
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "File too large to edit");
    return 0;
  }

  char *content = malloc(WRITE_FILE_MAX);
  if (!content) {
    fclose(file);
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "Out of memory");
    return 0;
  }
  memset(content, 0, WRITE_FILE_MAX);
  fread(content, 1, file_size, file);
  fclose(file);

  /* Find old_text in content */
  char *match = strstr(content, old_text);
  if (!match) {
    free(content);
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "String not found in %s", path);
    return 0;
  }

  /* Check for multiple occurrences if not replace_all */
  if (!replace_all) {
    char *second_match = strstr(match + 1, old_text);
    if (second_match) {
      /* Count total occurrences */
      int count = 1;
      char *p = match;
      while ((p = strstr(p + 1, old_text)) != NULL) {
        count++;
      }
      free(content);
      free(old_text);
      free(new_text);
      snprintf(output, output_size, "Found %d occurrences. Use replace_all or provide more context.", count);
      return 0;
    }
  }

  /* Perform replacement(s) */
  char *result = malloc(WRITE_FILE_MAX);
  if (!result) {
    free(content);
    free(old_text);
    free(new_text);
    snprintf(output, output_size, "Out of memory");
    return 0;
  }

  size_t old_len = strlen(old_text);
  size_t new_len = strlen(new_text);
  size_t result_pos = 0;
  char *src = content;
  int replacements = 0;

  while (*src) {
    char *found = strstr(src, old_text);
    if (found && (replace_all || replacements == 0)) {
      /* Copy content before match */
      size_t prefix_len = found - src;
      if (result_pos + prefix_len + new_len >= WRITE_FILE_MAX) {
        free(content);
        free(old_text);
        free(new_text);
        free(result);
        snprintf(output, output_size, "Result too large");
        return 0;
      }
      memcpy(result + result_pos, src, prefix_len);
      result_pos += prefix_len;
      
      /* Copy new_text */
      memcpy(result + result_pos, new_text, new_len);
      result_pos += new_len;
      
      src = found + old_len;
      replacements++;
    } else {
      /* Copy rest of string */
      size_t remaining = strlen(src);
      if (result_pos + remaining >= WRITE_FILE_MAX) {
        remaining = WRITE_FILE_MAX - result_pos - 1;
      }
      memcpy(result + result_pos, src, remaining);
      result_pos += remaining;
      break;
    }
  }
  result[result_pos] = '\0';

  /* Write result back to file */
  file = fopen(full_path, "w");
  if (!file) {
    free(content);
    free(old_text);
    free(new_text);
    free(result);
    snprintf(output, output_size, "Unable to write: %s", path);
    return 0;
  }
  fwrite(result, 1, result_pos, file);
  fclose(file);

  free(content);
  free(old_text);
  free(new_text);
  free(result);

  snprintf(output, output_size, "Edited %s (%d replacement%s)", 
           path, replacements, replacements == 1 ? "" : "s");
  return 1;
}

static int tool_run_lua(const char *input_json, char *output, size_t output_size) {
  jsmn_parser parser;
  jsmntok_t tokens[JSON_MAX_TOKENS];
  
  if (!parse_tool_input(input_json, &parser, tokens, JSON_MAX_TOKENS)) {
    snprintf(output, output_size, "Invalid input");
    return 0;
  }

  char script[PATH_MAX_LEN] = {0};
  if (!json_get_string(input_json, tokens, 0, "script", script, sizeof(script))) {
    snprintf(output, output_size, "Missing script parameter");
    return 0;
  }

  /* Validate .lua extension */
  size_t len = strlen(script);
  if (len < 5 || strcmp(script + len - 4, ".lua") != 0) {
    snprintf(output, output_size, "Script must be a .lua file");
    return 0;
  }

  if (!is_safe_path(script)) {
    snprintf(output, output_size, "Blocked path");
    return 0;
  }

  /* Verify file exists */
  char full_path[PATH_MAX_LEN];
  snprintf(full_path, sizeof(full_path), "%s/%s", NINTENCODE_WORKDIR, script);
  FILE *f = fopen(full_path, "r");
  if (!f) {
    snprintf(output, output_size, "Script not found: %s", script);
    return 0;
  }
  fclose(f);

  /* Run the game */
  char lua_error[512] = {0};
  if (!lua_runtime_run(script, lua_error, sizeof(lua_error))) {
    snprintf(output, output_size, "Lua error: %s", lua_error);
    return 0;
  }

  snprintf(output, output_size, "Game exited normally");
  return 1;
}

int tool_execute(const char *name, const char *input_json,
                 char *output, size_t output_size) {
  debug_log("tool_execute: name=%s", name);
  
  if (strcmp(name, "read_file") == 0) {
    return tool_read_file(input_json, output, output_size);
  }
  if (strcmp(name, "write_file") == 0) {
    return tool_write_file(input_json, output, output_size);
  }
  if (strcmp(name, "edit_file") == 0) {
    return tool_edit_file(input_json, output, output_size);
  }
  if (strcmp(name, "list_files") == 0) {
    return tool_list_files(input_json, output, output_size);
  }
  if (strcmp(name, "grep_files") == 0) {
    return tool_grep_files(input_json, output, output_size);
  }
  if (strcmp(name, "file_info") == 0) {
    return tool_file_info(input_json, output, output_size);
  }
  if (strcmp(name, "create_directory") == 0) {
    return tool_create_directory(input_json, output, output_size);
  }
  if (strcmp(name, "delete_file") == 0) {
    return tool_delete_file(input_json, output, output_size);
  }
  if (strcmp(name, "move_file") == 0) {
    return tool_move_file(input_json, output, output_size);
  }
  if (strcmp(name, "copy_file") == 0) {
    return tool_copy_file(input_json, output, output_size);
  }
  if (strcmp(name, "run_lua") == 0) {
    return tool_run_lua(input_json, output, output_size);
  }
  snprintf(output, output_size, "Unknown tool: %s", name);
  return 0;
}
