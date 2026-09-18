#include "net.h"

#include "cmd.h"
#include "log.h"

#include <ftpvita.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>

#define NET_CTL_ERROR_NOT_TERMINATED 0x80412102

extern int run;

int all_is_up;
int net_connected;

SceUID net_thid = -1;
static int netctl_cb_id = -1;
static volatile int desired_connected;
static int netctl_initialized;

int net_start()
{
    int ret;
    if (net_thid >= 0)
        return 0;

    net_thid = sceKernelCreateThread("vitacompanion_net_thread", net_thread, 0x40, 0x10000, 0, 0, NULL);
    if (net_thid < 0)
        return net_thid;

    ret = sceKernelStartThread(net_thid, 0, NULL);
    if (ret < 0)
    {
        sceKernelDeleteThread(net_thid);
        net_thid = -1;
    }
    return ret;
}

void net_end()
{
    if (net_thid >= 0)
    {
        sceKernelWaitThreadEnd(net_thid, NULL, NULL);
        sceKernelDeleteThread(net_thid);
        net_thid = -1;
    }
}

static int start_services()
{
    char vita_ip[16];
    unsigned short int vita_port;

    LOG("do_net_connected\n");

#if ENABLE_LOGGING == 1
    ftpvita_set_info_log_cb(LOG);
    ftpvita_set_debug_log_cb(LOG);
#endif

    /* Eight FTP clients share the plugin's 1 MiB taiHEN pool. A 512 KiB
     * transfer buffer lets ordinary overlapping harness probes exhaust it. */
    ftpvita_set_file_buf_size(64 * 1024);

    if (ftpvita_init(vita_ip, &vita_port) < 0)
        return -1;

    ftpvita_add_device("ux0:");
    ftpvita_add_device("ur0:");
    ftpvita_add_device("uma0:");
    ftpvita_add_device("imc0:");
    ftpvita_add_device("xmc0:");
    ftpvita_add_device("grw0:");

    /* The command thread's loop predicate must be true before it starts. */
    net_connected = 1;
    if (cmd_start() < 0)
    {
        net_connected = 0;
        ftpvita_fini();
        return -1;
    }

    all_is_up = 1;
    return 0;
}

static void stop_services()
{
    net_connected = 0;
    if (!all_is_up)
        return;

    /* Stop accepting command clients before potentially blocking on FTP
     * teardown.  Exactly this network thread owns both transitions. */
    cmd_end();
    ftpvita_fini();
    all_is_up = 0;
}

static void netctl_cb(int event_type, void* arg)
{
    (void)arg;
    LOG("netctl cb: %d\n", event_type);

    // TODO sceNetCtlInetGetResult

    if (event_type == 1 || event_type == 2)
        desired_connected = 0;
    else if (event_type == 3)
        desired_connected = 1;
}

int net_thread(unsigned int args, void* argp)
{
    int ret;

    sceKernelDelayThread(3 * 1000 * 1000);

    ret = sceNetCtlInit();
    LOG("sceNetCtlInit: 0x%08X\n", ret);
    if (ret < 0 && ret != NET_CTL_ERROR_NOT_TERMINATED)
        return ret;
    netctl_initialized = (ret == 0);

    // If already connected to Wifi
    int state;
    ret = sceNetCtlInetGetState(&state);
    LOG("sceNetCtlInetGetState: ret=0x%08X state=%d\n", ret, state);
    desired_connected = (ret >= 0 && state == 3);

    ret = sceNetCtlInetRegisterCallback(netctl_cb, NULL, &netctl_cb_id);
    LOG("sceNetCtlInetRegisterCallback: 0x%08X\n", ret);
    if (ret < 0)
        goto exit_netctl;

    /* Close the state-query/registration race by sampling again after the
     * callback is live. A transition during registration is represented by
     * either this state or the queued callback. */
    ret = sceNetCtlInetGetState(&state);
    LOG("sceNetCtlInetGetState post-register: ret=0x%08X state=%d\n", ret, state);
    desired_connected = (ret >= 0 && state == 3);

    while (run)
    {
        sceNetCtlCheckCallback();
        if (desired_connected && !all_is_up)
            start_services();
        else if (!desired_connected && all_is_up)
            stop_services();
        sceKernelDelayThread(1000 * 1000);
    }

    stop_services();
    if (netctl_cb_id >= 0)
    {
        sceNetCtlInetUnregisterCallback(netctl_cb_id);
        netctl_cb_id = -1;
    }

exit_netctl:
    if (netctl_initialized)
    {
        sceNetCtlTerm();
        netctl_initialized = 0;
    }

    return ret < 0 ? ret : 0;
}
