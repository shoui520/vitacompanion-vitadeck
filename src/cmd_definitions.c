#include "cmd_definitions.h"
#include "package_installer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ftpvita.h>
// #include <ctype.h> <-- REMOVED to prevent linker errors

// PS Vita System Headers
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/rtc.h>
#include <psp2/kernel/threadmgr.h> 
#include <psp2/power.h>

#include <taihen.h>
#include <psp2/kernel/modulemgr.h>

#define COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))
#define SCE_APPMGR_APP_ID_ACTIVE (-4)
#define FOREGROUND_TITLE_ID_SIZE (SCE_APPMGR_MAX_APP_NAME_LENGTH + 1)
#define LAUNCH_CONFIRM_ATTEMPTS (80)
#define LAUNCH_CONFIRM_INTERVAL_US (100 * 1000)

/* This SceShell-facing export is present in SceAppMgr_stub but omitted from
 * VitaSDK's public appmgr.h.  -4 is Sony's ACTIVE app selector; PSVshell
 * uses the same AppMgr query chain when tracking the foreground process. */
extern SceInt32 sceAppMgrGetAppIdByAppId(SceInt32 app_id);

const cmd_definition cmd_definitions[] = {
    {.name = "version",    .description = "Show protocol and hardening version", .arg_count = 0, .executor = &cmd_version},
    {.name = "help",       .description = "Display this help screen",          .arg_count = 0, .executor = &cmd_help},
    {.name = "destroy",    .description = "Kill all running applications",     .arg_count = 0, .executor = &cmd_destroy},
    {.name = "foreground", .description = "Show the foreground title ID",      .arg_count = 0, .executor = &cmd_foreground},
    {.name = "install",    .description = "Install and promote a VPK",          .arg_count = 1, .executor = &cmd_install},
    {.name = "launch",     .description = "Launch an app by Title ID",         .arg_count = 1, .executor = &cmd_launch},
    {.name = "kill",       .description = "Kill an app by Title ID",           .arg_count = 1, .executor = &cmd_kill},
    {.name = "ftpstatus",  .description = "Show FTP clients and transfer state", .arg_count = 0, .executor = &cmd_ftpstatus},
    {.name = "ftpreset",   .description = "Abort stranded FTP clients",        .arg_count = 0, .executor = &cmd_ftpreset},
    {.name = "reboot",     .description = "Reboot only when FTP is quiescent",  .arg_count = 0, .executor = &cmd_reboot},
    {.name = "screen",     .description = "Turn the screen on or off",         .arg_count = 1, .executor = &cmd_screen},
    {.name = "battery",    .description = "Show battery percentage",           .arg_count = 0, .executor = &cmd_battery},
    {.name = "screenshot", .description = "Trigger a SceShell screenshot",     .arg_count = 0, .executor = &cmd_screenshot}
};

static void append_response(char *res_msg, size_t res_msg_size,
                            const char *format, ...)
{
  size_t used = 0;
  while (used < res_msg_size && res_msg[used] != '\0')
    used++;
  if (used >= res_msg_size)
    return;

  va_list args;
  va_start(args, format);
  vsnprintf(res_msg + used, res_msg_size - used, format, args);
  va_end(args);
}

const cmd_definition *cmd_get_definition(char *cmd_name) {
  for (unsigned int i = 0; i < COUNT_OF(cmd_definitions); i++) {
    if (!strcmp(cmd_name, cmd_definitions[i].name)) {
      return &(cmd_definitions[i]);
    }
  }
  return NULL;
}

void cmd_version(char **arg_list, size_t arg_count, char *res_msg,
                 size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  snprintf(res_msg, res_msg_size,
           "VitaCompanion-vitadeck protocol=4 hardening=9 "
           "ftp=LIST,REST_SAFE,BOUNDED_IO,TIMED_IO,SINGLE_FLIGHT,SAFE_REBOOT,RECOVERABLE_CLIENTS,RETR_EXTENT,RETR_ERRORS,STOR_SAFE,CMD_FRAMED "
           "app=FOREGROUND,CONFIRMED_LAUNCH,VPK_INSTALL\n");
}

void cmd_help(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  append_response(res_msg, res_msg_size, "Commands");

  for (size_t i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    append_response(res_msg, res_msg_size, "; %s - %s",
                    cmd_definitions[i].name, cmd_definitions[i].description);
  }
  append_response(res_msg, res_msg_size, "\n");
}

void cmd_kill(char **arg_list, size_t arg_count, char* res_msg, size_t res_msg_size) {
  (void)arg_count;
  if (sceAppMgrDestroyAppByName(arg_list[1]) < 0) {
    snprintf(res_msg, res_msg_size, "Error: cannot kill the app. Is the TITLEID correct?\n");
  } else {
    snprintf(res_msg, res_msg_size, "Killed.\n");
  }
}

void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  sceAppMgrDestroyOtherApp();
  snprintf(res_msg, res_msg_size, "Apps destroyed.\n");
}

static int get_foreground_title_id(char *title_id, size_t title_id_size)
{
  if (title_id == NULL || title_id_size < FOREGROUND_TITLE_ID_SIZE)
    return SCE_APPMGR_ERROR_INVALID;

  memset(title_id, 0, title_id_size);
  SceInt32 app_id = sceAppMgrGetAppIdByAppId(SCE_APPMGR_APP_ID_ACTIVE);
  if (app_id <= 0) {
    snprintf(title_id, title_id_size, "main");
    return 0;
  }

  SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(app_id);
  if (pid < 0)
    return pid;

  int result = sceAppMgrGetNameById(pid, title_id);
  title_id[title_id_size - 1] = '\0';
  return result;
}

static bool title_ids_equal(const char *left, const char *right)
{
  while (*left != '\0' && *right != '\0') {
    char left_char = *left++;
    char right_char = *right++;
    if (left_char >= 'a' && left_char <= 'z')
      left_char -= 'a' - 'A';
    if (right_char >= 'a' && right_char <= 'z')
      right_char -= 'a' - 'A';
    if (left_char != right_char)
      return false;
  }
  return *left == *right;
}

void cmd_foreground(char **arg_list, size_t arg_count, char *res_msg,
                    size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  char title_id[FOREGROUND_TITLE_ID_SIZE];
  int result = get_foreground_title_id(title_id, sizeof(title_id));
  if (result < 0) {
    snprintf(res_msg, res_msg_size,
             "Error: cannot read foreground title ID (0x%08X).\n", result);
    return;
  }
  snprintf(res_msg, res_msg_size, "Foreground: %s\n", title_id);
}

void cmd_install(char **arg_list, size_t arg_count, char *res_msg,
                 size_t res_msg_size) {
  (void)arg_count;
  char title_id[12] = {0};
  int result = install_vpk(arg_list[1], title_id, sizeof(title_id));
  if (result < 0) {
    snprintf(res_msg, res_msg_size,
             "Error: VPK installation failed (0x%08X).\n", result);
    return;
  }
  snprintf(res_msg, res_msg_size, "Installed: %s\n", title_id);
}

void cmd_launch(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
  (void)arg_count;
  char uri[32];
  int uri_length = snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", arg_list[1]);
  if (uri_length < 0 || (size_t)uri_length >= sizeof(uri)) {
    snprintf(res_msg, res_msg_size, "Error: TITLEID is too long.\n");
    return;
  }
  int result = sceAppMgrLaunchAppByUri(0x20000, uri);
  if (result < 0) {
    snprintf(res_msg, res_msg_size,
             "Error: launch request failed for %s (0x%08X).\n",
             arg_list[1], result);
    return;
  }

  char foreground[FOREGROUND_TITLE_ID_SIZE] = "unknown";
  for (int attempt = 0; attempt < LAUNCH_CONFIRM_ATTEMPTS; attempt++) {
    result = get_foreground_title_id(foreground, sizeof(foreground));
    if (result >= 0 && title_ids_equal(foreground, arg_list[1])) {
      snprintf(res_msg, res_msg_size, "Launched: %s\n", foreground);
      return;
    }
    sceKernelDelayThread(LAUNCH_CONFIRM_INTERVAL_US);
  }

  snprintf(res_msg, res_msg_size,
           "Error: launch of %s was not confirmed; foreground=%s.\n",
           arg_list[1], foreground);
}

void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  int clients = 0;
  int transfer = 0;
  if (!ftpvita_prepare_for_reboot(&clients, &transfer)) {
    snprintf(res_msg, res_msg_size,
             "Error: FTP busy; reboot refused (clients=%d transfer=%d).\n",
             clients, transfer);
    return;
  }
  snprintf(res_msg, res_msg_size, "Rebooting...\n");
  int result = scePowerRequestColdReset();
  if (result < 0) {
    ftpvita_cancel_reboot();
    snprintf(res_msg, res_msg_size,
             "Error: cold reset request failed (0x%08X).\n", result);
  }
}

void cmd_ftpstatus(char **arg_list, size_t arg_count, char *res_msg,
                   size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  snprintf(res_msg, res_msg_size,
           "FTP clients=%d transfer=%d reboot_guard=%d timeout_failures=%d\n",
           ftpvita_get_active_client_count(),
           ftpvita_has_active_transfer(),
           ftpvita_is_reboot_quiesced(),
           ftpvita_get_socket_timeout_failure_count());
}

void cmd_ftpreset(char **arg_list, size_t arg_count, char *res_msg,
                  size_t res_msg_size) {
  (void)arg_list;
  (void)arg_count;
  int clients = ftpvita_get_active_client_count();
  int transfer = ftpvita_has_active_transfer();
  int aborted = ftpvita_abort_clients();
  snprintf(res_msg, res_msg_size,
           "FTP reset requested clients=%d transfer=%d aborted=%d.\n",
           clients, transfer, aborted);
}

void cmd_screen(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
  (void)arg_count;
  char *state = arg_list[1];
  if (!strcmp(state, "on")) {
    scePowerRequestDisplayOn();
    snprintf(res_msg, res_msg_size, "Turning display on...\n");
  } else if (!strcmp(state, "off")) {
    scePowerRequestDisplayOff();
    snprintf(res_msg, res_msg_size, "Turning display off...\n");
  } else {
    snprintf(res_msg, res_msg_size, "Error: param should be 'on' or 'off'\n");
  }
}

void cmd_battery(char **arg_list, size_t arg_count, char *res_msg, size_t res_msg_size) {
    (void)arg_list;
    (void)arg_count;
    int percent = scePowerGetBatteryLifePercent();
    int charging = scePowerIsBatteryCharging();
    if (percent >= 0) {
        snprintf(res_msg, res_msg_size, "Battery: %d%% (%s)\n", percent,
                 charging ? "Charging" : "Not charging");
    } else {
        snprintf(res_msg, res_msg_size, "Error: Could not read battery percentage\n");
    }
}

/*
==============================================================
   Minimal SceShell screenshot ("shellShot") logic
==============================================================
*/

static int (*shellShot)(void) = NULL;
static int shellshot_initialized = 0;
static int shellshot_available   = 0;

static int module_get_offset(SceUID modid, int segidx, uint32_t offset, void *stub_out){
    int res = 0;
    SceKernelModuleInfo info;
    if (segidx > 3) return -1;
    if (stub_out == NULL) return -2;

    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    res = sceKernelGetModuleInfo(modid, &info);
    if (res < 0) return res;
    if (offset > info.segments[segidx].memsz) return -3;

    *(uint32_t *)stub_out = (uint32_t)(info.segments[segidx].vaddr + offset);
    return 0;
}

static void init_shellshot(void) {
    if (shellshot_initialized) return;
    shellshot_initialized = 1;
    shellshot_available   = 0;

    tai_module_info_t tai_info;
    sceClibMemset(&tai_info, 0, sizeof(tai_info));
    tai_info.size = sizeof(tai_module_info_t);

    int ret = taiGetModuleInfo("SceShell", &tai_info);
    if (ret < 0) return;

    int offset_shellshot = -1;

    switch (tai_info.module_nid) {
        case 0x0552F692: offset_shellshot = 0x14a928; break; 
        case 0x6CB01295: case 0xEAB89D5C: offset_shellshot = 0x142d5c; break; 
        case 0x5549BF1F: offset_shellshot = 0x14a980; break; 
        case 0x34B4D82E: case 0x12DAC0F3: case 0x0703C828: 
        case 0x2053B5A5: case 0xF476E785: case 0x939FFBE9: 
        case 0x734D476A: case 0xE6A02F2B: case 0x587F9CED: 
            offset_shellshot = 0x142db4; break; 
        default: return;
    }

    if (offset_shellshot < 0) return;

    if (module_get_offset(tai_info.modid, 0, offset_shellshot | 1, &shellShot) < 0) {
        shellShot = NULL;
        return;
    }

    if (shellShot != NULL) {
        shellshot_available = 1;
    }
}

void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg,
                    size_t res_msg_size) {
    (void)arg_list;
    (void)arg_count;

    init_shellshot();

    if (!shellshot_available || shellShot == NULL) {
        snprintf(res_msg, res_msg_size, "[ERROR] shellShot() not available.\n");
        return;
    }

    /* SceShell owns screenshot encoding and gallery placement.  The host
     * already snapshots and polls the gallery, so this system plugin must not
     * sleep, recursively walk the user's photos, or rename personal files. */
    int ret = shellShot();
    snprintf(res_msg, res_msg_size,
             ret >= 0 ? "[OK] Screenshot triggered (0x%08X).\n"
                      : "[ERROR] Screenshot trigger failed (0x%08X).\n",
             (unsigned int)ret);
}
