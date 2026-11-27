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
#define TARGETS_FILE    "/tmp/overlay_targets.pids"
#define LOG_FILE        "/tmp/overlayd.log"
#define FONT_PATH       "/mnt/SDCARD/System/resources/DejaVuSans.ttf"
#define SYSTEMVAL_BIN   "/usr/trimui/bin/systemval"
#define SHMVAR_BIN      "/usr/trimui/bin/shmvar"
#define DISP_ATTR       "/sys/class/disp/disp/attr/"
#define LED_SYS         "/sys/class/led_anim/"
#define LAYER_FLAG      "/tmp/overlay_layer_active"
#define HUD_LAYER_FLAG  "/tmp/overlyder_hud_layer"
#define HUD_SYNC_FILE   "/tmp/overlyder_hud_sync"
#define OVERLAY_OPEN_FLAG "/tmp/overlyder_overlay_open"
#define FPS_LIMIT_FILE  "/tmp/overlyder_fps_limit"
#define PACE_LIB_REL    "/lib/liboverlyder_pace.so"
#define PRELOAD_PATH    "/etc/ld.so.preload"
#define HUD_PID_FILE    "/tmp/overlyder_hud.pid"
#define HUD_STATUS_FILE "/tmp/overlyder_hud.status"

/* Persistent read-only performance HUD. The field mask is a bit set so each
 * row can be toggled independently in the settings submenu. */
#define HUD_F_FPS       (1u << 0)
#define HUD_F_FRAME     (1u << 1)
#define HUD_F_CPU       (1u << 2)
#define HUD_F_GPU       (1u << 3)
#define HUD_F_TEMP      (1u << 4)
#define HUD_F_CPUFREQ   (1u << 5)
#define HUD_F_GPUFREQ   (1u << 6)
#define HUD_F_BATT      (1u << 7)
#define HUD_F_COUNT     8
#define HUD_FIELDS_ALL  ((1u << HUD_F_COUNT) - 1u)
#define HUD_STYLE_SIMPLE  0  /* one line of text, no box */
#define HUD_STYLE_WINDOW  1  /* rounded translucent box */

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
/* L2/R2 (LT/RT) are analog (EV_ABS ABS_Z / ABS_RZ, 0..255). Synthetic key
 * codes let them share the trigger/mapping machinery; the daemon and UI map
 * them from EV_ABS values >= 128. */
#define K_L2     1001
#define K_R2     1002
#define K_LT     K_L2
#define K_RT     K_R2

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
    int upd_reinstall;    /* 1 = allow reinstalling the installed version */
    int no_sigstop;       /* 1 = keep the game running, use a disp layer */
    int keyboard_mode;    /* 1 = full keyboard with input box, 2 = compact */
    int kbd_trigger[4];   /* key codes that open the keyboard directly */
    int kbd_trigger_count;/* 0 = shortcut disabled */
    int fps_hud;          /* 1 = persistent read-only FPS HUD */
    int fps_hud_pos;      /* 0=top-left 1=top-right 2=bottom-left 3=bottom-right */
    int fps_hud_style;    /* HUD_STYLE_SIMPLE / HUD_STYLE_WINDOW */
    unsigned fps_hud_fields; /* bit mask of HUD_F_* rows */
    int fps_limit;        /* 0 = no cap, else 5..60 in steps of 5 */
    int fps_limit_enabled;/* experimental feature flag from the config app */
} overlay_cfg;

void cfg_defaults(overlay_cfg *c);
int  cfg_load(overlay_cfg *c);
void cfg_update_led_color(overlay_cfg *c, uint32_t rgb, int enabled);
void cfg_update_reinstall(overlay_cfg *c, int enabled);
uint32_t led_hue_rgb(int hue);
void led_color_apply(uint32_t rgb);

/* Optional virtual keyboard (in-process, no external binary). */
int text_inject(const char *text, int gamepad_fd);

long now_ms(void);
void logm(const char *fmt, ...);

int read_int_file(const char *path, int def);
int write_int_file(const char *path, int v);
int write_str_file(const char *path, const char *s);

int run_cmd(const char *fmt, ...);
int read_cmd(char *out, size_t outsz, const char *fmt, ...);

/* Flat JSON string-object loader (used by the language files). */
int json_load_strings(const char *path, const char *const *keys,
                      char (*out)[256], int n);

int keycode_from_name(const char *name);

#endif
