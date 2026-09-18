#pragma once

#include <string.h>

#define PARSE_CMD_TOO_MANY_ARGS ((size_t)-1)

size_t parse_cmd(char *cmd, size_t cmd_size, char **arg_list, size_t arg_max);
