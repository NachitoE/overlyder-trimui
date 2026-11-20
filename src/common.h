#ifndef OVERLAY_COMMON_H
#define OVERLAY_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define APP_DIR         "/mnt/SDCARD/Apps/OverLyder"
#define MANIFEST_PATH   APP_DIR "/manifest.json"
#define CFG_PATH        "/mnt/SDCARD/System/etc/overlay.cfg"
#define DISABLE_FLAG    "/mnt/SDCARD/System/etc/overlay.disabled"
#define PID_FILE        "/tmp/overlayd.pid"
#define STOPPED_FILE    "/tmp/overlay_stopped.pids"
#define LOG_FILE        "/tmp/overlayd.log"
#define FONT_PATH       "/mnt/SDCARD/System/resources/DejaVuSans.ttf"
#define SYSTEMVAL_BIN   "/usr/trimui/bin/systemval"
#define SHMVAR_BIN      "/usr/trimui/bin/shmvar"
#define DISP_ATTR       "/sys/class/disp/disp/attr/"
#define LED_SYS         "/sys/class/led_anim/"

/* Runtime metadata (version + updater source) lives in manifest.json so it
 * can be changed without rebuilding the binaries. */
typedef struct {
    char version[32];
    char owner[64];
    char repo[64];
    char updater[128];
    char credits[128];
} app_manifest;

void manifest_load(app_manifest *m);

/* exit codes shared between overlay_ui and overlayd */
#define EXIT_CLOSE      0
#define EXIT_GAME       10
#define EXIT_REBOOT     11
#define EXIT_POWEROFF   12
#define EXIT_FAILED     90

/* key codes (linux/input-event-codes.h) */
#define K_A      305   /* BTN_EAST  */
#define K_B      304   /* BTN_SOUTH */
#define K_X      308
#define K_Y      307
#define K_L1     310
#define K_R1     311
#define K_SELECT 314
#define K_START  315
#define K_MODE   316   /* Home / Menu */

typedef struct {
    int hold_ms;      /* long press threshold in ms */
    int trigger[4];   /* key codes that must be held together */
    int trigger_count;
    int dim;          /* background dim 0..100 */
    int step;         /* default percent step */
    int confirm_exit; /* 1 = ask before exit/reboot/poweroff */
    int lang;         /* 0 = Spanish, 1 = English, 2 = Portuguese */
    uint32_t led_color;   /* 0xRRGGBB static LED color */
    int led_color_set;    /* 1 = a color was configured */
} overlay_cfg;

void cfg_defaults(overlay_cfg *c);
int  cfg_load(overlay_cfg *c);
void cfg_update_led_color(overlay_cfg *c, uint32_t rgb, int enabled);
uint32_t led_hue_rgb(int hue);
void led_color_apply(uint32_t rgb);

long now_ms(void);
void logm(const char *fmt, ...);

int read_int_file(const char *path, int def);
int write_int_file(const char *path, int v);
int write_str_file(const char *path, const char *s);

int run_cmd(const char *fmt, ...);
int read_cmd(char *out, size_t outsz, const char *fmt, ...);

int keycode_from_name(const char *name);

#endif
