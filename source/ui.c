/*
 * ui.c - Console-based chat interface
 *
 * Renders chat history on top screen, controls and status on bottom screen.
 * Handles text wrapping, scrolling, and on-screen keyboard input.
 */

#include "ui.h"
#include "debug_log.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* Display dimensions */
#define UI_LINE_WIDTH     50   /* Characters per line on top screen */
#define UI_MAX_LINES      200  /* Total scrollback buffer */
#define UI_VISIBLE_LINES  28   /* Lines visible on top screen */
#define UI_STATUS_SIZE    64   /* Max status message length */
#define UI_BOTTOM_WIDTH   40   /* Characters per line on bottom screen */
#define UI_MESSAGE_BUFFER 8192 /* Buffer for formatting messages */

/* Spinner animation frames */
#define SPINNER_FRAME_COUNT 8
static const char *spinner_frames[SPINNER_FRAME_COUNT] = {
  "[    ]", "[=   ]", "[==  ]", "[=== ]",
  "[ ===]", "[  ==]", "[   =]", "[    ]",
};

/* Console state */
static PrintConsole topConsole;
static PrintConsole bottomConsole;
static char lines[UI_MAX_LINES][UI_LINE_WIDTH + 1];
static int line_count = 0;
static char status_line[UI_STATUS_SIZE] = "Ready";
static int ui_initialized = 0;
static int spinner_frame = 0;

/* Forward declaration */
void ui_render(void);

/*
 * Add a single line to the scrollback buffer.
 * Scrolls existing lines up if buffer is full.
 */
static void ui_add_line(const char *line) {
  if (line_count >= UI_MAX_LINES) {
    for (int i = 1; i < UI_MAX_LINES; i++) {
      strcpy(lines[i - 1], lines[i]);
    }
    line_count = UI_MAX_LINES - 1;
  }
  strncpy(lines[line_count], line, UI_LINE_WIDTH);
  lines[line_count][UI_LINE_WIDTH] = '\0';
  line_count++;
}

/*
 * Wrap text to fit screen width and add to buffer.
 * Handles newlines and long lines.
 */
static void ui_wrap_text(const char *text) {
  char line[UI_LINE_WIDTH + 1];
  int pos = 0;
  
  for (const char *p = text; *p != '\0'; p++) {
    if (*p == '\n') {
      line[pos] = '\0';
      ui_add_line(line);
      pos = 0;
      continue;
    }
    
    /* Skip ANSI escape sequences when counting width */
    if (*p == '\x1b') {
      while (*p && *p != 'm') {
        line[pos++] = *p++;
      }
      if (*p == 'm') {
        line[pos++] = *p;
      }
      continue;
    }
    
    line[pos++] = *p;
    if (pos >= UI_LINE_WIDTH) {
      line[pos] = '\0';
      ui_add_line(line);
      pos = 0;
    }
  }
  if (pos > 0) {
    line[pos] = '\0';
    ui_add_line(line);
  }
}

void ui_init(void) {
  consoleInit(GFX_TOP, &topConsole);
  consoleInit(GFX_BOTTOM, &bottomConsole);
  
  /* ASCII art header */
  ui_add_line("");
  ui_add_line("      \x1b[31m _   _ ___ _   _ _____ _____ _   _ \x1b[0m");
  ui_add_line("      \x1b[31m| \\ | |_ _| \\ | |_   _| ____| \\ | |\x1b[0m");
  ui_add_line("      \x1b[31m|  \\| || ||  \\| | | | |  _| |  \\| |\x1b[0m");
  ui_add_line("      \x1b[31m| |\\  || || |\\  | | | | |___| |\\  |\x1b[0m");
  ui_add_line("      \x1b[31m|_| \\_|___|_| \\_| |_| |_____|_| \\_|\x1b[0m");
  ui_add_line("        \x1b[31m  ____ ___  ____  _____\x1b[0m");
  ui_add_line("        \x1b[31m / ___/ _ \\|  _ \\| ____|\x1b[0m");
  ui_add_line("        \x1b[31m| |  | | | | | | |  _|  \x1b[0m");
  ui_add_line("        \x1b[31m| |__| |_| | |_| | |___ \x1b[0m");
  ui_add_line("        \x1b[31m \\____\\___/|____/|_____| 3DS\x1b[0m");
  ui_add_line("");
  ui_add_line("           Model: Claude Sonnet 4.5");
  ui_add_line("");
  ui_add_line("");
  
  ui_initialized = 1;
}

void ui_shutdown(void) {
  ui_initialized = 0;
}

void ui_reinit_console(void) {
  debug_log("ui_reinit_console: reinitializing");
  
  consoleInit(GFX_TOP, &topConsole);
  consoleInit(GFX_BOTTOM, &bottomConsole);
  consoleSelect(&topConsole);
  ui_render();
  
  debug_log("ui_reinit_console: done");
}

void ui_add_message(const char *prefix, const char *message) {
  char buffer[UI_MESSAGE_BUFFER];
  
  /* Format with color codes based on sender */
  if (strcmp(prefix, "You:") == 0) {
    snprintf(buffer, sizeof(buffer), "\x1b[31m%s\x1b[0m %s", prefix, message);
  } else if (strcmp(prefix, "Claude:") == 0) {
    snprintf(buffer, sizeof(buffer), "\x1b[33m%s\x1b[0m %s", prefix, message);
  } else if (strcmp(prefix, "Tool:") == 0) {
    snprintf(buffer, sizeof(buffer), "\x1b[90m%s\x1b[0m %s", prefix, message);
  } else {
    snprintf(buffer, sizeof(buffer), "%s %s", prefix, message);
  }
  
  ui_wrap_text(buffer);
  ui_add_line("");
  
  if (ui_initialized) {
    ui_render();
  }
}

void ui_set_status(const char *status) {
  strncpy(status_line, status, sizeof(status_line) - 1);
  status_line[sizeof(status_line) - 1] = '\0';
}

void ui_render(void) {
  if (!ui_initialized) return;
  
  /* Top screen: chat history */
  consoleSelect(&topConsole);
  consoleClear();
  
  int start = 0;
  if (line_count > UI_VISIBLE_LINES) {
    start = line_count - UI_VISIBLE_LINES;
  }
  for (int i = start; i < line_count; i++) {
    printf("%s\n", lines[i]);
  }

  /* Bottom screen: status only */
  consoleSelect(&bottomConsole);
  consoleClear();
  
  /* Status at bottom, centered */
  printf("\x1b[28;0H");
  int status_len = strlen(status_line);
  int padding = (UI_BOTTOM_WIDTH - status_len) / 2;
  if (padding < 0) padding = 0;
  printf("%*s%s", padding, "", status_line);
}

void ui_render_thinking(void) {
  if (!ui_initialized) return;
  
  spinner_frame = (spinner_frame + 1) % SPINNER_FRAME_COUNT;
  
  consoleSelect(&bottomConsole);
  consoleClear();
  
  printf("\x1b[14;0H");
  printf("        \x1b[31m%s\x1b[0m Thinking...", spinner_frames[spinner_frame]);
  
  printf("\x1b[28;0H");
  int status_len = strlen(status_line);
  int padding = (UI_BOTTOM_WIDTH - status_len) / 2;
  if (padding < 0) padding = 0;
  printf("%*s%s", padding, "", status_line);
  
  gfxFlushBuffers();
  gfxSwapBuffers();
}

int ui_prompt(char *out, size_t out_size) {
  SwkbdState swkbd;
  swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
  swkbdSetHintText(&swkbd, "Enter your prompt...");
  swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
  swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Send", true);
  
  memset(out, 0, out_size);
  SwkbdButton button = swkbdInputText(&swkbd, out, out_size);
  
  if (button != SWKBD_BUTTON_RIGHT || strlen(out) == 0) {
    return 0;
  }
  return 1;
}
