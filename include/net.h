#pragma once

#include <stddef.h>

int net_init(char *error, size_t error_size);
void net_exit(void);
int net_post_json(const char *url, const char *body, char *response, size_t response_size, char *error, size_t error_size);
