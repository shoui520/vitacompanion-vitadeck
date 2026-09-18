/*
 * Copyright (c) 2015-2016 Sergi Granell (xerpi)
 */

#include "ftpvita.h"
#include "ftpvita_parse.h"
#include "ftpvita_retr.h"
#include "ftpvita_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/syslimits.h>

#include <psp2/kernel/threadmgr.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include <psp2/rtc.h>

#define UNUSED(x) (void)(x)

#define NET_CTL_ERROR_NOT_TERMINATED 0x80412102

#define FTP_PORT 1337
#define NET_INIT_SIZE (64 * 1024)
#define DEFAULT_FILE_BUF_SIZE (4 * 1024 * 1024)

#define FTP_DEFAULT_PATH   "/"

#define MAX_DEVICES 16
#define MAX_CUSTOM_COMMANDS 16
#define MAX_CLIENTS 8
#define SERVER_START_PENDING 0x7fffffff
#define SERVER_START_TIMEOUT_MS 2000
#define SOCKET_IO_TIMEOUT_US (5 * 1000 * 1000)

/* PSVita paths are in the form:
 *     <device name>:<filename in device>
 * for example: cache0:/foo/bar
 * We will send Unix-like paths to the FTP client, like:
 *     /cache0:/foo/bar
 */

typedef struct {
	const char *cmd;
	cmd_dispatch_func func;
} cmd_dispatch_entry;

static struct {
	char name[PATH_MAX];
	int valid;
} device_list[MAX_DEVICES];

static struct {
	const char *cmd;
	cmd_dispatch_func func;
	int valid;
} custom_command_dispatchers[MAX_CUSTOM_COMMANDS];

static void *net_memory = NULL;
static int ftp_initialized = 0;
static unsigned int file_buf_size = DEFAULT_FILE_BUF_SIZE;
static SceNetInAddr vita_addr;
static SceUID server_thid = -1;
static volatile int server_sockfd = -1;
static volatile int server_start_status = SERVER_START_PENDING;
static int number_clients = 0;
static unsigned int next_client_num = 0;
static ftpvita_client_info_t *client_list = NULL;
static SceUID client_list_mtx = -1;
static volatile int transfer_active = 0;
static volatile int reboot_quiesce_requested = 0;
static volatile int socket_timeout_config_failures = 0;

static int netctl_init = -1;
static int net_init = -1;

static void (*info_log_cb)(const char *) = NULL;
static void (*debug_log_cb)(const char *) = NULL;

static void log_func(ftpvita_log_cb_t log_cb, const char *s, ...)
{
	if (log_cb) {
		char buf[256];
		va_list argptr;
		va_start(argptr, s);
		vsnprintf(buf, sizeof(buf), s, argptr);
		va_end(argptr);
		log_cb(buf);
	}
}

#define INFO(...) log_func(info_log_cb, __VA_ARGS__)
#define DEBUG(...) log_func(debug_log_cb, __VA_ARGS__)

#define client_send_ctrl_msg(cl, str) \
	sceNetSend(cl->ctrl_sockfd, str, strlen(str), 0)

static int client_send_all(int sockfd, const void *buffer, unsigned int length)
{
	const unsigned char *cursor = buffer;
	unsigned int remaining = length;
	while (remaining > 0) {
		const int sent = sceNetSend(sockfd, cursor, remaining, 0);
		if (sent <= 0)
			return sent < 0 ? sent : -1;
		cursor += (unsigned int)sent;
		remaining -= (unsigned int)sent;
	}
	return 0;
}

static inline int client_send_data_msg(ftpvita_client_info_t *client, const char *str)
{
	const int sockfd = client->data_con_type == FTP_DATA_CONNECTION_ACTIVE ?
		client->data_sockfd : client->pasv_sockfd;
	return client_send_all(sockfd, str, (unsigned int)strlen(str));
}

static inline int client_recv_data_raw(ftpvita_client_info_t *client, void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return sceNetRecv(client->data_sockfd, buf, len, 0);
	} else {
		return sceNetRecv(client->pasv_sockfd, buf, len, 0);
	}
}

static inline const char *get_vita_path(const char *path)
{
	if (strlen(path) > 1)
		/* /cache0:/foo/bar -> cache0:/foo/bar */
		return &path[1];
	else
		return NULL;
}

static int file_exists(const char *path)
{
	SceIoStat stat;
	return (sceIoGetstat(path, &stat) >= 0);
}

static void close_socket(int *sockfd)
{
	if (*sockfd >= 0) {
		sceNetSocketClose(*sockfd);
		*sockfd = -1;
	}
}

static int configure_socket_io_timeout(int sockfd)
{
	const int timeout = SOCKET_IO_TIMEOUT_US;
	int send_result;
	int receive_result;
	if (sockfd < 0)
		return -1;
	send_result = sceNetSetsockopt(sockfd, SCE_NET_SOL_SOCKET,
		SCE_NET_SO_SNDTIMEO,
		&timeout, sizeof(timeout));
	receive_result = sceNetSetsockopt(sockfd, SCE_NET_SOL_SOCKET,
		SCE_NET_SO_RCVTIMEO,
		&timeout, sizeof(timeout));
	if (send_result < 0 || receive_result < 0) {
		__sync_fetch_and_add(&socket_timeout_config_failures, 1);
		INFO("Socket %d timeout configuration failed: send=0x%08X receive=0x%08X\n",
			sockfd, send_result, receive_result);
		return send_result < 0 ? send_result : receive_result;
	}
	return 0;
}

static void close_server_socket(void)
{
	int sockfd = __sync_lock_test_and_set(&server_sockfd, -1);
	if (sockfd >= 0)
		sceNetSocketClose(sockfd);
}

static void client_close_data_connection(ftpvita_client_info_t *client);

/* SceShell and VitaCompanion share the system-reserved CPU3.  Concurrent bulk
 * transfers provide no useful throughput to the development harness, but can
 * pin that core badly enough that a requested cold reset powers the Vita down
 * without bringing it back.  Admit one data transfer at a time and make the
 * reboot interlock close the admission race before it inspects live clients. */
static int client_begin_transfer(void)
{
	if (__sync_fetch_and_add(&reboot_quiesce_requested, 0) != 0)
		return 0;
	if (!__sync_bool_compare_and_swap(&transfer_active, 0, 1))
		return 0;
	__sync_synchronize();
	if (__sync_fetch_and_add(&reboot_quiesce_requested, 0) != 0) {
		__sync_lock_release(&transfer_active);
		return 0;
	}
	return 1;
}

static void client_end_transfer(void)
{
	__sync_lock_release(&transfer_active);
}

static void cmd_NOOP_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "200 No operation ;)" FTPVITA_EOL);
}

static void cmd_USER_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "331 Username OK, need password b0ss." FTPVITA_EOL);
}

static void cmd_PASS_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "230 User logged in!" FTPVITA_EOL);
}

static void cmd_QUIT_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "221 Goodbye senpai :'(" FTPVITA_EOL);
	client->quit_requested = 1;
}

static void cmd_SYST_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "215 UNIX Type: L8" FTPVITA_EOL);
}

static void cmd_PASV_func(ftpvita_client_info_t *client)
{
	int ret;

	char cmd[512];
	unsigned int namelen;
	SceNetSockaddrIn picked;

	/* A new PASV command replaces any unconsumed passive listener.  Python's
	 * ftplib legitimately retries with another transfer command after a server
	 * rejects an unsupported command; leaking the old listener eventually
	 * exhausts SceShell's socket resources. */
	client_close_data_connection(client);

	/* Create data mode socket name */
	char data_socket_name[64];
	sprintf(data_socket_name, "FTPVita_client_%i_data_socket",
		client->num);

	/* Create the data socket */
	client->data_sockfd = sceNetSocket(data_socket_name,
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	DEBUG("PASV data socket fd: %d\n", client->data_sockfd);
	if (client->data_sockfd < 0) {
		client_send_ctrl_msg(client, "425 Can't open passive connection." FTPVITA_EOL);
		return;
	}
	configure_socket_io_timeout(client->data_sockfd);

	/* Fill the data socket address */
	client->data_sockaddr.sin_family = SCE_NET_AF_INET;
	client->data_sockaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	/* Let the PSVita choose a port */
	client->data_sockaddr.sin_port = sceNetHtons(0);

	/* Bind the data socket address to the data socket */
	ret = sceNetBind(client->data_sockfd,
		(SceNetSockaddr *)&client->data_sockaddr,
		sizeof(client->data_sockaddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);
	if (ret < 0) {
		close_socket(&client->data_sockfd);
		client_send_ctrl_msg(client, "425 Can't bind passive connection." FTPVITA_EOL);
		return;
	}

	/* Start listening */
	ret = sceNetListen(client->data_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);
	if (ret < 0) {
		close_socket(&client->data_sockfd);
		client_send_ctrl_msg(client, "425 Can't listen for passive connection." FTPVITA_EOL);
		return;
	}

	/* Get the port that the PSVita has chosen */
	namelen = sizeof(picked);
	ret = sceNetGetsockname(client->data_sockfd, (SceNetSockaddr *)&picked,
		&namelen);
	if (ret < 0) {
		close_socket(&client->data_sockfd);
		client_send_ctrl_msg(client, "425 Can't inspect passive connection." FTPVITA_EOL);
		return;
	}

	DEBUG("PASV mode port: 0x%04X\n", picked.sin_port);

	/* Build the command */
	sprintf(cmd, "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu)" FTPVITA_EOL,
		(vita_addr.s_addr >> 0) & 0xFF,
		(vita_addr.s_addr >> 8) & 0xFF,
		(vita_addr.s_addr >> 16) & 0xFF,
		(vita_addr.s_addr >> 24) & 0xFF,
		(picked.sin_port >> 0) & 0xFF,
		(picked.sin_port >> 8) & 0xFF);

	client_send_ctrl_msg(client, cmd);

	/* Set the data connection type to passive! */
	client->data_con_type = FTP_DATA_CONNECTION_PASSIVE;
}

static void cmd_PORT_func(ftpvita_client_info_t *client)
{
	unsigned int tuple[6];
	unsigned int *data_ip = tuple;
	unsigned int porthi;
	unsigned int portlo;
	unsigned short data_port;
	char ip_str[16];
	SceNetInAddr data_addr;

	if (!ftpvita_parse_port_tuple(client->recv_cmd_args, tuple)) {
		client_send_ctrl_msg(client, "501 Invalid PORT parameters." FTPVITA_EOL);
		return;
	}
	porthi = tuple[4];
	portlo = tuple[5];

	client_close_data_connection(client);

	data_port = portlo + porthi*256;

	/* Convert to an X.X.X.X IP string */
	sprintf(ip_str, "%d.%d.%d.%d",
		data_ip[0], data_ip[1], data_ip[2], data_ip[3]);

	/* Convert the IP to a SceNetInAddr */
	sceNetInetPton(SCE_NET_AF_INET, ip_str, &data_addr);

	DEBUG("PORT connection to client's IP: %s Port: %d\n", ip_str, data_port);

	/* Create data mode socket name */
	char data_socket_name[64];
	sprintf(data_socket_name, "FTPVita_client_%i_data_socket",
		client->num);

	/* Create data mode socket */
	client->data_sockfd = sceNetSocket(data_socket_name,
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	DEBUG("Client %i data socket fd: %d\n", client->num,
		client->data_sockfd);
	if (client->data_sockfd < 0) {
		client_send_ctrl_msg(client, "425 Can't open active connection." FTPVITA_EOL);
		return;
	}
	configure_socket_io_timeout(client->data_sockfd);

	/* Prepare socket address for the data connection */
	client->data_sockaddr.sin_family = SCE_NET_AF_INET;
	client->data_sockaddr.sin_addr = data_addr;
	client->data_sockaddr.sin_port = sceNetHtons(data_port);

	/* Set the data connection type to active! */
	client->data_con_type = FTP_DATA_CONNECTION_ACTIVE;

	client_send_ctrl_msg(client, "200 PORT command successful!" FTPVITA_EOL);
}

static int client_open_data_connection(ftpvita_client_info_t *client)
{
	int ret;

	unsigned int addrlen;
	if (client->data_con_type == FTP_DATA_CONNECTION_NONE ||
		client->data_sockfd < 0) {
		return -1;
	}

	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		/* Connect to the client using the data socket */
		ret = sceNetConnect(client->data_sockfd,
			(SceNetSockaddr *)&client->data_sockaddr,
			sizeof(client->data_sockaddr));

		DEBUG("sceNetConnect(): 0x%08X\n", ret);
		return ret;
	} else {
		/* Listen to the client using the data socket */
		addrlen = sizeof(client->pasv_sockaddr);
		client->pasv_sockfd = sceNetAccept(client->data_sockfd,
			(SceNetSockaddr *)&client->pasv_sockaddr,
			&addrlen);
		DEBUG("PASV client fd: 0x%08X\n", client->pasv_sockfd);
		configure_socket_io_timeout(client->pasv_sockfd);
		return client->pasv_sockfd < 0 ? client->pasv_sockfd : 0;
	}
}

static void client_close_data_connection(ftpvita_client_info_t *client)
{
	close_socket(&client->data_sockfd);
	close_socket(&client->pasv_sockfd);
	client->data_con_type = FTP_DATA_CONNECTION_NONE;
}

static int gen_list_format(char *out, int n, int dir, const SceIoStat *stat, const char *filename)
{
	static const char num_to_month[][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	char yt[6];
	SceDateTime cdt;
	sceRtcGetCurrentClockLocalTime(&cdt);

	if  (cdt.year == stat->st_mtime.year) {
		snprintf(yt, sizeof(yt), "%02d:%02d", stat->st_mtime.hour, stat->st_mtime.minute);
	}
	else {
		snprintf(yt, sizeof(yt), "%04d", stat->st_mtime.year);
	}

	return snprintf(out, n,
		"%c%s 1 vita vita %u %s %-2d %s %s" FTPVITA_EOL,
		dir ? 'd' : '-',
		dir ? "rwxr-xr-x" : "rw-r--r--",
		(unsigned int) stat->st_size,
		num_to_month[stat->st_mtime.month<=0?0:(stat->st_mtime.month-1)%12],
		stat->st_mtime.day,
		yt,
		filename);
}

static void send_LIST(ftpvita_client_info_t *client, const char *path)
{
	int i;
	char buffer[512];
	SceUID dir;
	SceIoDirent dirent;
	SceIoStat stat;
	char *devname;
	int send_devices = 0;
	int transfer_ok = 1;

	if (!client_begin_transfer()) {
		client_close_data_connection(client);
		client_send_ctrl_msg(client,
			"425 Another FTP transfer is active." FTPVITA_EOL);
		return;
	}

	/* "/" path is a special case, if we are here we have
	 * to send the list of devices (aka mountpoints). */
	if (strcmp(path, "/") == 0) {
		send_devices = 1;
	}

	if (!send_devices) {
		dir = sceIoDopen(get_vita_path(path));
		if (dir < 0) {
			client_close_data_connection(client);
			client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
			client_end_transfer();
			return;
		}
	}

	if (client_open_data_connection(client) < 0) {
		if (!send_devices)
			sceIoDclose(dir);
		client_close_data_connection(client);
		client_send_ctrl_msg(client, "425 Can't open data connection." FTPVITA_EOL);
		client_end_transfer();
		return;
	}

	client_send_ctrl_msg(client, "150 Opening ASCII mode data transfer for LIST." FTPVITA_EOL);

	if (send_devices) {
		for (i = 0; i < MAX_DEVICES; i++) {
			if (device_list[i].valid) {
				devname = device_list[i].name;
				if (sceIoGetstat(devname, &stat) >= 0) {
					gen_list_format(buffer, sizeof(buffer),	1, &stat, devname);
					if (client_send_data_msg(client, buffer) < 0) {
						transfer_ok = 0;
						break;
					}
				}
			}
		}
	} else {
		memset(&dirent, 0, sizeof(dirent));

		while (sceIoDread(dir, &dirent) > 0) {
			gen_list_format(buffer, sizeof(buffer), SCE_S_ISDIR(dirent.d_stat.st_mode),
				&dirent.d_stat, dirent.d_name);
			if (client_send_data_msg(client, buffer) < 0) {
				transfer_ok = 0;
				break;
			}
			memset(&dirent, 0, sizeof(dirent));
			memset(buffer, 0, sizeof(buffer));
		}

		sceIoDclose(dir);
	}

	DEBUG("Done sending LIST\n");

	client_close_data_connection(client);
	client_send_ctrl_msg(client, transfer_ok ?
		"226 Transfer complete." FTPVITA_EOL :
		"426 Connection closed; transfer aborted." FTPVITA_EOL);
	client_end_transfer();
}

static void cmd_LIST_func(ftpvita_client_info_t *client)
{
	char list_path[PATH_MAX];
	int list_cur_path = 1;

	if (ftpvita_copy_command_argument(client->recv_cmd_args, list_path,
		sizeof(list_path)) && file_exists(get_vita_path(list_path)))
		list_cur_path = 0;

	if (list_cur_path)
		send_LIST(client, client->cur_path);
	else
		send_LIST(client, list_path);
}

static void cmd_PWD_func(ftpvita_client_info_t *client)
{
	char msg[PATH_MAX];
	snprintf(msg, sizeof(msg), "257 \"%s\" is the current directory." FTPVITA_EOL, client->cur_path);
	client_send_ctrl_msg(client, msg);
}

static int path_is_at_root(const char *path)
{
	return strrchr(path, '/') == (path + strlen(path) - 1);
}

static void dir_up(char *path)
{
	char *pch;
	size_t len_in = strlen(path);
	if (len_in == 1) {
		strcpy(path, "/");
		return;
	}
	if (path_is_at_root(path)) { /* Case root of the device (/foo0:/) */
		strcpy(path, "/");
	} else {
		pch = strrchr(path, '/');
		size_t s = len_in - (pch - path);
		memset(pch, '\0', s);
		/* If the path is like: /foo: add slash */
		if (strrchr(path, '/') == path)
			strcat(path, "/");
	}
}

static void cmd_CWD_func(ftpvita_client_info_t *client)
{
	char cmd_path[PATH_MAX];
	char tmp_path[PATH_MAX];
	SceUID pd;

	if (!ftpvita_copy_command_argument(client->recv_cmd_args, cmd_path,
		sizeof(cmd_path))) {
		client_send_ctrl_msg(client, "500 Syntax error, command unrecognized." FTPVITA_EOL);
	} else {
		if (strcmp(cmd_path, "/") == 0) {
			strcpy(client->cur_path, cmd_path);
		} else  if (strcmp(cmd_path, "..") == 0) {
			dir_up(client->cur_path);
		} else {
			if (cmd_path[0] == '/') { /* Full path */
				strcpy(tmp_path, cmd_path);
			} else { /* Change dir relative to current dir */
				/* If we are at the root of the device, don't add
				 * an slash to add new path */
				if (path_is_at_root(client->cur_path))
					snprintf(tmp_path, sizeof(tmp_path), "%s%s", client->cur_path, cmd_path);
				else
					snprintf(tmp_path, sizeof(tmp_path), "%s/%s", client->cur_path, cmd_path);
			}

			/* If the path is like: /foo: add an slash */
			if (strrchr(tmp_path, '/') == tmp_path)
				strcat(tmp_path, "/");

			/* If the path is not "/", check if it exists */
			if (strcmp(tmp_path, "/") != 0) {
				/* Check if the path exists */
				pd = sceIoDopen(get_vita_path(tmp_path));
				if (pd < 0) {
					client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
					return;
				}
				sceIoDclose(pd);
			}
			strcpy(client->cur_path, tmp_path);
		}
		client_send_ctrl_msg(client, "250 Requested file action okay, completed." FTPVITA_EOL);
	}
}

static void cmd_TYPE_func(ftpvita_client_info_t *client)
{
	const char data_type = client->recv_cmd_args ? client->recv_cmd_args[0] : '\0';

	if (data_type != '\0' && data_type != '\r' && data_type != '\n') {
		switch(data_type) {
		case 'A':
		case 'I':
			client_send_ctrl_msg(client, "200 Okay" FTPVITA_EOL);
			break;
		case 'E':
		case 'L':
		default:
			client_send_ctrl_msg(client, "504 Error: bad parameters?" FTPVITA_EOL);
			break;
		}
	} else {
		client_send_ctrl_msg(client, "504 Error: bad parameters?" FTPVITA_EOL);
	}
}

static void cmd_CDUP_func(ftpvita_client_info_t *client)
{
	dir_up(client->cur_path);
	client_send_ctrl_msg(client, "200 Command okay." FTPVITA_EOL);
}

typedef struct {
	SceUID fd;
	int sockfd;
} retr_io_t;

static int retr_read(void *context, void *buffer, unsigned int length)
{
	const retr_io_t *io = context;
	return sceIoRead(io->fd, buffer, length);
}

static int retr_send(void *context, const void *buffer, unsigned int length,
	int *net_error)
{
	const retr_io_t *io = context;
	const int sent = sceNetSend(io->sockfd, buffer, length, 0);
	/* libnet Reference: sceNetSend + sce_net_errno. Capture the thread-local
	 * error immediately, before cleanup/control sends can overwrite it. */
	*net_error = sent < 0 ? *sceNetErrnoLoc() : 0;
	return sent;
}

static void send_file(ftpvita_client_info_t *client, const char *path)
{
	unsigned char *buffer;
	SceUID fd;
	const unsigned int restore_point = client->restore_point;
	client->restore_point = 0;

	if (!client_begin_transfer()) {
		client_close_data_connection(client);
		client_send_ctrl_msg(client,
			"425 Another FTP transfer is active." FTPVITA_EOL);
		return;
	}

	DEBUG("Opening: %s\n", path);

	if ((fd = sceIoOpen(path, SCE_O_RDONLY, 0777)) >= 0) {

		/* Use the opened descriptor, not a racing path stat. SDK iofilemgr.h:
		 * sceIoLseek returns a 64-bit position or a sign-extended error. */
		const SceOff extent = sceIoLseek(fd, 0, SCE_SEEK_END);
		const SceOff position = extent >= 0 && restore_point <= extent ?
			sceIoLseek(fd, restore_point, SCE_SEEK_SET) : -1;
		if (extent < 0 || restore_point > extent || position != restore_point) {
			char reply[192];
			snprintf(reply, sizeof(reply),
				"550 RETR seek failed start=%u extent=%lld position=%lld." FTPVITA_EOL,
				restore_point, (long long)extent, (long long)position);
			sceIoClose(fd);
			client_close_data_connection(client);
			client_send_ctrl_msg(client, reply);
			client_end_transfer();
			return;
		}

		buffer = malloc(file_buf_size);
		if (buffer == NULL) {
			sceIoClose(fd);
			client_close_data_connection(client);
			client_send_ctrl_msg(client, "550 Could not allocate memory." FTPVITA_EOL);
			client_end_transfer();
			return;
		}

		if (client_open_data_connection(client) < 0) {
			sceIoClose(fd);
			free(buffer);
			client_close_data_connection(client);
			client_send_ctrl_msg(client, "425 Can't open data connection." FTPVITA_EOL);
			client_end_transfer();
			return;
		}
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer." FTPVITA_EOL);

		retr_io_t io = {fd, client->data_con_type == FTP_DATA_CONNECTION_ACTIVE ?
			client->data_sockfd : client->pasv_sockfd};
		const ftpvita_retr_result_t result = ftpvita_retr_extent(&io,
			retr_read, retr_send, buffer, file_buf_size, restore_point, (uint64_t)extent);

		sceIoClose(fd);
		free(buffer);
		client_close_data_connection(client);
		if (strcmp(result.stage, "complete") == 0) {
			client_send_ctrl_msg(client, "226 Transfer completed." FTPVITA_EOL);
		} else {
			char reply[256];
			snprintf(reply, sizeof(reply),
				"426 RETR stage=%s code=0x%08X net_errno=%d start=%u "
				"extent=%llu read=%llu sent=%llu." FTPVITA_EOL,
				result.stage, (unsigned int)result.code, result.net_errno,
				restore_point, (unsigned long long)extent,
				(unsigned long long)result.bytes_read,
				(unsigned long long)result.bytes_sent);
			INFO("%s", reply);
			client_send_ctrl_msg(client, reply);
		}

	} else {
		client_close_data_connection(client);
		client_send_ctrl_msg(client, "550 File not found." FTPVITA_EOL);
	}
	client_end_transfer();
}

/* This function generates an FTP full-path with the input path (relative or absolute)
 * from RETR, STOR, DELE, RMD, MKD, RNFR and RNTO commands */
static void gen_ftp_fullpath(ftpvita_client_info_t *client, char *path, size_t path_size)
{
	char cmd_path[PATH_MAX];
	if (!ftpvita_copy_command_argument(client->recv_cmd_args, cmd_path,
		sizeof(cmd_path))) {
		if (path_size > 0)
			path[0] = '\0';
		return;
	}

	if (cmd_path[0] == '/') {
		/* Full path */
		snprintf(path, path_size, "%s", cmd_path);
	} else {
		if (strlen(cmd_path) >= 5 && cmd_path[3] == ':' && cmd_path[4] == '/') {
			/* Case "ux0:/foo */
			snprintf(path, path_size, "/%s", cmd_path);
		} else {
			/* The file is relative to current dir, so
			 * append the file to the current path */
			snprintf(path, path_size, "%s/%s", client->cur_path, cmd_path);
		}
	}
}

static void cmd_RETR_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	send_file(client, get_vita_path(dest_path));
}

typedef struct {
	ftpvita_client_info_t *client;
	SceUID fd;
} store_io_t;

static int store_receive(void *context, void *data, unsigned int length,
	int *net_error)
{
	store_io_t *store = context;
	const int received = client_recv_data_raw(store->client, data, length);
	*net_error = received < 0 ? *sceNetErrnoLoc() : 0;
	return received;
}

static int store_write(void *context, const void *data, unsigned int length)
{
	store_io_t *store = context;
	return sceIoWrite(store->fd, data, length);
}

static void receive_file(ftpvita_client_info_t *client, const char *path)
{
	unsigned char *buffer;
	SceUID fd;

	if (!client_begin_transfer()) {
		client_close_data_connection(client);
		client_send_ctrl_msg(client,
			"425 Another FTP transfer is active." FTPVITA_EOL);
		return;
	}

	DEBUG("Opening: %s\n", path);

	int mode = SCE_O_CREAT | SCE_O_RDWR;
	/* if we resume broken - append missing part
	 * else - overwrite file */
	if (client->restore_point) {
		mode = mode | SCE_O_APPEND;
	}
	else {
		mode = mode | SCE_O_TRUNC;
	}

	if ((fd = sceIoOpen(path, mode, 0777)) >= 0) {

		buffer = malloc(file_buf_size);
		if (buffer == NULL) {
			sceIoClose(fd);
			client_close_data_connection(client);
			client_send_ctrl_msg(client, "550 Could not allocate memory." FTPVITA_EOL);
			client_end_transfer();
			return;
		}

		if (client_open_data_connection(client) < 0) {
			sceIoClose(fd);
			free(buffer);
			client_close_data_connection(client);
			client_send_ctrl_msg(client, "425 Can't open data connection." FTPVITA_EOL);
			client_end_transfer();
			return;
		}
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer." FTPVITA_EOL);

		store_io_t io = {client, fd};
		const ftpvita_store_result_t result = ftpvita_store_stream(&io,
			store_receive, store_write, buffer, file_buf_size);

		sceIoClose(fd);
		free(buffer);
		client->restore_point = 0;
		if (strcmp(result.stage, "complete") == 0) {
			client_send_ctrl_msg(client, "226 Transfer completed." FTPVITA_EOL);
		} else {
			char reply[256];
			snprintf(reply, sizeof(reply),
				"426 STOR stage=%s code=0x%08X net_errno=%d "
				"received=%llu written=%llu." FTPVITA_EOL,
				result.stage, (unsigned int)result.code, result.net_errno,
				(unsigned long long)result.bytes_received,
				(unsigned long long)result.bytes_written);
			INFO("%s", reply);
			client_send_ctrl_msg(client, reply);
		}
		client_close_data_connection(client);

	} else {
		client_close_data_connection(client);
		client_send_ctrl_msg(client, "550 File not found." FTPVITA_EOL);
	}
	client_end_transfer();
}

static void cmd_STOR_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	receive_file(client, get_vita_path(dest_path));
}

static void delete_file(ftpvita_client_info_t *client, const char *path)
{
	DEBUG("Deleting: %s\n", path);

	if (sceIoRemove(path) >= 0) {
		client_send_ctrl_msg(client, "250 File deleted." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the file." FTPVITA_EOL);
	}
}

static void cmd_DELE_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	delete_file(client, get_vita_path(dest_path));
}

static void delete_dir(ftpvita_client_info_t *client, const char *path)
{
	int ret;
	DEBUG("Deleting: %s\n", path);
	ret = sceIoRmdir(path);
	if (ret >= 0) {
		client_send_ctrl_msg(client, "250 Directory deleted." FTPVITA_EOL);
	} else if (ret == 0x8001005A) { /* DIRECTORY_IS_NOT_EMPTY */
		client_send_ctrl_msg(client, "550 Directory is not empty." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the directory." FTPVITA_EOL);
	}
}

static void cmd_RMD_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	delete_dir(client, get_vita_path(dest_path));
}

static void create_dir(ftpvita_client_info_t *client, const char *path)
{
	DEBUG("Creating: %s\n", path);

	if (sceIoMkdir(path, 0777) >= 0) {
		client_send_ctrl_msg(client, "257 Directory created." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "550 Could not create the directory." FTPVITA_EOL);
	}
}

static void cmd_MKD_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	create_dir(client, get_vita_path(dest_path));
}

static void cmd_RNFR_func(ftpvita_client_info_t *client)
{
	char path_src[PATH_MAX];
	const char *vita_path_src;
	/* Get the origin filename */
	gen_ftp_fullpath(client, path_src, sizeof(path_src));
	vita_path_src = get_vita_path(path_src);

	/* Check if the file exists */
	if (!file_exists(vita_path_src)) {
		client_send_ctrl_msg(client, "550 The file doesn't exist." FTPVITA_EOL);
		return;
	}
	/* The file to be renamed is the received path */
	strcpy(client->rename_path, vita_path_src);
	client_send_ctrl_msg(client, "350 I need the destination name b0ss." FTPVITA_EOL);
}

static void cmd_RNTO_func(ftpvita_client_info_t *client)
{
	char path_dst[PATH_MAX];
	const char *vita_path_dst;
	/* Get the destination filename */
	gen_ftp_fullpath(client, path_dst,sizeof(path_dst));
	vita_path_dst = get_vita_path(path_dst);

	DEBUG("Renaming: %s to %s\n", client->rename_path, vita_path_dst);

	if (sceIoRename(client->rename_path, vita_path_dst) < 0) {
		client_send_ctrl_msg(client, "550 Error renaming the file." FTPVITA_EOL);
		return;
	}

	client_send_ctrl_msg(client, "250 Rename completed." FTPVITA_EOL);
}

static void cmd_SIZE_func(ftpvita_client_info_t *client)
{
	SceIoStat stat;
	char path[PATH_MAX];
	char cmd[64];
	/* Get the filename to retrieve its size */
	gen_ftp_fullpath(client, path, sizeof(path));

	/* Check if the file exists */
	if (sceIoGetstat(get_vita_path(path), &stat) < 0) {
		client_send_ctrl_msg(client, "550 The file doesn't exist." FTPVITA_EOL);
		return;
	}
	/* Send the size of the file */
	sprintf(cmd, "213 %lld" FTPVITA_EOL, stat.st_size);
	client_send_ctrl_msg(client, cmd);
}

static void cmd_REST_func(ftpvita_client_info_t *client)
{
	char cmd[64];
	unsigned int restore_point = 0;
	if (!ftpvita_parse_rest_offset(client->recv_cmd_args, &restore_point)) {
		client_send_ctrl_msg(client, "501 Invalid REST offset." FTPVITA_EOL);
		return;
	}
	client->restore_point = restore_point;
	snprintf(cmd, sizeof(cmd), "350 Resuming at %u" FTPVITA_EOL,
		client->restore_point);
	client_send_ctrl_msg(client, cmd);
}

static void cmd_FEAT_func(ftpvita_client_info_t *client)
{
	/*So client would know that we support resume */
	client_send_ctrl_msg(client, "211-extensions" FTPVITA_EOL);
	client_send_ctrl_msg(client, " REST STREAM" FTPVITA_EOL);
	client_send_ctrl_msg(client, " UTF8" FTPVITA_EOL);
	client_send_ctrl_msg(client, "211 end" FTPVITA_EOL);
}

static void cmd_OPTS_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "501 bad OPTS" FTPVITA_EOL);
}

static void cmd_APPE_func(ftpvita_client_info_t *client)
{
	/* set restore point to not 0
	restore point numeric value only matters if we RETR file from vita.
	If we STOR or APPE, it is only used to indicate that we want to resume
	a broken transfer */
	client->restore_point = -1;
	char dest_path[PATH_MAX];
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	receive_file(client, get_vita_path(dest_path));
}

#define add_entry(name) {#name, cmd_##name##_func}
static const cmd_dispatch_entry cmd_dispatch_table[] = {
	add_entry(NOOP),
	add_entry(USER),
	add_entry(PASS),
	add_entry(QUIT),
	add_entry(SYST),
	add_entry(PASV),
	add_entry(PORT),
	add_entry(LIST),
	add_entry(PWD),
	add_entry(CWD),
	add_entry(TYPE),
	add_entry(CDUP),
	add_entry(RETR),
	add_entry(STOR),
	add_entry(DELE),
	add_entry(RMD),
	add_entry(MKD),
	add_entry(RNFR),
	add_entry(RNTO),
	add_entry(SIZE),
	add_entry(REST),
	add_entry(FEAT),
	add_entry(OPTS),
	add_entry(APPE),
	{NULL, NULL}
};

static cmd_dispatch_func get_dispatch_func(const char *cmd)
{
	int i;
	for(i = 0; cmd_dispatch_table[i].cmd && cmd_dispatch_table[i].func; i++) {
		if (strcmp(cmd, cmd_dispatch_table[i].cmd) == 0) {
			return cmd_dispatch_table[i].func;
		}
	}
	// Check for custom commands
	for(i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (custom_command_dispatchers[i].valid) {
			if (strcmp(cmd, custom_command_dispatchers[i].cmd) == 0) {
				return custom_command_dispatchers[i].func;
			}
		}
	}
	return NULL;
}

static int client_list_add(ftpvita_client_info_t *client)
{
	/* Add the client at the front of the client list */
	sceKernelLockMutex(client_list_mtx, 1, NULL);
	if (reboot_quiesce_requested || number_clients >= MAX_CLIENTS) {
		sceKernelUnlockMutex(client_list_mtx, 1);
		return 0;
	}

	if (client_list == NULL) { /* List is empty */
		client_list = client;
		client->prev = NULL;
		client->next = NULL;
	} else {
		client->next = client_list;
		client_list->prev = client;
		client->prev = NULL;
		client_list = client;
	}
	client->restore_point = 0;
	client->in_client_list = 1;
	number_clients++;

	sceKernelUnlockMutex(client_list_mtx, 1);
	return 1;
}

/* Returns non-zero when the caller removed itself from the live list and
 * therefore owns deletion of its kernel thread object.  ftpvita_fini()
 * detaches clients before waiting for them; those clients must only exit so
 * the teardown owner can perform the matching sceKernelDeleteThread(). */
static int client_list_delete(ftpvita_client_info_t *client)
{
	/* Remove the client from the client list */
	sceKernelLockMutex(client_list_mtx, 1, NULL);
	if (!client->in_client_list) {
		sceKernelUnlockMutex(client_list_mtx, 1);
		return 0;
	}

	if (client->prev) {
		client->prev->next = client->next;
	}
	if (client->next) {
		client->next->prev = client->prev;
	}
	if (client == client_list) {
		client_list = client->next;
	}

	client->next = NULL;
	client->prev = NULL;
	client->in_client_list = 0;
	if (number_clients > 0)
		number_clients--;

	sceKernelUnlockMutex(client_list_mtx, 1);
	return 1;
}

static void client_list_thread_end()
{
	ftpvita_client_info_t *it;
	SceUID client_thids[MAX_CLIENTS];
	int client_count = 0;
	const int data_abort_flags = SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION |
				SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION;

	if (client_list_mtx < 0)
		return;

	sceKernelLockMutex(client_list_mtx, 1, NULL);

	/* Abort while holding the list lock, then detach every client.  Waiting
	 * under this mutex deadlocks with the client's normal list deletion path. */
	for (it = client_list; it && client_count < MAX_CLIENTS; it = it->next) {
		client_thids[client_count++] = it->thid;

		/* Abort the client's control socket, only abort
		 * receiving data so we can still send control messages */
		sceNetSocketAbort(it->ctrl_sockfd,
			SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION);

		/* If there's an open data connection, abort it */
		if (it->data_con_type != FTP_DATA_CONNECTION_NONE) {
			if (it->data_sockfd >= 0)
				sceNetSocketAbort(it->data_sockfd, data_abort_flags);
			if (it->data_con_type == FTP_DATA_CONNECTION_PASSIVE &&
				it->pasv_sockfd >= 0) {
				sceNetSocketAbort(it->pasv_sockfd, data_abort_flags);
			}
		}
		it->in_client_list = 0;
	}
	client_list = NULL;
	number_clients = 0;
	transfer_active = 0;

	sceKernelUnlockMutex(client_list_mtx, 1);

	for (int i = 0; i < client_count; i++) {
		sceKernelWaitThreadEnd(client_thids[i], NULL, NULL);
		sceKernelDeleteThread(client_thids[i]);
	}
}

static int client_thread(SceSize args, void *argp)
{
	char cmd[16];
	cmd_dispatch_func dispatch_func;
	ftpvita_client_info_t *client = *(ftpvita_client_info_t **)argp;

	DEBUG("Client thread %i started!\n", client->num);

	client_send_ctrl_msg(client, "220 FTPVita Server ready." FTPVITA_EOL);

	while (1) {
		memset(client->recv_buffer, 0, sizeof(client->recv_buffer));

		client->n_recv = sceNetRecv(client->ctrl_sockfd, client->recv_buffer,
			sizeof(client->recv_buffer) - 1, 0);
		if (client->n_recv > 0) {
			client->recv_buffer[client->n_recv] = '\0';
			DEBUG("Received %i bytes from client number %i:\n",
				client->n_recv, client->num);

			INFO("\t%i> %s", client->num, client->recv_buffer);

			/* Parse command and argument without Vita PDCLib scanf. */
			if (!ftpvita_parse_command_line(client->recv_buffer, cmd,
				sizeof(cmd), &client->recv_cmd_args)) {
				client_send_ctrl_msg(client, "500 Empty command." FTPVITA_EOL);
				continue;
			}

			/* Wait 1 ms before sending any data */
			sceKernelDelayThread(1*1000);

			if ((dispatch_func = get_dispatch_func(cmd))) {
				dispatch_func(client);
				if (client->quit_requested) {
					INFO("Client %i completed QUIT.\n", client->num);
					break;
				}
			} else {
				client_send_ctrl_msg(client, "502 Sorry, command not implemented. :(" FTPVITA_EOL);
			}

		} else if (client->n_recv == 0) {
			/* Value 0 means connection closed by the remote peer */
			INFO("Connection closed by the client %i.\n", client->num);
			break;
		} else if (client->n_recv == SCE_NET_ERROR_EINTR) {
			/* Socket aborted (ftpvita_fini() called) */
			INFO("Client %i socket aborted.\n", client->num);
			break;
		} else {
			/* Other errors */
			INFO("Client %i socket error: 0x%08X\n", client->num, client->n_recv);
			break;
		}
	}
	const int delete_own_thread = client_list_delete(client);

	/* Close the client's socket */
	close_socket(&client->ctrl_sockfd);

	/* If there's an open data connection, close it */
	client_close_data_connection(client);

	DEBUG("Client thread %i exiting!\n", client->num);

	free(client);

	if (delete_own_thread)
		sceKernelExitDeleteThread(0);

	/* ftpvita_fini() detached this client and owns the corresponding wait and
	 * delete.  Exiting without deleting keeps that thread ID valid for it. */
	sceKernelExitThread(0);
	return 0;
}

static int server_thread(SceSize args, void *argp)
{
	int ret;

	SceNetSockaddrIn serveraddr = {0};

	DEBUG("Server thread started!\n");

	/* Create server socket */
	server_sockfd = sceNetSocket("FTPVita_server_sock",
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	DEBUG("Server socket fd: %d\n", server_sockfd);
	if (server_sockfd < 0) {
		INFO("Could not create FTP server socket: 0x%08X\n", server_sockfd);
		server_start_status = server_sockfd;
		goto exit_thread;
	}

	/* Fill the server's address */
	serveraddr.sin_family = SCE_NET_AF_INET;
	serveraddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	serveraddr.sin_port = sceNetHtons(FTP_PORT);

	/* Bind the server's address to the socket */
	ret = sceNetBind(server_sockfd, (SceNetSockaddr *)&serveraddr, sizeof(serveraddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);
	if (ret < 0) {
		INFO("Could not bind FTP server socket: 0x%08X\n", ret);
		server_start_status = ret;
		close_server_socket();
		goto exit_thread;
	}

	/* Start listening */
	ret = sceNetListen(server_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);
	if (ret < 0) {
		INFO("Could not listen on FTP server socket: 0x%08X\n", ret);
		server_start_status = ret;
		close_server_socket();
		goto exit_thread;
	}
	server_start_status = 0;

	while (1) {
		/* Accept clients */
		SceNetSockaddrIn clientaddr;
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		DEBUG("Waiting for incoming connections...\n");

		client_sockfd = sceNetAccept(server_sockfd, (SceNetSockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {
			configure_socket_io_timeout(client_sockfd);
			DEBUG("New connection, client fd: 0x%08X\n", client_sockfd);

			/* Get the client's IP address */
			char remote_ip[16];
			sceNetInetNtop(SCE_NET_AF_INET,
				&clientaddr.sin_addr.s_addr,
				remote_ip,
				sizeof(remote_ip));

			unsigned int client_num = next_client_num++;
			INFO("Client %u connected, IP: %s port: %i\n",
				client_num, remote_ip, clientaddr.sin_port);

			/* Allocate state before creating or starting a thread. */
			/* taipool_calloc() recursively takes taipool's non-recursive
			 * semaphore through taipool_alloc(), deadlocking this accept thread.
			 * Keep allocation and initialization explicit. */
			ftpvita_client_info_t *client = malloc(sizeof(*client));
			if (client == NULL) {
				client_send_ctrl_msg((&(ftpvita_client_info_t){.ctrl_sockfd = client_sockfd}),
					"421 Server is out of memory." FTPVITA_EOL);
				sceNetSocketClose(client_sockfd);
				continue;
			}
			memset(client, 0, sizeof(*client));
			client->num = client_num;
			client->ctrl_sockfd = client_sockfd;
			client->data_sockfd = -1;
			client->pasv_sockfd = -1;
			client->data_con_type = FTP_DATA_CONNECTION_NONE;
			strcpy(client->cur_path, FTP_DEFAULT_PATH);
			memcpy(&client->addr, &clientaddr, sizeof(client->addr));

			/* Create a new thread for the client */
			char client_thread_name[64];
			snprintf(client_thread_name, sizeof(client_thread_name),
				"FTPVita_client_%u_thread", client_num);

			SceUID client_thid = sceKernelCreateThread(
				client_thread_name, client_thread,
				0x10000100, 0x10000, 0, 0, NULL);

			DEBUG("Client %u thread UID: 0x%08X\n", client_num, client_thid);
			if (client_thid < 0) {
				client_send_ctrl_msg(client, "421 Server cannot create a client thread." FTPVITA_EOL);
				close_socket(&client->ctrl_sockfd);
				free(client);
				continue;
			}
			client->thid = client_thid;

			/* Add the new client to the client list */
			if (!client_list_add(client)) {
				client_send_ctrl_msg(client, "421 Too many FTP clients." FTPVITA_EOL);
				sceKernelDeleteThread(client_thid);
				close_socket(&client->ctrl_sockfd);
				free(client);
				continue;
			}

			/* Start the client thread */
			ret = sceKernelStartThread(client_thid, sizeof(client), &client);
			if (ret < 0) {
				client_list_delete(client);
				sceKernelDeleteThread(client_thid);
				close_socket(&client->ctrl_sockfd);
				free(client);
			}
		} else {
			/* if sceNetAccept returns < 0, it means that the listening
			 * socket has been closed, this means that we want to
			 * finish the server thread */
			DEBUG("Server socket closed, 0x%08X\n", client_sockfd);
			break;
		}
	}

	exit_thread:
	if (server_start_status == SERVER_START_PENDING)
		server_start_status = -1;
	DEBUG("Server thread exiting!\n");

	sceKernelExitThread(0);
	return 0;
}

int ftpvita_init(char *vita_ip, unsigned short int *vita_port)
{
	int ret;
	int i;
	SceNetInitParam initparam;
	SceNetCtlInfo info;

	if (ftp_initialized) {
		return -1;
	}

	/* Init Net */
	ret = sceNetShowNetstat();
	if (ret == 0) {
		DEBUG("Net is already initialized.\n");
		net_init = -1;
	} else if (ret == SCE_NET_ERROR_ENOTINIT) {
		net_memory = malloc(NET_INIT_SIZE);
		if (net_memory == NULL) {
			ret = -1;
			goto error_netinit;
		}

		initparam.memory = net_memory;
		initparam.size = NET_INIT_SIZE;
		initparam.flags = 0;

		ret = net_init = sceNetInit(&initparam);
		DEBUG("sceNetInit(): 0x%08X\n", net_init);
		if (net_init < 0)
			goto error_netinit;
	} else {
		INFO("Net error: 0x%08X\n", net_init);
		goto error_netstat;
	}

	/* Init NetCtl */
	ret = netctl_init = sceNetCtlInit();
	DEBUG("sceNetCtlInit(): 0x%08X\n", netctl_init);
	if (netctl_init < 0 && netctl_init != NET_CTL_ERROR_NOT_TERMINATED)
		goto error_netctlinit;

	/* Get IP address */
	ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
	DEBUG("sceNetCtlInetGetInfo(): 0x%08X\n", ret);
	if (ret < 0)
		goto error_netctlgetinfo;

	/* Return data */
	strcpy(vita_ip, info.ip_address);
	*vita_port = FTP_PORT;

	/* Save the IP of PSVita to a global variable */
	sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &vita_addr);

	/* Create the client list mutex */
	client_list_mtx = sceKernelCreateMutex("FTPVita_client_list_mutex", 0, 0, NULL);
	DEBUG("Client list mutex UID: 0x%08X\n", client_list_mtx);
	if (client_list_mtx < 0) {
		ret = client_list_mtx;
		goto error_client_mutex;
	}

	/* Create server thread */
	server_thid = sceKernelCreateThread("FTPVita_server_thread",
		server_thread, 0x10000100, 0x10000, 0, 0, NULL);
	DEBUG("Server thread UID: 0x%08X\n", server_thid);
	if (server_thid < 0) {
		ret = server_thid;
		goto error_server_thread;
	}

	/* Init device list */
	for (i = 0; i < MAX_DEVICES; i++) {
		device_list[i].valid = 0;
	}

	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		custom_command_dispatchers[i].valid = 0;
	}
	client_list = NULL;
	number_clients = 0;
	next_client_num = 0;
	transfer_active = 0;
	reboot_quiesce_requested = 0;
	server_sockfd = -1;
	server_start_status = SERVER_START_PENDING;

	/* Start the server thread */
	ret = sceKernelStartThread(server_thid, 0, NULL);
	if (ret < 0)
		goto error_server_start;

	for (i = 0; i < SERVER_START_TIMEOUT_MS &&
		server_start_status == SERVER_START_PENDING; i++) {
		sceKernelDelayThread(1000);
	}
	if (server_start_status != 0) {
		ret = server_start_status == SERVER_START_PENDING ? -1 : server_start_status;
		close_server_socket();
		sceKernelWaitThreadEnd(server_thid, NULL, NULL);
		goto error_server_start;
	}

	ftp_initialized = 1;

	return 0;

error_server_start:
	sceKernelDeleteThread(server_thid);
	server_thid = -1;
error_server_thread:
	sceKernelDeleteMutex(client_list_mtx);
	client_list_mtx = -1;
error_client_mutex:
error_netctlgetinfo:
	if (netctl_init == 0) {
		sceNetCtlTerm();
		netctl_init = -1;
	}
error_netctlinit:
	if (net_init == 0) {
		sceNetTerm();
		net_init = -1;
	}
error_netinit:
	if (net_memory) {
		free(net_memory);
		net_memory = NULL;
	}
error_netstat:
	return ret;
}

void ftpvita_fini()
{
	if (ftp_initialized) {
		/* Prevent a second owner from entering teardown while descriptors and
		 * thread IDs are being invalidated. */
		ftp_initialized = 0;

		/* In order to "stop" the blocking sceNetAccept,
		 * we have to close the server socket; this way
		 * the accept call will return an error */
		close_server_socket();

		/* Wait until the server threads ends */
		if (server_thid >= 0) {
			sceKernelWaitThreadEnd(server_thid, NULL, NULL);
			sceKernelDeleteThread(server_thid);
			server_thid = -1;
		}

		/* To close the clients we have to do the same:
		 * we have to iterate over all the clients
		 * and shutdown their sockets */
		client_list_thread_end();

		/* Delete the client list mutex */
		if (client_list_mtx >= 0) {
			sceKernelDeleteMutex(client_list_mtx);
			client_list_mtx = -1;
		}

		client_list = NULL;
		number_clients = 0;
		transfer_active = 0;
		reboot_quiesce_requested = 0;

		if (netctl_init == 0)
			sceNetCtlTerm();
		if (net_init == 0)
			sceNetTerm();
		if (net_memory)
			free(net_memory);

		netctl_init = -1;
		net_init = -1;
		net_memory = NULL;
	}
}

int ftpvita_is_initialized()
{
	return ftp_initialized;
}

int ftpvita_get_active_client_count(void)
{
	return __sync_fetch_and_add(&number_clients, 0);
}

int ftpvita_has_active_transfer(void)
{
	return __sync_fetch_and_add(&transfer_active, 0) != 0;
}

int ftpvita_is_reboot_quiesced(void)
{
	return __sync_fetch_and_add(&reboot_quiesce_requested, 0) != 0;
}

int ftpvita_get_socket_timeout_failure_count(void)
{
	return __sync_fetch_and_add(&socket_timeout_config_failures, 0);
}

int ftpvita_abort_clients(void)
{
	ftpvita_client_info_t *it;
	int aborted = 0;
	const int abort_flags = SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION |
		SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION;

	if (client_list_mtx < 0)
		return 0;
	if (sceKernelLockMutex(client_list_mtx, 1, NULL) < 0)
		return -1;
	for (it = client_list; it; it = it->next) {
		if (it->ctrl_sockfd >= 0)
			sceNetSocketAbort(it->ctrl_sockfd, abort_flags);
		if (it->data_sockfd >= 0)
			sceNetSocketAbort(it->data_sockfd, abort_flags);
		if (it->pasv_sockfd >= 0)
			sceNetSocketAbort(it->pasv_sockfd, abort_flags);
		aborted++;
	}
	sceKernelUnlockMutex(client_list_mtx, 1);
	return aborted;
}

int ftpvita_prepare_for_reboot(int *active_clients, int *active_transfer)
{
	int clients = ftpvita_get_active_client_count();
	int transfer = ftpvita_has_active_transfer();

	if (!ftp_initialized || client_list_mtx < 0 ||
		!__sync_bool_compare_and_swap(&reboot_quiesce_requested, 0, 1)) {
		if (active_clients)
			*active_clients = clients;
		if (active_transfer)
			*active_transfer = transfer;
		return 0;
	}

	/* client_list_add() checks the guard while holding this same mutex. Once
	 * this lock is acquired, no pre-guard client can appear after the count. */
	if (sceKernelLockMutex(client_list_mtx, 1, NULL) < 0) {
		__sync_lock_release(&reboot_quiesce_requested);
		return 0;
	}
	clients = number_clients;
	transfer = ftpvita_has_active_transfer();
	if (active_clients)
		*active_clients = clients;
	if (active_transfer)
		*active_transfer = transfer;
	if (clients != 0 || transfer != 0)
		__sync_lock_release(&reboot_quiesce_requested);
	sceKernelUnlockMutex(client_list_mtx, 1);
	return clients == 0 && transfer == 0;
}

void ftpvita_cancel_reboot(void)
{
	__sync_lock_release(&reboot_quiesce_requested);
}

int ftpvita_add_device(const char *devname)
{
	int i;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (!device_list[i].valid) {
			strcpy(device_list[i].name, devname);
			device_list[i].valid = 1;
			return 1;
		}
	}
	return 0;
}

int ftpvita_del_device(const char *devname)
{
	int i;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (strcmp(devname, device_list[i].name) == 0) {
			device_list[i].valid = 0;
			return 1;
		}
	}
	return 0;
}

void ftpvita_set_info_log_cb(ftpvita_log_cb_t cb)
{
	info_log_cb = cb;
}

void ftpvita_set_debug_log_cb(ftpvita_log_cb_t cb)
{
	debug_log_cb = cb;
}

void ftpvita_set_file_buf_size(unsigned int size)
{
	file_buf_size = size;
}

int ftpvita_ext_add_custom_command(const char *cmd, cmd_dispatch_func func)
{
	int i;
	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (!custom_command_dispatchers[i].valid) {
			custom_command_dispatchers[i].cmd = cmd;
			custom_command_dispatchers[i].func = func;
			custom_command_dispatchers[i].valid = 1;
			return 1;
		}
	}
	return 0;
}

int ftpvita_ext_del_custom_command(const char *cmd)
{
	int i;
	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (strcmp(cmd, custom_command_dispatchers[i].cmd) == 0) {
			custom_command_dispatchers[i].valid = 0;
			return 1;
		}
	}
	return 0;
}

void ftpvita_ext_client_send_ctrl_msg(ftpvita_client_info_t *client, const char *msg)
{
	client_send_ctrl_msg(client, msg);
}

void ftpvita_ext_client_send_data_msg(ftpvita_client_info_t *client, const char *str)
{
	client_send_data_msg(client, str);
}
