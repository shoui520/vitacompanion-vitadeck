#include "cmd_definitions.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
// #include <ctype.h> <-- REMOVED to prevent linker errors

#include <vitasdk.h>

// PS Vita System Headers
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/rtc.h>
#include <psp2/kernel/threadmgr.h> 

#include <taihen.h>
#include <psp2/kernel/modulemgr.h>

#define COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))

// --- Forward Declarations ---
void cmd_help(char **arg_list, size_t arg_count, char *res_msg);
void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg);
void cmd_launch(char **arg_list, size_t arg_count, char *res_msg);
void cmd_kill(char **arg_list, size_t arg_count, char *res_msg);
void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg);
void cmd_screen(char **arg_list, size_t arg_count, char *res_msg);
void cmd_battery(char **arg_list, size_t arg_count, char *res_msg);
void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg);

const cmd_definition cmd_definitions[] = {
    {.name = "help",       .description = "Display this help screen",          .arg_count = 0, .executor = &cmd_help},
    {.name = "destroy",    .description = "Kill all running applications",     .arg_count = 0, .executor = &cmd_destroy},
    {.name = "launch",     .description = "Launch an app by Title ID",         .arg_count = 1, .executor = &cmd_launch},
    {.name = "kill",       .description = "Kill an app by Title ID",           .arg_count = 1, .executor = &cmd_kill},
    {.name = "reboot",     .description = "Reboot the console",                .arg_count = 0, .executor = &cmd_reboot},
    {.name = "screen",     .description = "Turn the screen on or off",         .arg_count = 1, .executor = &cmd_screen},
    {.name = "battery",    .description = "Show battery percentage",           .arg_count = 0, .executor = &cmd_battery},
    {.name = "screenshot", .description = "Take screenshot (Time-stamped)",    .arg_count = 0, .executor = &cmd_screenshot}
};

const cmd_definition *cmd_get_definition(char *cmd_name) {
  for (unsigned int i = 0; i < COUNT_OF(cmd_definitions); i++) {
    if (!strcmp(cmd_name, cmd_definitions[i].name)) {
      return &(cmd_definitions[i]);
    }
  }
  return NULL;
}

void cmd_help(char **arg_list, size_t arg_count, char *res_msg) {
  char buf[2000] = {0};
  int longest_cmd = 0;

  for (int i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    int cmd_length = strlen(cmd_definitions[i].name);
    if (cmd_length > longest_cmd) longest_cmd = cmd_length;
  }

  sprintf(buf, "%-*s\t\t%s\n", longest_cmd, "Command", "Description");
  strcpy(res_msg, buf);

  for (int i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    sprintf(buf, "%-*s\t\t%s\n", longest_cmd, cmd_definitions[i].name, cmd_definitions[i].description);
    strcat(res_msg, buf);
  }
}

void cmd_kill(char **arg_list, size_t arg_count, char* res_msg) {
  if (sceAppMgrDestroyAppByName(arg_list[1]) < 0) {
    strcpy(res_msg, "Error: cannot kill the app. Is the TITLEID correct?\n");
  } else {
    strcpy(res_msg, "Killed.\n");
  }
}

void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg) {
  sceAppMgrDestroyOtherApp();
  strcpy(res_msg, "Apps destroyed.\n");
}

void cmd_launch(char **arg_list, size_t arg_count, char *res_msg) {
  char uri[32];
  snprintf(uri, 32, "psgm:play?titleid=%s", arg_list[1]);
  if (sceAppMgrLaunchAppByUri(0x20000, uri) < 0) {
    strcpy(res_msg, "Error: cannot launch the app. Is the TITLEID correct?\n");
  } else {
    strcpy(res_msg, "Launched.\n");
  }
}

void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg) {
  scePowerRequestColdReset();
  strcpy(res_msg, "Rebooting...\n");
}

void cmd_screen(char **arg_list, size_t arg_count, char *res_msg) {
  char *state = arg_list[1];
  if (!strcmp(state, "on")) {
    scePowerRequestDisplayOn();
    strcpy(res_msg, "Turning display on...\n");
  } else if (!strcmp(state, "off")) {
    scePowerRequestDisplayOff();
    strcpy(res_msg, "Turning display off...\n");
  } else {
    strcpy(res_msg, "Error: param should be 'on' or 'off'\n");
  }
}

void cmd_battery(char **arg_list, size_t arg_count, char *res_msg) {
    int percent = scePowerGetBatteryLifePercent();
    int charging = scePowerIsBatteryCharging();
    if (percent >= 0) {
        sprintf(res_msg, "Battery: %d%% (%s)\n", percent, charging ? "Charging" : "Not charging");
    } else {
        strcpy(res_msg, "Error: Could not read battery percentage\n");
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

// --- Screenshot Management Logic ---

typedef struct {
    char latest_path[512];
    SceDateTime latest_time;
    int found;
} ScreenshotSearchCtx;

bool is_newer(SceDateTime *t1, SceDateTime *t2) {
    if (t1->year != t2->year) return t1->year > t2->year;
    if (t1->month != t2->month) return t1->month > t2->month;
    if (t1->day != t2->day) return t1->day > t2->day;
    if (t1->hour != t2->hour) return t1->hour > t2->hour;
    if (t1->minute != t2->minute) return t1->minute > t2->minute;
    return t1->second > t2->second;
}

// Helper: Custom lowercase to avoid ctype dependencies
static char custom_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

// Helper: Custom case-insensitive compare
int simple_strcasecmp(const char *s1, const char *s2) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    int result;
    if (p1 == p2) return 0;
    while ((result = custom_tolower(*p1) - custom_tolower(*p2++)) == 0)
        if (*p1++ == '\0') break;
    return result;
}

// Recursive scanner
void find_newest_recursive(const char *dir_path, ScreenshotSearchCtx *ctx) {
    SceUID dfd = sceIoDopen(dir_path);
    if (dfd < 0) return;

    SceIoDirent dirent;
    while (sceIoDread(dfd, &dirent) > 0) {
        if (strcmp(dirent.d_name, ".") == 0 || strcmp(dirent.d_name, "..") == 0) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dirent.d_name);

        if (SCE_S_ISDIR(dirent.d_stat.st_mode)) {
            find_newest_recursive(full_path, ctx);
        } else {
            int len = strlen(dirent.d_name);
            if (len > 4 && simple_strcasecmp(&dirent.d_name[len - 4], ".jpg") == 0) {
                
                // Exclude any files already in the root screenshot folder that match our pattern
                // to prevent recursively "finding" the one we just moved.
                if (!ctx->found || is_newer(&dirent.d_stat.st_mtime, &ctx->latest_time)) {
                    ctx->latest_time = dirent.d_stat.st_mtime;
                    strncpy(ctx->latest_path, full_path, sizeof(ctx->latest_path));
                    ctx->found = 1;
                }
            }
        }
    }
    sceIoDclose(dfd);
}


void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg) {
    (void)arg_list;
    (void)arg_count;

    init_shellshot();

    if (!shellshot_available || shellShot == NULL) {
        sprintf(res_msg, "[ERROR] shellShot() not available.\n");
        return;
    }

    // 1. Take the screenshot
    int ret = shellShot();
    
    // 2. WAIT for file I/O (3 seconds)
    sceKernelDelayThread(3 * 1000 * 1000); 

    // 3. Find the newest file
    ScreenshotSearchCtx ctx;
    ctx.found = 0;
    sceClibMemset(&ctx.latest_time, 0, sizeof(SceDateTime));
    
    find_newest_recursive("ux0:/picture/SCREENSHOT", &ctx);

    if (ctx.found) {
        char target_path[128];

        // 4. Generate Filename: YYYY-MM-DD-HHMMSS.jpg
        // --- ADDED SECONDS (%02d) TO PREVENT DUPLICATE FILE ERRORS ---
        snprintf(target_path, sizeof(target_path), 
            "ux0:/picture/SCREENSHOT/%04d-%02d-%02d-%02d%02d%02d.jpg",
            ctx.latest_time.year,
            ctx.latest_time.month,
            ctx.latest_time.day,
            ctx.latest_time.hour,
            ctx.latest_time.minute,
            ctx.latest_time.second
        );

        // 5. Move (Rename) the file to the root screenshot folder
        int move_ret = sceIoRename(ctx.latest_path, target_path);

        if (move_ret >= 0) {
            sprintf(res_msg, 
                "[OK] Saved: %s\n", target_path);
        } else {
            sprintf(res_msg, 
                "[WARN] Move failed (0x%X).\nSource: %s\nTarget: %s\n", 
                move_ret, ctx.latest_path, target_path);
        }
    } else {
        sprintf(res_msg, "[WARN] Shot triggered (0x%X), but file not found.\n", ret);
    }
}