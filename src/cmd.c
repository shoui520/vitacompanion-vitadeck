#include "cmd.h"

#include "cmd_definitions.h"
#include "parser.h"

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CMD_PORT 1338
#define ARG_MAX (20)
#define CMD_SOCKET_IO_TIMEOUT_US (5 * 1000 * 1000)
#define CMD_START_PENDING 0x7fffffff
#define CMD_START_TIMEOUT_MS 2000

extern int run;
extern int all_is_up;
extern int net_connected;

static SceUID loader_thid = -1;
static volatile int loader_sockfd = -1;
static volatile int active_client_sockfd = -1;
static volatile int loader_start_status = CMD_START_PENDING;

static void close_socket_slot(volatile int *slot)
{
    int sockfd = __sync_lock_test_and_set(slot, -1);
    if (sockfd >= 0)
        sceNetSocketClose(sockfd);
}

static void set_response(char *res_msg, size_t res_msg_size, const char *message)
{
    snprintf(res_msg, res_msg_size, "%s", message);
}

static int configure_socket_io_timeout(int sockfd)
{
    const int timeout = CMD_SOCKET_IO_TIMEOUT_US;
    int ret = sceNetSetsockopt(sockfd, SCE_NET_SOL_SOCKET,
                               SCE_NET_SO_SNDTIMEO,
                               &timeout, sizeof(timeout));
    if (ret < 0)
        return ret;
    return sceNetSetsockopt(sockfd, SCE_NET_SOL_SOCKET,
                            SCE_NET_SO_RCVTIMEO,
                            &timeout, sizeof(timeout));
}

static int receive_command(int sockfd, char *cmd, size_t capacity,
                           bool *too_long)
{
    size_t used = 0;
    *too_long = false;

    while (used < capacity)
    {
        int received = sceNetRecv(sockfd, cmd + used, capacity - used, 0);
        if (received < 0)
            return received;
        if (received == 0)
            break;
        used += (size_t)received;
        if (memchr(cmd, '\n', used) != NULL)
            break;
    }

    if (used == capacity && memchr(cmd, '\n', used) == NULL)
        *too_long = true;
    cmd[used] = '\0';
    return (int)used;
}

void cmd_handle(char* cmd, unsigned int cmd_size, char* res_msg,
                size_t res_msg_size)
{
    char* arg_list[ARG_MAX];

    size_t arg_count = parse_cmd(cmd, cmd_size, arg_list, ARG_MAX);
    if (arg_count == PARSE_CMD_TOO_MANY_ARGS)
    {
        set_response(res_msg, res_msg_size, "Error: Too many arguments.\n");
        return;
    }
    if (arg_count == 0)
    {
        set_response(res_msg, res_msg_size, "Error: Empty command.\n");
        return;
    }

    const cmd_definition* cmd_def = cmd_get_definition(arg_list[0]);

    if (cmd_def == NULL)
    {
        set_response(res_msg, res_msg_size, "Error: Unknown command.\n");
        return;
    }

    if (cmd_def->arg_count != arg_count - 1)
    {
        set_response(res_msg, res_msg_size, "Error: Incorrect number of arguments.\n");
        return;
    }

    cmd_def->executor(arg_list, arg_count, res_msg, res_msg_size);
}

static void send_response(int sockfd, const char *response)
{
    size_t remaining = strlen(response);
    while (remaining > 0)
    {
        int sent = sceNetSend(sockfd, response, remaining, 0);
        if (sent <= 0)
            break;
        response += sent;
        remaining -= (size_t)sent;
    }
}

int cmd_thread(unsigned int args, void* argp)
{
    struct SceNetSockaddrIn loaderaddr = { 0 };

    loader_sockfd = sceNetSocket("vitacompanion_cmd_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (loader_sockfd < 0) {
        loader_start_status = loader_sockfd;
        goto exit_thread;
    }

    loaderaddr.sin_family = SCE_NET_AF_INET;
    loaderaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    loaderaddr.sin_port = sceNetHtons(CMD_PORT);

    int ret = sceNetBind(loader_sockfd, (struct SceNetSockaddr*)&loaderaddr,
                         sizeof(loaderaddr));
    if (ret < 0) {
        loader_start_status = ret;
        goto exit_thread;
    }

    ret = sceNetListen(loader_sockfd, 8);
    if (ret < 0) {
        loader_start_status = ret;
        goto exit_thread;
    }
    loader_start_status = 0;

    while (run && net_connected)
    {
        struct SceNetSockaddrIn clientaddr;
        int client_sockfd;
        unsigned int addrlen = sizeof(clientaddr);

        client_sockfd = sceNetAccept(loader_sockfd, (struct SceNetSockaddr*)&clientaddr, &addrlen);
        if (client_sockfd >= 0)
        {
            __sync_lock_test_and_set(&active_client_sockfd, client_sockfd);
            char cmd[CMD_REQUEST_BYTES + 1] = { 0 };
            char res_msg[CMD_RESPONSE_BYTES] = { 0 };
            bool too_long = false;
            int size;

            ret = configure_socket_io_timeout(client_sockfd);
            if (ret < 0)
            {
                set_response(res_msg, sizeof(res_msg),
                             "Error: Cannot configure command timeout.\n");
            }
            else
            {
                size = receive_command(client_sockfd, cmd, CMD_REQUEST_BYTES,
                                       &too_long);
                if (too_long)
                {
                    set_response(res_msg, sizeof(res_msg), "Error: Command is too long.\n");
                }
                else if (size > 0)
                {
                    cmd_handle(cmd, (unsigned int)size, res_msg, sizeof(res_msg));
                }
                else if (size == 0)
                {
                    set_response(res_msg, sizeof(res_msg), "Error: Empty command.\n");
                }
                else
                {
                    set_response(res_msg, sizeof(res_msg),
                                 "Error: Command receive failed or timed out.\n");
                }
            }

            if (res_msg[0] == '\0')
            {
                set_response(res_msg, sizeof(res_msg), "Error: Empty response.\n");
            }

            send_response(client_sockfd, res_msg);
            close_socket_slot(&active_client_sockfd);
        }
        else
        {
            break;
        }
    }

exit_thread:
    if (loader_start_status == CMD_START_PENDING)
        loader_start_status = -1;
    close_socket_slot(&active_client_sockfd);
    close_socket_slot(&loader_sockfd);
    sceKernelExitThread(0);
    return 0;
}

int cmd_start()
{
    int ret;
    int waited;
    if (loader_thid >= 0)
        return 0;

    loader_start_status = CMD_START_PENDING;
    loader_thid = sceKernelCreateThread("vitacompanion_cmd_thread", cmd_thread, 0x40, 0x10000, 0, 0, NULL);
    if (loader_thid < 0)
        return loader_thid;

    ret = sceKernelStartThread(loader_thid, 0, NULL);
    if (ret < 0)
    {
        sceKernelDeleteThread(loader_thid);
        loader_thid = -1;
        return ret;
    }

    for (waited = 0; waited < CMD_START_TIMEOUT_MS &&
         loader_start_status == CMD_START_PENDING; waited++)
        sceKernelDelayThread(1000);

    if (loader_start_status != 0)
    {
        ret = loader_start_status == CMD_START_PENDING ? -1 : loader_start_status;
        close_socket_slot(&loader_sockfd);
        sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
        sceKernelDeleteThread(loader_thid);
        loader_thid = -1;
        return ret;
    }
    return 0;
}

void cmd_end()
{
    close_socket_slot(&loader_sockfd);
    close_socket_slot(&active_client_sockfd);
    if (loader_thid >= 0)
    {
        sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
        sceKernelDeleteThread(loader_thid);
        loader_thid = -1;
    }
}
