#pragma once

#define CMD_REQUEST_BYTES 256
#define CMD_RESPONSE_BYTES 1024

int cmd_thread(unsigned int args, void* argp);
int cmd_start();
void cmd_end();
