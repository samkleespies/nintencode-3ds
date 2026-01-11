#pragma once

#include <stddef.h>

void ui_init(void);
void ui_shutdown(void);
void ui_reinit_console(void);
void ui_add_message(const char *prefix, const char *message);
void ui_set_status(const char *status);
void ui_render(void);
void ui_render_thinking(void);
int ui_prompt(char *out, size_t out_size);
