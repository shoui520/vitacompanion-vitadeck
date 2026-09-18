#include "parser.h"

#include <string.h>
#include <stdbool.h>

// Note: maybe use https://github.com/ryanflannery/str2argv
//       doesn't seem to support positional argument decision trees though


size_t parse_cmd(char *cmd, size_t cmd_size, char **arg_list, size_t arg_max) {
    size_t arg_count = 0;
    char *cursor = cmd;
    char *end = cmd + cmd_size;

    if (cmd == NULL || arg_list == NULL || arg_max == 0)
        return 0;

    while (cursor < end) {
        while (cursor < end &&
               (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) {
            *cursor++ = '\0';
        }

        if (cursor >= end || *cursor == '\0' || *cursor == '\n')
            break;
        if (arg_count == arg_max)
            return PARSE_CMD_TOO_MANY_ARGS;

        arg_list[arg_count++] = cursor;
        while (cursor < end && *cursor != '\0' && *cursor != ' ' &&
               *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            cursor++;
        }

        if (cursor < end) {
            const bool terminal = (*cursor == '\0' || *cursor == '\n');
            *cursor++ = '\0';
            if (terminal)
                break;
        }
    }

    return arg_count;
}
