#pragma once

#include <string.h>

typedef void cmd_executor(char **arg_list, size_t arg_count, char *res_msg,
                          size_t res_msg_size);

typedef struct {
    char  *name;
    char  *description;
    size_t arg_count;
    cmd_executor *executor;
} cmd_definition;

const cmd_definition *cmd_get_definition(char *cmd_name);
void cmd_version(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_help(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_foreground(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_install(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_launch(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_kill(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_ftpstatus(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_ftpreset(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_screen(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_battery(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size);
