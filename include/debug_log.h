/*
 * debug_log.h - Simple file-based debug logging to SD card
 *
 * Logs are written to sdmc:/nintencode/debug.log for troubleshooting.
 */

#pragma once

#include <stdio.h>
#include <stdarg.h>

#define DEBUG_LOG_PATH "sdmc:/nintencode/debug.log"

static inline void debug_log_init(void) {
  FILE *f = fopen(DEBUG_LOG_PATH, "w");
  if (f) {
    fprintf(f, "=== Nintencode Debug Log ===\n");
    fclose(f);
  }
}

static inline void debug_log(const char *fmt, ...) {
  FILE *f = fopen(DEBUG_LOG_PATH, "a");
  if (f) {
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    va_end(args);
    fclose(f);
  }
}
