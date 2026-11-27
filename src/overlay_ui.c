#include "common.h"
#include "overlay_layer.h"
#include "textinject.h"

#include <math.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <linux/fb.h>
#include <linux/uinput.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <dirent.h>
#include <ctype.h>
#include <stdint.h>

#define SCR_W 1280
#define SCR_H 720
#define CANVAS_PX (SCR_W * SCR_H)

/* ------------------------------------------------------------------ */
/* items                                                              */
/* ------------------------------------------------------------------ */

enum { IT_SLIDER = 0, IT_ACTION = 1, IT_TOGGLE = 2 };
enum {
    ACT_NONE = 0, ACT_CLOSE, ACT_EXIT_GAME, ACT_REBOOT, ACT_POWEROFF,
    ACT_KEYBOARD, ACT_FPSHUD
};
enum {
    I_VOL = 0, I_BRI, I_CON, I_EXP, I_SAT, I_TMP,
    I_LEDBRI, I_LEDCOLOR, I_MUTE, I_KEYBOARD, I_FPSHUD,
    I_PLAY, I_EXIT, I_REBOOT, I_POWER
};

/* Glyphs from the bundled Material Icons pack (Apache-2.0), by code point:
 * volume_up, brightness_7, contrast, exposure, palette, device_thermostat,
 * play_arrow, logout, restart_alt, power_settings_new. */
#define GL_VOL     0xE050u
#define GL_BRI     0xE3ACu
#define GL_CON     0xEB37u
#define GL_EXP     0xE3CAu
#define GL_SAT     0xE40Au
#define GL_TMP     0xE1FFu
#define GL_PLAY    0xE037u
#define GL_EXIT    0xE9BAu
#define GL_REBOOT  0xF053u
#define GL_POWER   0xE8ACu
#define GL_LED     0xEA24u  /* emoji_objects (lightbulb) */
#define GL_COLOR   0xE0C3u  /* invert_colors */
#define GL_MUTE    0xE04Fu  /* volume_off */
#define GL_KEYBOARD 0xE5DAu /* keyboard */
#define GL_FPS      0xE24Bu /* show_chart */
#define GL_BATTERY 0xE1A4u  /* battery_full */
#define GL_WIFI    0xE63Eu  /* wifi (Material Icons v1.017 codepoint) */
#define GL_BT      0xE1A7u  /* bluetooth */

typedef struct {
    const char *label;
    int kind;
    int action;
    int step;
    int value; /* 0..100 */
    uint32_t glyph;
} item_t;

static item_t items[] = {
    { "Volumen",     IT_SLIDER, ACT_NONE,      5, 50, GL_VOL },
    { "Brillo",      IT_SLIDER, ACT_NONE,     10, 80, GL_BRI },
    { "Contraste",   IT_SLIDER, ACT_NONE,      5, 60, GL_CON },
    { "Exposición",  IT_SLIDER, ACT_NONE,      5, 60, GL_EXP },
    { "Saturación",  IT_SLIDER, ACT_NONE,      5, 80, GL_SAT },
    { "Temperatura", IT_SLIDER, ACT_NONE,      5, 50, GL_TMP },
    { "LED brillo",  IT_SLIDER, ACT_NONE,     10, 40, GL_LED },
    { "LED color",   IT_SLIDER, ACT_NONE,      5,  0, GL_COLOR },
    { "Silenciar",   IT_TOGGLE, ACT_NONE,      0,  0, GL_MUTE },
    { "Teclado",     IT_ACTION, ACT_KEYBOARD,  0,  0, GL_KEYBOARD },
    { "Contador FPS", IT_ACTION, ACT_FPSHUD,   0,  0, GL_FPS },
    { "Continuar",   IT_ACTION, ACT_CLOSE,     0,  0, GL_PLAY },
    { "Salir juego", IT_ACTION, ACT_EXIT_GAME, 0,  0, GL_EXIT },
    { "Reiniciar",   IT_ACTION, ACT_REBOOT,    0,  0, GL_REBOOT },
    { "Apagar",      IT_ACTION, ACT_POWEROFF,  0,  0, GL_POWER },
};

#define NITEMS ((int)(sizeof items / sizeof items[0]))

/* The two circular wireless toggles form one extra row below the action list.
 * Up/Down treats them as a single stop (so it keeps cycling the menu), while
 * Left/Right picks WiFi or Bluetooth inside the row. */
#define SEL_RADIO      NITEMS
#define NSEL           (NITEMS + 1)
#define SEL_IS_RADIO(s) ((s) == SEL_RADIO)

/* ------------------------------------------------------------------ */
/* globals                                                            */
/* ------------------------------------------------------------------ */

static TTF_Font *g_f_big, *g_f_mid, *g_f_small, *g_f_icon;

#define ANIM_MS 320

static int g_sel = 0;
static int g_confirm = ACT_NONE;
static int g_quit = 0;
static int g_exit_code = EXIT_CLOSE;
static int g_has_game = 0;
static int g_dirty = 1;
static int g_dim = 55;
static int g_debug = 0;
static int g_busy = 0;
static int g_keep_overlay = 0;
static int g_led_color_changed = 0;
static int g_mode_cfg = 0;
static int g_closing = 0;
static long g_anim_start = 0;
static long g_close_at = 0;
static long g_open_at = 0;
static int g_trigger_held = 0; /* keys held when the overlay opened */
static long g_trigger_released_at = 0;
static int g_saved_vol = 50; /* volume restored when unmuting */
static int g_ax = 0, g_ay = 0;         /* analog stick axes */
static int g_stick_max = 0;            /* axis range from the driver */
static int g_stick_dir = 0;            /* last applied stick direction */

/* Virtual keyboard state. The overlay panel stays behind it but is not drawn
 * while g_kb_open, so the keyboard behaves like a separate full-screen mode. */
static int g_kb_open = 0;
static int g_kb_only = 0;   /* opened via dedicated shortcut, no panel behind */
static long g_kb_open_at = 0;
static long g_kb_closed_at = 0;
static char g_kb_text[128];
static int g_kb_shift = 0;
static int g_kb_mode = 2;  /* 1 = full with input box, 2 = compact */

/* Compact keyboard position/size. Movable with the right stick. */
#define KB2_W        720
#define KB2_H        248
#define KB2_ROW_TOP  26
#define KB2_ROW_STEP 40
#define KB2_KEY_H    36
#define KB2_KEY_W    64
#define KB2_CTRL_W   100
#define KB2_GAP      5
static int g_kb_x = (SCR_W - KB2_W) / 2;
static int g_kb_y = SCR_H - KB2_H - 40;
static int g_rx = 0, g_ry = 0, g_rstick_max = 0;
static int g_kb_symbols = 0;
static int g_kb_row = 0, g_kb_col = 0;
static int g_kbd_fd = -1;               /* uinput virtual keyboard */
static int g_kbd_inherited = 0;        /* owned by overlayd, do not close */
static int g_gamepad_fd = -1;           /* the device the game actually reads */
static int g_text_to_gamepad = 0;       /* OVERLAY_TEXT_TO_GAMEPAD=1 */

/* ------------------------------------------------------------------ */
/* configuration screen (CrossMix app)                                */
/* ------------------------------------------------------------------ */

enum {
    CFG_ESTADO = 0, CFG_GATILLO,
    CFG_K1, CFG_K2, CFG_K3, CFG_K4,
    CFG_UMBRAL, CFG_CONFIRMAR, CFG_IDIOMA, CFG_SIGSTOP, CFG_KBMODE,
    CFG_KBHOTKEY, CFG_LEARN, CFG_RESET, CFG_ABOUT, CFG_UPDATE, CFG_LISTO, NCFG
};

/* FPS counter submenu, opened from the in-game overlay (not the app).
 * Read-only telemetry configuration: no clocks or game tweaks live here. */
enum {
    HUD_R_ENABLED = 0, HUD_R_POS, HUD_R_STYLE, HUD_R_FIELDS,
    HUD_R_BACK = HUD_R_FIELDS + HUD_F_COUNT
};
#define NHUDROWS (HUD_R_BACK + 1)

#define PRESET_CUSTOM 6

static const struct {
    const char *name;
    int codes[4];
    int count;
} g_presets[] = {
    { "Mantener Home",  { K_MODE, 0, 0, 0 }, 1 },
    { "Home + Select",  { K_MODE, K_SELECT, 0, 0 }, 2 },
    { "Home + Start",   { K_MODE, K_START, 0, 0 }, 2 },
    { "L1 + R1",        { K_L1, K_R1, 0, 0 }, 2 },
    { "Mantener Select",{ K_SELECT, 0, 0, 0 }, 1 },
    { "Mantener Start", { K_START, 0, 0, 0 }, 1 },
    { "Personalizado",  { 0, 0, 0, 0 }, 0 },
};
#define NPRESETS ((int)(sizeof g_presets / sizeof g_presets[0]))

/* Keys that can be part of a custom trigger. Index 0 means "unused". */
static const struct { const char *name; int code; } g_keys[] = {
    { "Ninguna", 0 },
    { "Home / Menu", K_MODE },
    { "Select",  K_SELECT },
    { "Start",   K_START },
    { "A",       K_A },
    { "B",       K_B },
    { "X",       K_X },
    { "Y",       K_Y },
    { "L1",      K_L1 },
    { "R1",      K_R1 },
    { "L2 / LT", K_L2 },
    { "R2 / RT", K_R2 },
};
#define NKEYS ((int)(sizeof g_keys / sizeof g_keys[0]))

static const int g_holds[] = { 400, 700, 1000, 1500 };
#define NHOLDS ((int)(sizeof g_holds / sizeof g_holds[0]))

static int g_cfg_sel = 0;
static int g_cfg_enabled = 1;
static int g_cfg_preset = 0;
static int g_cfg_hold = 1;
static int g_cfg_confirm = 1;
static int g_cfg_sigstop = 1;  /* 1 = hardware layer, 0 = SIGSTOP */
static int g_cfg_kb_mode = 2;  /* 1 = full, 2 = compact */
static int g_kb_custom[4];
static int g_kb_custom_count = 0;
static int g_kb_learning = 0;
static unsigned int g_kb_learn_mask = 0;
static long g_kb_learn_at = 0;
static int g_custom[4] = { 1, 0, 0, 0 }; /* indices into g_keys */
static int g_learning = 0;
static int g_learn_mask = 0;
static long g_learn_at = 0;
static int g_about = 0;

static overlay_cfg g_cfg;
static app_manifest g_manifest;

/* Self-updater state (CrossMix config app only). */
enum {
    UPD_NONE = 0, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE, UPD_CONFIRM,
    UPD_INSTALLING, UPD_INSTALLED, UPD_ERROR, UPD_OPTIONS
};
static int g_upd_state = UPD_NONE;
static int g_upd_prev_state = UPD_UPTODATE; /* state behind the options submenu */
static int g_upd_same = 0;                  /* remote tag == installed version */
static int g_upd_opt_sel = 0;               /* 0 = reinstall switch, 1 = back */
static char g_upd_latest[32] = "";

static int g_hud_sub = 0;
static int g_hud_sel = HUD_R_ENABLED;

static void update_compose(void);
static void upd_check(void);
static void upd_install(void);

#define COL_TEXT_R   240
#define COL_TEXT_G   242
#define COL_TEXT_B   248
#define COL_DIM_R    150
#define COL_DIM_G    152
#define COL_DIM_B    162
#define COL_ACC_R    230
#define COL_ACC_G    30
#define COL_ACC_B    50
#define COL_ICON_R   226
#define COL_ICON_G   229
#define COL_ICON_B   238

/* ------------------------------------------------------------------ */
/* translations (selector lives in the CrossMix config app)           */
/* ------------------------------------------------------------------ */

enum { LANG_ES = 0, LANG_EN, LANG_PT, NLANG };
static int g_lang = LANG_ES;

/* Language keys and the S_* enum are generated from app/lang/keys.json.
 * See scripts/gen_lang.py and docs/LANGUAGES.md. */
#include "lang_keys.h"
static char g_tr_values[S_COUNT][256];

#define TR(id) (g_tr_values[(id)][0] ? g_tr_values[(id)] : g_tr_keys[(id)])
#define LANG_NAME(l) (l == LANG_EN ? "English" : (l == LANG_PT ? "Português" : "Español"))

static const char *const g_lang_codes[NLANG] = { "es", "en", "pt" };

/* Load app/lang/<code>.json. Values missing from the file fall back to the
 * key name so the UI never renders blank labels. OVERLAY_LANG_DIR can point
 * to an alternative folder (useful for testing). */
static void lang_load(int lang)
{
    int idx = lang;
    if (idx < 0 || idx >= NLANG)
        idx = 0;
    const char *code = g_lang_codes[idx];
    char path[512] = "";
    const char *dir = getenv("OVERLAY_LANG_DIR");
    if (dir && *dir) {
        snprintf(path, sizeof path, "%s/%s.json", dir, code);
    } else {
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                snprintf(path, sizeof path, "%s/../lang/%s.json", exe, code);
            }
        }
        if (!path[0] || access(path, R_OK) != 0)
            snprintf(path, sizeof path, "%s/lang/%s.json", APP_DIR, code);
    }

    memset(g_tr_values, 0, sizeof g_tr_values);
    int loaded = json_load_strings(path, g_tr_keys, g_tr_values, S_COUNT);
    if (loaded < 0) {
        logm("language file %s unavailable", path);
        loaded = 0;
    }
    for (int i = 0; i < S_COUNT; i++)
        if (!g_tr_values[i][0])
            snprintf(g_tr_values[i], sizeof g_tr_values[i], "%s", g_tr_keys[i]);
    logm("language %s: %d/%d strings from %s", code, loaded, S_COUNT, path);
}

static void items_set_language(void)
{
    static const int ids[NITEMS] = {
        S_VOL, S_BRI, S_CON, S_EXP, S_SAT, S_TMP,
        S_LED_BRI, S_LED_COLOR, S_MUTE, S_KEYBOARD, S_CFG_FPSHUD,
        S_PLAY, S_EXIT, S_REBOOT, S_POWER
    };
    for (int i = 0; i < NITEMS; i++)
        items[i].label = TR(ids[i]);
}

static const char *preset_name(int p)
{
    static const int ids[NPRESETS] = {
        S_PRESET_HOLD_HOME, S_PRESET_HOME_SELECT, S_PRESET_HOME_START,
        S_PRESET_L1R1, S_PRESET_HOLD_SELECT, S_PRESET_HOLD_START,
        S_PRESET_CUSTOM
    };
    if (p < 0 || p >= NPRESETS)
        return "";
    return TR(ids[p]);
}

static const char *key_name(int i)
{
    if (i == 0)
        return TR(S_NONE);
    if (i < 0 || i >= NKEYS)
        return "";
    return g_keys[i].name;
}

/* ------------------------------------------------------------------ */
/* framebuffer                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_cv[CANVAS_PX];      /* our canvas          */
static uint32_t g_bg[CANVAS_PX];      /* captured background */
static uint32_t g_bg_dim[CANVAS_PX];  /* pre-dimmed background */
static int g_bg_ok = 0;

/* Optional hardware-layer backend: the game keeps running and we composite
 * our canvas on a separate display layer instead of fighting for /dev/fb0. */
static overlay_layer g_layer;
static int g_use_layer = 0;

static int g_fb_fd = -1;
static unsigned char *g_fb_mem = NULL;
static size_t g_fb_len = 0;
static struct fb_var_screeninfo g_vi;
static struct fb_fix_screeninfo g_fi;

int overlay_fb_open(void)
{
    g_fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0)
        return -1;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vi) || ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_fi))
        return -1;
    g_fb_len = (size_t)g_fi.line_length * g_vi.yres_virtual;
    g_fb_mem = mmap(NULL, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        g_fb_mem = NULL;
        return -1;
    }
    return (g_vi.bits_per_pixel == 32) ? 0 : -1;
}

/* copy the page currently shown by the display controller */
static void fb_capture(uint32_t *dst)
{
    if (!g_fb_mem)
        return;
    unsigned char *src = g_fb_mem + (size_t)g_vi.yoffset * g_fi.line_length;
    for (int y = 0; y < SCR_H; y++) {
        const uint32_t *line = (const uint32_t *)(src + (size_t)y * g_fi.line_length);
        for (int x = 0; x < SCR_W; x++)
            dst[y * SCR_W + x] = line[x] & 0xFFFFFF;
    }
    g_bg_ok = 1;
}

/* Done once at startup: the dimmed background never changes while the UI is
 * up, so copying it per frame is much cheaper than re-dimming 921600 pixels. */
static void prepare_background(void)
{
    int m = 100 - g_dim;
    for (int i = 0; i < CANVAS_PX; i++) {
        uint32_t p = g_bg_ok ? g_bg[i] : 0x101016;
        uint32_t r = ((p >> 16) & 0xFF) * m / 100;
        uint32_t g = ((p >> 8) & 0xFF) * m / 100;
        uint32_t b = (p & 0xFF) * m / 100;
        g_bg_dim[i] = (r << 16) | (g << 8) | b;
    }
}

/* write our canvas to page 0 and make the controller show page 0 */
static void fb_flip(void)
{
    if (!g_fb_mem)
        return;
    for (int y = 0; y < SCR_H; y++)
        memcpy(g_fb_mem + (size_t)y * g_fi.line_length, &g_cv[y * SCR_W], (size_t)SCR_W * 4);

    struct fb_var_screeninfo v = g_vi;
    v.yoffset = 0;
    v.xoffset = 0;
    v.activate = FB_ACTIVATE_NOW;
    if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &v) < 0)
        ioctl(g_fb_fd, FBIOPUT_VSCREENINFO, &v);
}

/* Write the canvas into our hardware-layer page. The display engine expects
 * the UI layer geometry (720x1280 portrait, rotated to the panel), so rotate
 * our 1280x720 canvas into that buffer. */
static void layer_flip(void)
{
    uint32_t *dst = g_layer.pixels;
    if (!dst)
        return;
    if (g_layer.width == SCR_W && g_layer.height == SCR_H) {
        memcpy(dst, g_cv, sizeof g_cv);
        return;
    }
    if (g_layer.width != SCR_H || g_layer.height != SCR_W) {
        logm("unexpected layer geometry %dx%d", g_layer.width, g_layer.height);
        return;
    }
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int dx = y;
            int dy = SCR_W - 1 - x;
            dst[(size_t)dy * (size_t)g_layer.width + (size_t)dx] =
                g_cv[(size_t)y * SCR_W + (size_t)x];
        }
    }
}

static void display_flip(void)
{
    if (g_use_layer)
        layer_flip();
    else
        fb_flip();
}

static void fb_restore(void)
{
    if (!g_fb_mem || !g_bg_ok)
        return;
    for (int y = 0; y < SCR_H; y++)
        memcpy(g_fb_mem + (size_t)y * g_fi.line_length, &g_bg[y * SCR_W], (size_t)SCR_W * 4);
    struct fb_var_screeninfo v = g_vi;
    v.yoffset = 0;
    v.xoffset = 0;
    v.activate = FB_ACTIVATE_NOW;
    if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &v) < 0)
        ioctl(g_fb_fd, FBIOPUT_VSCREENINFO, &v);
}

/* ------------------------------------------------------------------ */
/* canvas helpers                                                     */
/* ------------------------------------------------------------------ */

static inline void px_blend(int x, int y, uint32_t rgb, int a)
{
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H || a <= 0)
        return;
    uint32_t *d = &g_cv[y * SCR_W + x];
    if (g_use_layer) {
        /* The layer is alpha-blended by the display hardware over the live
         * game image, so every pixel needs a real alpha channel. */
        if (a >= 255) {
            *d = 0xFF000000u | (rgb & 0xFFFFFFu);
            return;
        }
        uint32_t da = (*d >> 24) & 0xFF;
        uint32_t oa = (uint32_t)a > da ? (uint32_t)a : da;
        uint32_t dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
        uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
        dr = (sr * a + dr * (255 - a)) / 255;
        dg = (sg * a + dg * (255 - a)) / 255;
        db = (sb * a + db * (255 - a)) / 255;
        *d = (oa << 24) | (dr << 16) | (dg << 8) | db;
        return;
    }
    if (a >= 255) {
        *d = rgb;
        return;
    }
    uint32_t dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
    uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    dr = (sr * a + dr * (255 - a)) / 255;
    dg = (sg * a + dg * (255 - a)) / 255;
    db = (sb * a + db * (255 - a)) / 255;
    *d = (dr << 16) | (dg << 8) | db;
}

static void fill_rect_a(int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCR_W ? SCR_W : x + w;
    int y1 = y + h > SCR_H ? SCR_H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            px_blend(xx, yy, rgb, alpha);
}

static void fill_circle_a(int cx, int cy, int r, uint32_t rgb, int alpha)
{
    if (r <= 0)
        return;
    for (int dy = -r; dy <= r; dy++) {
        int w = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
        for (int dx = -w; dx <= w; dx++)
            px_blend(cx + dx, cy + dy, rgb, alpha);
    }
}

static int imin(int a, int b) { return a < b ? a : b; }

static void fill_round_a(int x, int y, int w, int h, int radius, uint32_t rgb, int alpha)
{
    radius = imin(radius, imin(w, h) / 2);
    if (radius <= 0) {
        fill_rect_a(x, y, w, h, rgb, alpha);
        return;
    }
    fill_rect_a(x + radius, y, w - 2 * radius, h, rgb, alpha);
    fill_rect_a(x, y + radius, radius, h - 2 * radius, rgb, alpha);
    fill_rect_a(x + w - radius, y + radius, radius, h - 2 * radius, rgb, alpha);
    fill_circle_a(x + radius, y + radius, radius, rgb, alpha);
    fill_circle_a(x + w - radius - 1, y + radius, radius, rgb, alpha);
    fill_circle_a(x + radius, y + h - radius - 1, radius, rgb, alpha);
    fill_circle_a(x + w - radius - 1, y + h - radius - 1, radius, rgb, alpha);
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

/* Reuse rendered surfaces: the animation redraws the same strings ~30x, and
 * TTF_RenderUTF8_Blended per frame was the bottleneck. */
#define NCACHE 96
typedef struct {
    SDL_Surface *s;
    TTF_Font *f;
    uint32_t rgb;
    char key[64];
} cache_entry;
static cache_entry g_cache[NCACHE];
static int g_cache_next = 0;

static SDL_Surface *cached_surface(TTF_Font *f, const char *key, const char *text, uint32_t rgb)
{
    for (int i = 0; i < NCACHE; i++) {
        cache_entry *e = &g_cache[i];
        if (e->s && e->f == f && e->rgb == rgb && !strcmp(e->key, key))
            return e->s;
    }
    SDL_Color c = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(f, text, c);
    if (!s)
        return NULL;
    int slot = g_cache_next++ % NCACHE;
    if (g_cache[slot].s)
        SDL_FreeSurface(g_cache[slot].s);
    g_cache[slot].s = s;
    g_cache[slot].f = f;
    g_cache[slot].rgb = rgb;
    snprintf(g_cache[slot].key, sizeof g_cache[slot].key, "%s", key);
    return s;
}

static void draw_text(const char *txt, int x, int y, TTF_Font *f, uint32_t rgb, int align)
{
    if (!f || !txt || !*txt)
        return;
    SDL_Surface *s = cached_surface(f, txt, txt, rgb);
    if (!s)
        return;
    int dx = x;
    if (align == 1)
        dx = x - s->w / 2;
    else if (align == 2)
        dx = x - s->w;

    SDL_LockSurface(s);
    for (int yy = 0; yy < s->h; yy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)yy * s->pitch);
        for (int xx = 0; xx < s->w; xx++) {
            uint32_t p = row[xx];
            int a = (p >> 24) & 0xFF;
            if (a)
                px_blend(dx + xx, y + yy, p & 0xFFFFFF, a);
        }
    }
    SDL_UnlockSurface(s);
}

/* ------------------------------------------------------------------ */
/* icons                                                              */
/* ------------------------------------------------------------------ */

static void cp_to_utf8(uint32_t cp, char out[5])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
    }
}

/* Icons come from the bundled Material Icons font, not hand-drawn shapes. */
static void draw_icon(uint32_t cp, int cx, int cy, uint32_t rgb, int alpha)
{
    if (!g_f_icon)
        return;
    char glyph[8], key[9];
    cp_to_utf8(cp, glyph);
    key[0] = '\x01'; /* cache key only, never rendered */
    strcpy(key + 1, glyph);
    SDL_Surface *s = cached_surface(g_f_icon, key, glyph, rgb);
    if (!s)
        return;
    int dx = cx - s->w / 2, dy = cy - s->h / 2;
    SDL_LockSurface(s);
    for (int yy = 0; yy < s->h; yy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)yy * s->pitch);
        for (int xx = 0; xx < s->w; xx++) {
            int a = (row[xx] >> 24) & 0xFF;
            if (a)
                px_blend(dx + xx, dy + yy, row[xx] & 0xFFFFFF, a * alpha / 255);
        }
    }
    SDL_UnlockSurface(s);
}

/* ------------------------------------------------------------------ */
/* backends: brightness / volume / image controls                     */
/* ------------------------------------------------------------------ */

#define DBG "/sys/kernel/debug/dispdbg"

static int ensure_debugfs(void)
{
    if (access(DBG "/name", W_OK) == 0)
        return 0;
    mkdir("/sys/kernel/debug", 0755);
    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
    return access(DBG "/name", W_OK) == 0 ? 0 : -1;
}

static int clampi(int v, int lo, int hi);

static const int bl_table[11] = { 1, 25, 50, 75, 100, 125, 150, 175, 200, 225, 255 };

static int bl_get(void)
{
    if (ensure_debugfs() < 0)
        return -1;
    write_str_file(DBG "/name", "lcd0");
    write_str_file(DBG "/command", "getbl");
    write_str_file(DBG "/start", "1");
    usleep(40000);
    int raw = read_int_file(DBG "/param", -1);
    if (raw < 0)
        return -1;
    /* the hardware uses 0..255; convert back to the 0..10 UI level */
    int best = 0, dist = 1 << 30;
    for (int i = 0; i <= 10; i++) {
        int d = abs(raw - bl_table[i]);
        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return best;
}

static int bl_set(int level)
{
    if (ensure_debugfs() < 0)
        return -1;
    level = clampi(level, 0, 10);
    write_str_file(DBG "/name", "lcd0");
    write_str_file(DBG "/command", "setbl");
    write_int_file(DBG "/param", bl_table[level]);
    write_str_file(DBG "/start", "1");
    usleep(20000);
    return 0;
}

static int sval_get(const char *key)
{
    char buf[256];
    if (read_cmd(buf, sizeof buf, SYSTEMVAL_BIN " %s", key) != 0)
        return -1;
    return atoi(buf);
}

static void sval_set(const char *key, int v)
{
    run_cmd(SYSTEMVAL_BIN " %s %d >/dev/null 2>&1", key, v);
}

/* keymon keeps its own live copy of volume/brightness in a SysV shared block
 * (libshmvar) and only reads system.json at boot. systemval alone does NOT
 * update it: the next press of a physical button would start from the stale
 * cached value and jump back. The CLI exposes the same cache: key 0 = vol,
 * key 1 = brightness, key 4 = vol_changed (notification consumed by MainUI). */
static int shm_get(int key)
{
    char buf[128];
    if (read_cmd(buf, sizeof buf, SHMVAR_BIN " %d", key) != 0)
        return -1;
    return atoi(buf);
}

static void shm_set(int key, int v)
{
    run_cmd(SHMVAR_BIN " %d %d >/dev/null 2>&1", key, v);
}

static int audio_digital_get(void)
{
    char buf[8192];
    if (read_cmd(buf, sizeof buf, "tinymix -D 0 get 'digital volume'") == 0 && buf[0]) {
        int v = atoi(buf);
        if (v >= 0 && v <= 63)
            return v;
    }
    if (read_cmd(buf, sizeof buf, "tinymix -D 0 contents") != 0)
        return -1;
    char *p = strstr(buf, "digital volume");
    if (!p)
        return -1;
    p += strlen("digital volume");
    while (*p == ' ' || *p == '\t')
        p++;
    return atoi(p);
}

static void audio_digital_set(int v)
{
    run_cmd("tinymix -D 0 set 'digital volume' %d >/dev/null 2>&1", v);
}

/* Stick LEDs: CrossMix stores ledswitch/ledvalue in system.json and shmvar,
 * but the driver only reacts to direct writes to /sys/class/led_anim. */
static void led_apply(int on, int level)
{
    write_int_file(LED_SYS "effect_enable", on ? 1 : 0);
    write_int_file(LED_SYS "max_scale", clampi(level, 0, 10) * 4);
    sval_set("ledswitch", on ? 1 : 0);
    sval_set("ledvalue", level);
    shm_set(8, on ? 1 : 0);
    shm_set(10, level);
}

/* The color slider is language-neutral: 0 = white (firmware default),
 * 1..99 = a hue around the color wheel, 100 = LEDs off. HSV with full
 * saturation and value gives the brightest possible color for each hue. */
static uint32_t led_value_to_rgb(int v)
{
    if (v <= 0)
        return 0xFFFFFF;
    if (v >= 100)
        return 0x000000;

    return led_hue_rgb((int)((v - 1) * 360 / 98));
}

static int led_rgb_to_value(uint32_t rgb)
{
    int best = 0;
    long best_dist = 1L << 30;
    int tr = (int)((rgb >> 16) & 0xFF);
    int tg = (int)((rgb >> 8) & 0xFF);
    int tb = (int)(rgb & 0xFF);
    for (int v = 0; v <= 100; v++) {
        uint32_t c = led_value_to_rgb(v);
        long d = labs((long)((c >> 16) & 0xFF) - tr) +
                 labs((long)((c >> 8) & 0xFF) - tg) +
                 labs((long)(c & 0xFF) - tb);
        if (d < best_dist) {
            best_dist = d;
            best = v;
        }
    }
    return best;
}

static int battery_capacity(void)
{
    return read_int_file("/sys/class/power_supply/axp2202-battery/capacity", -1);
}

/* ------------------------------------------------------------------ */
/* wireless toggles                                                    */
/* ------------------------------------------------------------------ */

/* Stock firmware keeps the on/off setting in system.json ("wifi" and
 * "bluetooth") and MainUI applies it with the commands below. rfkill(8) is a
 * broken busybox applet here, so sysfs is used directly for Bluetooth power. */
#define BT_RFKILL_STATE "/sys/class/rfkill/rfkill0/state"

/* MainUI caches Bluetooth state in two ints: sBluetoothEnable (0x7ba6a0,
 * read from system.json) and sBluetoothOpened (0x80f894, set by its own
 * toggle). Its settings page shows On only when both are non-zero. MainUI is
 * non-PIE (ET_EXEC), so poke them to keep its menu in sync with this overlay. */
#define MAINUI_BT_ENABLE 0x7ba6a0u
#define MAINUI_BT_OPENED 0x80f894u

static int g_wifi_on = 0, g_bt_on = 0, g_radio = 0; /* 0 = WiFi, 1 = BT */

static void poke_mainui(int addr, int val)
{
    char pidbuf[64], path[96];
    if (read_cmd(pidbuf, sizeof pidbuf, "pgrep -x MainUI 2>/dev/null | head -1") != 0 ||
        !pidbuf[0] || !isdigit((unsigned char)pidbuf[0]))
        return;
    int pid = atoi(pidbuf);
    if (pid <= 0)
        return;
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    uint32_t v = val ? 1u : 0u;
    (void)pwrite(fd, &v, sizeof v, (off_t)addr);
    close(fd);
}

static void mainui_set_bt_state(int on)
{
    poke_mainui(MAINUI_BT_ENABLE, on);
    poke_mainui(MAINUI_BT_OPENED, on);
}

static int wifi_state_get(void)
{
    int v = sval_get("wifi");
    return (v == 0 || v == 1) ? v : 1;
}

static void wifi_state_set(int on)
{
    sval_set("wifi", on);
    if (on) {
        run_cmd("ifconfig wlan0 up >/dev/null 2>&1");
        run_cmd("wpa_supplicant -B -D nl80211 -iwlan0 "
                "-c /etc/wifi/wpa_supplicant.conf -O /etc/wifi/sockets >/dev/null 2>&1");
        run_cmd("udhcpc -i wlan0 >/dev/null 2>&1 &");
    } else {
        run_cmd("ifconfig wlan0 down >/dev/null 2>&1");
        run_cmd("killall -15 wpa_supplicant >/dev/null 2>&1");
        run_cmd("killall -15 udhcpc >/dev/null 2>&1");
    }
    logm("wifi = %d", on);
}

static int bt_state_get(void)
{
    int v = sval_get("bluetooth");
    if (v == 0 || v == 1)
        return v;
    return read_int_file(BT_RFKILL_STATE, 0) ? 1 : 0;
}

static void bt_state_set(int on)
{
    sval_set("bluetooth", on);
    if (on) {
        run_cmd("echo 1 > " BT_RFKILL_STATE " 2>/dev/null");
        run_cmd("timeout 3 /etc/init.d/hciattach start >/dev/null 2>&1");
        run_cmd("timeout 3 hciconfig hci0 up >/dev/null 2>&1");
        run_cmd("timeout 3 /etc/bluetooth/bluetoothd start >/dev/null 2>&1");
        run_cmd("timeout 3 /usr/bin/bluetoothctl power on </dev/null >/dev/null 2>&1 &");
        run_cmd("/usr/trimui/bin/trimui_btmanager </dev/null >/dev/null 2>&1 &");
        run_cmd("/usr/bin/bluealsa -p a2dp-source </dev/null >/dev/null 2>&1 &");
        run_cmd("touch /tmp/bluetooth_ready 2>/dev/null");
    } else {
        run_cmd("timeout 1 sh -c 'echo switchoff > /tmp/bluetooth_request' >/dev/null 2>&1");
        run_cmd("/etc/init.d/hciattach stop >/dev/null 2>&1");
        run_cmd("rm -f /tmp/bluetooth_ready");
        run_cmd("/etc/bluetooth/bluetoothd stop >/dev/null 2>&1");
        run_cmd("killall -15 bluealsa >/dev/null 2>&1");
        run_cmd("killall -9 trimui_btmanager >/dev/null 2>&1");
        run_cmd("killall -9 bluetoothctl >/dev/null 2>&1");
        run_cmd("echo 0 > " BT_RFKILL_STATE " 2>/dev/null");
    }
    mainui_set_bt_state(on);
    logm("bluetooth = %d", on);
}

static void radio_toggle(int which)
{
    if (which == 0) {
        g_wifi_on = !g_wifi_on;
        wifi_state_set(g_wifi_on);
    } else {
        g_bt_on = !g_bt_on;
        bt_state_set(g_bt_on);
    }
    g_dirty = 1;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Authoritative volume level (0..20): keymon's live shared state first, then
 * system.json, then the ALSA hardware value. */
static int current_volume(void)
{
    int vol = shm_get(0);
    if (vol < 0 || vol > 20)
        vol = sval_get("vol");
    if (vol >= 0 && vol <= 20)
        return vol;
    int d = audio_digital_get();
    return (d >= 0) ? clampi((39 - d) / 2, 0, 20) : -1;
}

/* Same for the backlight level (0..10). */
static int current_brightness(void)
{
    int bl = shm_get(1);
    if (bl < 0 || bl > 10)
        bl = bl_get();
    if (bl < 0)
        bl = sval_get("brightness");
    return (bl >= 0 && bl <= 10) ? bl : -1;
}

static void item_init_values(void)
{
    int vol = current_volume();
    if (vol < 0)
        vol = 12;
    items[0].value = clampi(vol * 5, 0, 100);

    int bl = current_brightness();
    if (bl < 0)
        bl = 8;
    items[1].value = bl * 10;

    items[2].value = clampi(read_int_file(DISP_ATTR "enhance_contrast", 60), 0, 100);
    items[3].value = clampi(read_int_file(DISP_ATTR "enhance_bright", 60), 0, 100);
    items[4].value = clampi(read_int_file(DISP_ATTR "enhance_saturation", 80), 0, 100);

    int ct = read_int_file(DISP_ATTR "color_temperature", 0);
    if (ct < -50 || ct > 50)
        ct = 0;
    items[5].value = ct + 50;

    /* 0% = LEDs off, otherwise the level drives the driver directly */
    items[I_LEDBRI].value = clampi(sval_get("ledvalue"), 0, 10) * 10;
    if (!sval_get("ledswitch"))
        items[I_LEDBRI].value = 0;
    items[I_LEDCOLOR].value = led_rgb_to_value(g_cfg.led_color);
    items[I_MUTE].value = 0; /* start unmuted; mute always saves current volume */
    g_saved_vol = items[I_VOL].value;

    /* Image settings persist as 0..10 and are restored as value*10, so their
     * step must be 10: with step 5 the snap below made "left" a no-op. */
    for (int i = I_CON; i <= I_SAT; i++)
        items[i].step = 10;
    items[I_TMP].step = 5;
    items[I_VOL].step = 5;
    items[I_BRI].step = 10;
    items[I_LEDBRI].step = 10;

    g_wifi_on = wifi_state_get();
    g_bt_on = bt_state_get();
}

/* The physical volume buttons are EV_KEY 114/115 on event0 and are handled by
 * keymon, not by the gamepad fd we grabbed. Poll the real system state so
 * changes made while the overlay is open are reflected in the slider. */
static void item_refresh_volume(void)
{
    int vol = current_volume();
    if (vol >= 0) {
        int v = clampi(vol * 5, 0, 100);
        if (v != items[0].value) {
            items[0].value = v;
            g_dirty = 1;
        }
    }
}

/* Same for the backlight (also driven by keymon via event0). Polled less
 * often because bl_get() writes to debugfs and sleeps briefly. */
static void item_refresh_brightness(void)
{
    int bl = current_brightness();
    if (bl >= 0) {
        int v = clampi(bl * 10, 0, 100);
        if (v != items[1].value) {
            items[1].value = v;
            g_dirty = 1;
        }
    }
}

static void item_apply(int idx)
{
    item_t *it = &items[idx];
    switch (idx) {
    case 0: {
        int vol = clampi(it->value / 5, 0, 20);
        audio_digital_set(clampi(39 - 2 * vol, 0, 63));
        sval_set("vol", vol);
        shm_set(0, vol);        /* keymon's live cache, so buttons keep our value */
        shm_set(4, 1);          /* vol_changed notification for MainUI */
        break;
    }
    case 1: {
        int bl = clampi(it->value / 10, 0, 10);
        bl_set(bl);
        sval_set("brightness", bl);
        shm_set(1, bl);         /* same issue with the physical brightness keys */
        break;
    }
    case 2:
        write_int_file(DISP_ATTR "enhance_contrast", it->value);
        sval_set("colorcontrast", it->value / 10);
        break;
    case 3:
        write_int_file(DISP_ATTR "enhance_bright", it->value);
        sval_set("colorbrightness", it->value / 10);
        break;
    case 4:
        write_int_file(DISP_ATTR "enhance_saturation", it->value);
        sval_set("colorsaturation", it->value / 10);
        break;
    case 5: {
        int ct = it->value - 50;
        write_int_file(DISP_ATTR "color_temperature", ct);
        sval_set("colortemperature", ct / 5 + 10);
        break;
    }
    case I_LEDBRI:
        led_apply(it->value > 0, it->value / 10);
        break;
    case I_LEDCOLOR: {
        uint32_t rgb = led_value_to_rgb(it->value);
        led_color_apply(rgb);
        /* Defer the config write to panel close so holding < > doesn't touch
         * the SD card on every D-pad repeat. */
        g_cfg.led_color = rgb;
        g_cfg.led_color_set = 1;
        g_led_color_changed = 1;
        break;
    }
    case I_MUTE:
        if (it->value) {
            g_saved_vol = items[I_VOL].value;
            items[I_VOL].value = 0;
        } else {
            items[I_VOL].value = g_saved_vol;
        }
        item_apply(I_VOL); /* real mute: set volume to zero */
        /* firmware flag: 1 = sound on, 0 = muted */
        shm_set(3, it->value ? 0 : 1);
        sval_set("mute", it->value ? 0 : 1);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* layout + drawing                                                   */
/* ------------------------------------------------------------------ */

/* Switch-style quick settings: a drawer that slides in from the right. */
#define PANEL_W      540
#define PANEL_X      (SCR_W - PANEL_W - 32)
#define PANEL_TOP    28
#define PANEL_H      664
#define ROW_H        30
#define ROW_TOP      (PANEL_TOP + 88)

static void item_value_text(int idx, char *buf, size_t n)
{
    if (idx == I_TMP)
        snprintf(buf, n, "%+d", items[idx].value - 50);
    else if (idx == I_LEDCOLOR) {
        uint32_t rgb = led_value_to_rgb(items[idx].value);
        if (!rgb)
            snprintf(buf, n, "%s", TR(S_OFF));
        else
            snprintf(buf, n, "#%06X", rgb);
    }
    else if (items[idx].kind == IT_TOGGLE)
        snprintf(buf, n, "%s", items[idx].value ? TR(S_ON) : TR(S_OFF));
    else
        snprintf(buf, n, "%d%%", items[idx].value);
}

/* ease-out cubic, 0 = fully off-screen right, 1 = fully open */
static float panel_progress(void)
{
    long t = g_closing ? (g_close_at + ANIM_MS - now_ms()) : (now_ms() - g_anim_start);
    float p = (float)t / (float)ANIM_MS;
    if (p < 0)
        p = 0;
    if (p > 1)
        p = 1;
    return 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
}

static void draw_row_slider(int x, int y, int w, int value, int focused)
{
    int h = 6;
    fill_round_a(x, y, w, h, h / 2, 0x41434E, 255);
    int fw = w * value / 100;
    if (fw > 0)
        fill_round_a(x, y, fw < h ? h : fw, h, h / 2,
                     focused ? 0xE62037 : 0x9E2230, 255);
    int kx = x + fw;
    if (kx < x + h)
        kx = x + h;
    if (kx > x + w - h)
        kx = x + w - h;
    fill_circle_a(kx, y + h / 2, focused ? 8 : 6, 0xF5F6FA, 255);
    fill_circle_a(kx, y + h / 2, focused ? 5 : 3, 0xE62037, 255);
}

/* Same track as the other sliders, but filled with the colors it selects:
 * white -> hue wheel -> black. */
static void draw_color_slider(int x, int y, int w, int value, int focused)
{
    int h = 6;
    for (int i = 0; i < w; i++) {
        int v = i * 100 / (w > 1 ? w - 1 : 1);
        fill_rect_a(x + i, y, 1, h, led_value_to_rgb(v), 255);
    }
    int kx = x + w * value / 100;
    if (kx < x + h)
        kx = x + h;
    if (kx > x + w - h)
        kx = x + w - h;
    fill_circle_a(kx, y + h / 2, focused ? 9 : 7, 0x101016, 255);
    fill_circle_a(kx, y + h / 2, focused ? 7 : 5, 0xF5F6FA, 255);
}

static void draw_confirm(void)
{
    fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, 170);

    const char *q = TR(S_CONFIRM_EXIT);
    if (g_confirm == ACT_REBOOT)
        q = TR(S_CONFIRM_REBOOT);
    else if (g_confirm == ACT_POWEROFF)
        q = TR(S_CONFIRM_POWER);

    int w = 640, h = 230;
    int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    fill_round_a(x, y, w, h, 26, 0x1A1B22, 250);

    draw_text(q, SCR_W / 2, y + 56, g_f_mid, 0xF0F2F8, 1);
    draw_text(TR(S_CONFIRM_KEYS), SCR_W / 2, y + 132, g_f_mid, 0x9698A2, 1);
}

/* ------------------------------------------------------------------ */
/* virtual keyboard                                                    */
/* ------------------------------------------------------------------ */

/* The panel stays frozen behind this screen but is not drawn while the
 * keyboard is up. Keys are injected through a dedicated uinput device so the
 * app under the overlay receives them exactly like physical key presses. */
#define KB_ROWS      4
#define KB_CTRL_ROW  4
#define KB_CTRL_KEYS 5
#define KB_ROW_TOP   254
#define KB_ROW_STEP  82
#define KB_KEY_H     70
#define KB_KEY_GAP   12

static const char *const g_kb_layout[2][KB_ROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
    { "!@#$%^&*()", "-_=+[]{}", ";:'\",./?", "\\|~<>" }
};

static int kb_row_len(int row)
{
    if (row == KB_CTRL_ROW)
        return KB_CTRL_KEYS;
    return (int)strlen(g_kb_layout[g_kb_symbols ? 1 : 0][row]);
}

static char kb_char(int row, int col)
{
    return g_kb_layout[g_kb_symbols ? 1 : 0][row][col];
}

/* Linux keycodes are QWERTY-position based, not alphabetical. */
static const unsigned short g_kb_codes[26] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
    KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
    KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V,
    KEY_W, KEY_X, KEY_Y, KEY_Z
};

static void kb_char_code(char ch, int *code, int *shift)
{
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') {
        *code = g_kb_codes[ch - 'a'];
        return;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *code = g_kb_codes[ch - 'A'];
        *shift = 1;
        return;
    }
    if (ch >= '1' && ch <= '9') {
        *code = KEY_1 + (ch - '1');
        return;
    }
    if (ch == '0') {
        *code = KEY_0;
        return;
    }
    switch (ch) {
    case ' ':  *code = KEY_SPACE; break;
    case '\n': *code = KEY_ENTER; break;
    case '\b': *code = KEY_BACKSPACE; break;
    case '-':  *code = KEY_MINUS; break;
    case '_':  *code = KEY_MINUS; *shift = 1; break;
    case '=':  *code = KEY_EQUAL; break;
    case '+':  *code = KEY_EQUAL; *shift = 1; break;
    case '[':  *code = KEY_LEFTBRACE; break;
    case '{':  *code = KEY_LEFTBRACE; *shift = 1; break;
    case ']':  *code = KEY_RIGHTBRACE; break;
    case '}':  *code = KEY_RIGHTBRACE; *shift = 1; break;
    case '\\': *code = KEY_BACKSLASH; break;
    case '|':  *code = KEY_BACKSLASH; *shift = 1; break;
    case '`':  *code = KEY_GRAVE; break;
    case '~':  *code = KEY_GRAVE; *shift = 1; break;
    case ';':  *code = KEY_SEMICOLON; break;
    case ':':  *code = KEY_SEMICOLON; *shift = 1; break;
    case '\'': *code = KEY_APOSTROPHE; break;
    case '"':  *code = KEY_APOSTROPHE; *shift = 1; break;
    case ',':  *code = KEY_COMMA; break;
    case '<':  *code = KEY_COMMA; *shift = 1; break;
    case '.':  *code = KEY_DOT; break;
    case '>':  *code = KEY_DOT; *shift = 1; break;
    case '/':  *code = KEY_SLASH; break;
    case '?':  *code = KEY_SLASH; *shift = 1; break;
    case '!':  *code = KEY_1; *shift = 1; break;
    case '@':  *code = KEY_2; *shift = 1; break;
    case '#':  *code = KEY_3; *shift = 1; break;
    case '$':  *code = KEY_4; *shift = 1; break;
    case '%':  *code = KEY_5; *shift = 1; break;
    case '^':  *code = KEY_6; *shift = 1; break;
    case '&':  *code = KEY_7; *shift = 1; break;
    case '*':  *code = KEY_8; *shift = 1; break;
    case '(':  *code = KEY_9; *shift = 1; break;
    case ')':  *code = KEY_0; *shift = 1; break;
    default:   *code = 0; break;
    }
}

static void kb_emit_to(int fd, int code, int val)
{
    if (fd < 0)
        return;
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = EV_KEY;
    e.code = code;
    e.value = val;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e)
        return;
    memset(&e, 0, sizeof e);
    e.type = EV_SYN;
    e.code = SYN_REPORT;
    write(fd, &e, sizeof e);
}

static void kb_emit(int code, int val)
{
    kb_emit_to(g_kbd_fd, code, val);
    if (g_text_to_gamepad)
        kb_emit_to(g_gamepad_fd, code, val);
}

static void kb_inject(char ch)
{
    int code = 0, shift = 0;
    kb_char_code(ch, &code, &shift);
    if ((g_kbd_fd < 0 && (!g_text_to_gamepad || g_gamepad_fd < 0)) || !code)
        return;
    if (shift)
        kb_emit(KEY_LEFTSHIFT, 1);
    kb_emit(code, 1);
    kb_emit(code, 0);
    if (shift)
        kb_emit(KEY_LEFTSHIFT, 0);
}

static void kb_device_open(void)
{
    if (g_kbd_fd >= 0)
        return;
    /* Prefer the keyboard created by overlayd at startup: games can enumerate
     * it when they launch, before the overlay is ever opened. */
    const char *inherited = getenv("OVERLAY_KBD_FD");
    if (inherited && *inherited) {
        int fd = atoi(inherited);
        if (fcntl(fd, F_GETFD) >= 0) {
            g_kbd_fd = fd;
            g_kbd_inherited = 1;
            logm("using inherited virtual keyboard fd %d", fd);
            return;
        }
    }
    g_kbd_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (g_kbd_fd < 0) {
        logm("cannot open /dev/uinput: %s", strerror(errno));
        return;
    }
    ioctl(g_kbd_fd, UI_SET_EVBIT, EV_KEY);
    for (unsigned i = 0; i < sizeof g_kb_codes / sizeof g_kb_codes[0]; i++)
        ioctl(g_kbd_fd, UI_SET_KEYBIT, g_kb_codes[i]);
    /* KEY_1..KEY_9 are 2..10 and KEY_0 is 11, so iterate low to high. */
    for (int c = KEY_1; c <= KEY_0; c++)
        ioctl(g_kbd_fd, UI_SET_KEYBIT, c);
    static const int extra[] = {
        KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT,
        KEY_SLASH, KEY_BACKSLASH, KEY_LEFTBRACE, KEY_RIGHTBRACE,
        KEY_MINUS, KEY_EQUAL, KEY_SPACE, KEY_ENTER, KEY_BACKSPACE,
        KEY_LEFTSHIFT
    };
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++)
        ioctl(g_kbd_fd, UI_SET_KEYBIT, extra[i]);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, sizeof ud.name, "OverLyder Virtual Keyboard");
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor = 0x1234;
    ud.id.product = 0x5679;
    ud.id.version = 1;
    if (write(g_kbd_fd, &ud, sizeof ud) != (ssize_t)sizeof ud ||
        ioctl(g_kbd_fd, UI_DEV_CREATE) < 0) {
        logm("uinput keyboard setup failed: %s", strerror(errno));
        close(g_kbd_fd);
        g_kbd_fd = -1;
    }
}

static void kb_device_close(void)
{
    if (g_kbd_fd < 0)
        return;
    if (!g_kbd_inherited) {
        ioctl(g_kbd_fd, UI_DEV_DESTROY);
        close(g_kbd_fd);
    }
    g_kbd_fd = -1;
    g_kbd_inherited = 0;
}

static void kb_key_label(int row, int col, char *out, size_t n)
{
    if (row == KB_CTRL_ROW) {
        static const char *const names[KB_CTRL_KEYS] = {
            "Shift", "", "Space", "Del", "OK"
        };
        if (col == 1)
            snprintf(out, n, "%s", g_kb_symbols ? "ABC" : "123");
        else
            snprintf(out, n, "%s", names[col]);
        return;
    }
    char ch = kb_char(row, col);
    if (g_kb_shift && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    snprintf(out, n, "%c", ch);
}

static void kb_backspace(void)
{
    size_t l = strlen(g_kb_text);
    if (l)
        g_kb_text[l - 1] = '\0';
    kb_inject('\b');
    g_dirty = 1;
}

static void kb_type(char ch)
{
    size_t l = strlen(g_kb_text);
    if (l >= sizeof(g_kb_text) - 1) {
        memmove(g_kb_text, g_kb_text + 1, l - 1);
        l--;
    }
    g_kb_text[l] = ch;
    g_kb_text[l + 1] = '\0';
    kb_inject(ch);
    g_dirty = 1;
}

static void kb_close(void)
{
    text_save(g_kb_text);
    g_kb_open = 0;
    g_kb_closed_at = now_ms();
    g_sel = I_KEYBOARD; /* land back on the option that opened the keyboard */
    g_dirty = 1;
}

/* Keyboard-only session (opened by the dedicated shortcut): B/Start close the
 * keyboard and leave the overlay immediately, without drawing the panel. */
static void kb_exit(void)
{
    text_save(g_kb_text);
    g_kb_open = 0;
    g_kb_only = 0;
    g_exit_code = EXIT_CLOSE;
    g_quit = 1;
    g_dirty = 1;
}

static void kb_activate(void)
{
    if (g_kb_row == KB_CTRL_ROW) {
        switch (g_kb_col) {
        case 0: g_kb_shift = !g_kb_shift; break;
        case 1: g_kb_symbols = !g_kb_symbols; g_kb_col = 0; break;
        case 2: kb_type(' '); break;
        case 3: kb_backspace(); break;
        case 4:
            kb_inject('\n');
            if (g_kb_only)
                kb_exit();
            else
                kb_close();
            break;
        default: break;
        }
    } else {
        char label[8];
        kb_key_label(g_kb_row, g_kb_col, label, sizeof label);
        kb_type(label[0]);
    }
    g_dirty = 1;
}

static void kb_move(int vertical, int dir)
{
    if (vertical) {
        int r = clampi(g_kb_row + dir, 0, KB_CTRL_ROW);
        if (r != g_kb_row) {
            g_kb_row = r;
            int n = kb_row_len(r);
            if (g_kb_col >= n)
                g_kb_col = n - 1;
        }
    } else {
        int n = kb_row_len(g_kb_row);
        g_kb_col = (g_kb_col + (dir > 0 ? 1 : -1) + n) % n;
    }
    g_dirty = 1;
}

/* Right stick moves the compact keyboard inside the screen bounds. */
static void kb_move_panel(void)
{
    if (!g_kb_open || g_kb_mode != 2 || g_rstick_max <= 0)
        return;
    int dx = 0, dy = 0;
    int dead = g_rstick_max / 5;
    if (abs(g_rx) > dead)
        dx = (int)(((long)g_rx * 12L) / g_rstick_max);
    if (abs(g_ry) > dead)
        dy = (int)(((long)g_ry * 12L) / g_rstick_max);
    if (!dx && !dy)
        return;
    int nx = clampi(g_kb_x + dx, 0, SCR_W - KB2_W);
    int ny = clampi(g_kb_y + dy, 0, SCR_H - KB2_H);
    if (nx != g_kb_x || ny != g_kb_y) {
        g_kb_x = nx;
        g_kb_y = ny;
        g_dirty = 1;
        if (g_debug)
            logm("kb panel %d,%d (rx=%d ry=%d)", g_kb_x, g_kb_y, g_rx, g_ry);
    }
}

static void kb_key(int code)
{
    /* ignore the tail of the A press that opened the keyboard */
    if (now_ms() - g_kb_open_at < 250)
        return;
    switch (code) {
    case K_A:      kb_activate(); break;
    case K_B:
    case K_MODE:
        if (g_kb_only)
            kb_exit();
        else
            kb_close();
        break;
    case K_Y:      g_kb_shift = !g_kb_shift; g_dirty = 1; break;
    case K_X:      g_kb_symbols = !g_kb_symbols; g_kb_col = 0; g_dirty = 1; break;
    case K_START:
        kb_inject('\n');
        if (g_kb_only)
            kb_exit();
        else
            kb_close();
        break;
    case K_SELECT: kb_backspace(); break;
    default: break;
    }
}

/* If the overlay was using SIGSTOP, thaw the game for the keyboard session
 * and move our drawing to a hardware layer so both can render at once. */
static void kb_release_game(void)
{
    FILE *f = fopen(STOPPED_FILE, "r");
    if (f) {
        int pid;
        while (fscanf(f, "%d", &pid) == 1) {
            kill(pid, SIGCONT);
            logm("keyboard resumed pid %d", pid);
        }
        fclose(f);
        unlink(STOPPED_FILE);
    }
    if (!g_use_layer) {
        if (overlay_layer_open(&g_layer, SCR_W, SCR_H) == 0) {
            g_use_layer = 1;
            overlay_layer_show(&g_layer);
            FILE *flag = fopen(LAYER_FLAG, "w");
            if (flag) {
                fprintf(flag, "%d %u\n", (int)getpid(), g_layer.channel);
                fclose(flag);
            }
        } else {
            logm("keyboard could not claim a layer; game still resumed");
        }
    }
}

static void kb_open(void)
{
    logm("keyboard opened");
    kb_release_game();
    g_kb_open = 1;
    g_kb_open_at = now_ms();
    g_kb_row = 0;
    g_kb_col = 0;
    g_stick_dir = 0;
    if (g_kb_mode == 2) {
        g_kb_x = (SCR_W - KB2_W) / 2;
        g_kb_y = SCR_H - KB2_H - 40;
        g_rx = g_ry = 0;
    }
    kb_device_open();
    g_dirty = 1;
}

static void kb_draw_keys(int px, int py, int pw, int row_top, int row_step,
                         int key_h, int key_w, int ctrl_w, int gap)
{
    for (int row = 0; row <= KB_CTRL_ROW; row++) {
        int n = kb_row_len(row);
        int kw = (row == KB_CTRL_ROW) ? ctrl_w : key_w;
        int total = n * kw + (n - 1) * gap;
        int sx = px + (pw - total) / 2;
        int y = py + row_top + row * row_step;
        for (int col = 0; col < n; col++) {
            int x = sx + col * (kw + gap);
            int focused = (row == g_kb_row && col == g_kb_col);
            int toggled = (row == KB_CTRL_ROW) &&
                          ((col == 0 && g_kb_shift) || (col == 1 && g_kb_symbols));
            fill_round_a(x + 3, y + 4, kw, key_h, 12, 0x000000, 100);
            fill_round_a(x, y, kw, key_h, 12,
                         focused ? 0xE62037 : (toggled ? 0x4A2530 : 0x242630), 250);
            if (focused)
                fill_round_a(x + 3, y + 3, 4, key_h - 6, 2, 0xFFFFFF, 255);
            char label[8];
            kb_key_label(row, col, label, sizeof label);
            draw_text(label, x + kw / 2, y + ((key_h - 28) > 0 ? (key_h - 28) / 2 : 0),
                      row == KB_CTRL_ROW ? g_f_small : g_f_mid,
                      focused ? 0xFFFFFF : 0xD0D2DC, 1);
        }
    }
}

static void kb_compose(void)
{
    if (g_kb_mode == 1) {
        /* Full keyboard: dims the whole screen and shows an input box. */
        if (g_use_layer) {
            memset(g_cv, 0, sizeof g_cv);
            fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, g_dim * 255 / 100);
        } else {
            memcpy(g_cv, g_bg_dim, sizeof g_cv);
            fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, 70);
        }
        draw_text(TR(S_KB_TITLE), SCR_W / 2, 64, g_f_mid, 0xE62037, 1);

        int fx = 80, fy = 112, fw = SCR_W - 160, fh = 84;
        fill_round_a(fx, fy, fw, fh, 16, 0x14151C, 250);
        fill_round_a(fx, fy, fw, 3, 2, 0xE62037, 255);

        const char *txt = g_kb_text;
        size_t l = strlen(txt);
        if (l > 48)
            txt += l - 48;
        char shown[64];
        snprintf(shown, sizeof shown, "%s", txt);
        if (!*shown) {
            draw_text(TR(S_KB_EMPTY), fx + 22, fy + 25, g_f_mid, 0x5F6170, 0);
        } else {
            draw_text(shown, fx + 22, fy + 25, g_f_mid, 0xF0F2F8, 0);
            SDL_Surface *s = cached_surface(g_f_mid, shown, shown, 0xF0F2F8);
            int cx = fx + 22 + (s ? s->w : 0) + 5;
            if ((now_ms() / 450) & 1)
                fill_rect_a(cx, fy + 23, 3, 30, 0xE62037, 255);
        }
        if (g_kbd_fd < 0)
            draw_text(TR(S_KB_UNAVAILABLE), SCR_W / 2, fy + fh + 10,
                      g_f_small, 0xE62037, 1);
        kb_draw_keys(0, 0, SCR_W, KB_ROW_TOP, KB_ROW_STEP, KB_KEY_H,
                     100, 176, KB_KEY_GAP);
        draw_text(TR(S_KB_HINT), SCR_W / 2, 686, g_f_small, 0x9698A2, 1);
        return;
    }

    /* Compact keyboard: small floating panel, no dim, no input box. It can be
     * moved anywhere on screen with the right stick; position is clamped to
     * the screen bounds. Keys go straight into the app. */
    if (g_use_layer)
        memset(g_cv, 0, sizeof g_cv);
    else
        memcpy(g_cv, g_bg, sizeof g_cv);

    fill_round_a(g_kb_x + 4, g_kb_y + 6, KB2_W, KB2_H, 16, 0x000000, 110);
    fill_round_a(g_kb_x, g_kb_y, KB2_W, KB2_H, 16, 0x14151C, 225);
    fill_round_a(g_kb_x, g_kb_y, KB2_W, 3, 2, 0xE62037, 255);
    draw_text(TR(S_KB_TITLE), g_kb_x + KB2_W / 2, g_kb_y + 4,
              g_f_small, 0xE62037, 1);
    kb_draw_keys(g_kb_x, g_kb_y, KB2_W, KB2_ROW_TOP, KB2_ROW_STEP,
                 KB2_KEY_H, KB2_KEY_W, KB2_CTRL_W, KB2_GAP);
    if (g_kbd_fd < 0)
        draw_text(TR(S_KB_UNAVAILABLE), g_kb_x + KB2_W / 2, g_kb_y + 224,
                  g_f_small, 0xE62037, 1);
    else
        draw_text(TR(S_KB_HINT), g_kb_x + KB2_W / 2, g_kb_y + 224,
                  g_f_small, 0x9698A2, 1);
}

static void compose(void)
{
    if (g_kb_open) {
        kb_compose();
        return;
    }
    if (g_use_layer) {
        memset(g_cv, 0, sizeof g_cv);
        fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, g_dim * 255 / 100);
    } else {
        memcpy(g_cv, g_bg_dim, sizeof g_cv);
    }

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tmv);
    int bat = battery_capacity();
    if (bat >= 0) {
        char bt[16];
        snprintf(bt, sizeof bt, "%d%%", bat);
        draw_icon(GL_BATTERY, 54, 42, 0x9698A2, 255);
        draw_text(bt, 74, 30, g_f_small, 0x9698A2, 0);
    }
    draw_text(TR(S_HINT_CLOSE), 44, 66, g_f_small, 0x70727E, 0);

    int px = PANEL_X + (int)((float)(SCR_W - PANEL_X + 60) * (1.0f - panel_progress()));
    fill_round_a(px + 8, PANEL_TOP + 10, PANEL_W, PANEL_H, 26, 0x000000, 110);
    fill_round_a(px, PANEL_TOP, PANEL_W, PANEL_H, 26, 0x14151C, 244);
    fill_round_a(px + 24, PANEL_TOP + 18, 5, 34, 2, 0xE62037, 255);
    draw_text(TR(S_AJUSTES), px + 44, PANEL_TOP + 20, g_f_mid, 0xF0F2F8, 0);
    draw_text(clk, px + PANEL_W - 28, PANEL_TOP + 24, g_f_mid, 0x9698A2, 2);

    for (int i = 0; i < NITEMS; i++) {
        item_t *it = &items[i];
        int y = ROW_TOP + i * ROW_H;
        int focused = (i == g_sel);
        int disabled = (it->action == ACT_EXIT_GAME && !g_has_game);
        uint32_t fg = disabled ? 0x5F6170 : (focused ? 0xF5F6FA : 0xC0C2CC);

        if (focused) {
            fill_round_a(px + 12, y - 2, PANEL_W - 24, ROW_H, 10, 0x2A2C36, 255);
            fill_round_a(px + 12, y - 2, 4, ROW_H, 2, 0xE62037, 255);
        }
        draw_icon(it->glyph, px + 46, y + ROW_H / 2 - 1, fg, disabled ? 110 : 255);
        draw_text(it->label, px + 76, y + 2, g_f_small, fg, 0);
        if (it->kind == IT_SLIDER || it->kind == IT_TOGGLE) {
            char vb[32];
            item_value_text(i, vb, sizeof vb);
            if (i == I_LEDCOLOR) {
                uint32_t rgb = led_value_to_rgb(it->value);
                fill_round_a(px + PANEL_W - 126, y + 3, 16, 14, 4,
                             rgb ? rgb : 0x2A2C36, 255);
            }
            draw_text(vb, px + PANEL_W - 28, y + 2, g_f_small,
                      focused ? 0xE62037 : 0x9698A2, 2);
            if (it->kind == IT_SLIDER) {
                if (i == I_LEDCOLOR)
                    draw_color_slider(px + 76, y + 24, PANEL_W - 104, it->value, focused);
                else
                    draw_row_slider(px + 76, y + 24, PANEL_W - 104, it->value, focused);
            }
        } else {
            draw_text(disabled ? "-" : (focused ? TR(S_ACCEPT) : ">"),
                      px + PANEL_W - 28, y + 2, g_f_small,
                      focused ? 0xE62037 : 0x7E808C, 2);
        }
    }

    /* WiFi / Bluetooth: two circular buttons in a strip under the list.
     * Green = on, grey = off; the focused one gets a wider highlight disc. */
    {
        int strip_top = ROW_TOP + NITEMS * ROW_H;
        int cy = strip_top + 24;
        int r = 20, step = 36;
        int cxs[2] = { px + PANEL_W / 2 - step, px + PANEL_W / 2 + step };
        int states[2] = { g_wifi_on, g_bt_on };
        uint32_t glyphs[2] = { GL_WIFI, GL_BT };
        const char *names[2] = { TR(S_WIFI), TR(S_BT) };
        for (int k = 0; k < 2; k++) {
            int focused = SEL_IS_RADIO(g_sel) && g_radio == k;
            int on = states[k];
            if (focused)
                fill_circle_a(cxs[k], cy, r + 4, 0x2A2C36, 255);
            fill_circle_a(cxs[k], cy, r, on ? 0x2ECC71 : 0x33353F, 255);
            draw_icon(glyphs[k], cxs[k], cy,
                      on ? 0x0B1A10 : (focused ? 0xF5F6FA : 0x9A9CA6), 255);
            draw_text(names[k], cxs[k], cy + r + 3, g_f_small,
                      focused ? 0xF5F6FA : 0x9698A2, 1);
        }
    }

    fill_round_a(px + 20, PANEL_TOP + PANEL_H - 46, PANEL_W - 40, 34, 10, 0x1A1B22, 255);
    if (SEL_IS_RADIO(g_sel))
        draw_text(TR(S_HINT_RADIO),
                  px + PANEL_W / 2, PANEL_TOP + PANEL_H - 38, g_f_small, 0xB0B2BC, 1);
    else if (items[g_sel].action == ACT_KEYBOARD)
        draw_text(TR(S_HINT_KEYBOARD),
                  px + PANEL_W / 2, PANEL_TOP + PANEL_H - 38, g_f_small, 0xB0B2BC, 1);
    else if (items[g_sel].kind == IT_SLIDER || items[g_sel].kind == IT_TOGGLE)
        draw_text(TR(S_HINT_SLIDER),
                  px + PANEL_W / 2, PANEL_TOP + PANEL_H - 38, g_f_small, 0xB0B2BC, 1);
    else
        draw_text(TR(S_HINT_ACTION),
                  px + PANEL_W / 2, PANEL_TOP + PANEL_H - 38, g_f_small, 0xB0B2BC, 1);

    if (g_confirm)
        draw_confirm();

    if (g_busy) {
        fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, 190);
        fill_round_a(SCR_W / 2 - 320, SCR_H / 2 - 70, 640, 140, 24, 0x1A1B22, 250);
        draw_text(TR(S_BUSY), SCR_W / 2, SCR_H / 2 - 28, g_f_mid, 0xF0F2F8, 1);
    }
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static void do_close(int code);

static int key_index_from_code(int code)
{
    for (int i = 0; i < NKEYS; i++)
        if (g_keys[i].code == code)
            return i;
    return 0;
}

static void custom_from_codes(const int *codes, int count)
{
    for (int i = 0; i < 4; i++)
        g_custom[i] = (i < count) ? key_index_from_code(codes[i]) : 0;
}

static void custom_default(void)
{
    g_custom[0] = 1; /* Home / Menu */
    for (int i = 1; i < 4; i++)
        g_custom[i] = 0;
}

static int custom_any(void)
{
    for (int i = 0; i < 4; i++)
        if (g_custom[i])
            return 1;
    return 0;
}

static void config_load(void)
{
    g_cfg_enabled = (access(DISABLE_FLAG, F_OK) != 0);
    g_cfg_confirm = g_cfg.confirm_exit;
    g_cfg_sigstop = g_cfg.no_sigstop;
    g_cfg_kb_mode = g_cfg.keyboard_mode;
    custom_from_codes(g_cfg.kbd_trigger, g_cfg.kbd_trigger_count);
    for (int i = 0; i < 4; i++)
        g_kb_custom[i] = g_custom[i];
    g_kb_custom_count = g_cfg.kbd_trigger_count;
    g_cfg_preset = 0;
    int found = 0;
    for (int p = 0; p < NPRESETS; p++) {
        if (g_presets[p].count != g_cfg.trigger_count)
            continue;
        int same = 1;
        for (int i = 0; i < g_cfg.trigger_count; i++)
            if (g_presets[p].codes[i] != g_cfg.trigger[i])
                same = 0;
        if (same) {
            g_cfg_preset = p;
            found = 1;
            break;
        }
    }
    if (found)
        custom_from_codes(g_presets[g_cfg_preset].codes, g_presets[g_cfg_preset].count);
    else {
        g_cfg_preset = PRESET_CUSTOM;
        custom_from_codes(g_cfg.trigger, g_cfg.trigger_count);
        if (!custom_any())
            custom_default();
    }
    g_cfg_hold = 1;
    for (int i = 0; i < NHOLDS; i++)
        if (abs(g_holds[i] - g_cfg.hold_ms) < abs(g_holds[g_cfg_hold] - g_cfg.hold_ms))
            g_cfg_hold = i;
}

/* Key rows only make sense for a custom trigger. */
static int cfg_visible(int i)
{
    if (i >= CFG_K1 && i <= CFG_K4)
        return g_cfg_preset == PRESET_CUSTOM;
    return 1;
}

static void cfg_move(int dir)
{
    for (int n = 0; n < NCFG; n++) {
        g_cfg_sel = (g_cfg_sel + dir + NCFG) % NCFG;
        if (cfg_visible(g_cfg_sel))
            break;
    }
}

static void config_save(void)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (f) {
        fprintf(f, "# OverLyder configuration\n");
        fprintf(f, "hold_ms=%d\n", g_holds[g_cfg_hold]);
        fprintf(f, "trigger=");
        int n = 0;
        if (g_cfg_preset == PRESET_CUSTOM) {
            if (!custom_any())
                custom_default();
            for (int i = 0; i < 4; i++) {
                int code = g_keys[g_custom[i]].code;
                if (!code)
                    continue;
                fprintf(f, "%s%d", n ? "," : "", code);
                n++;
            }
        } else {
            for (int i = 0; i < g_presets[g_cfg_preset].count; i++) {
                fprintf(f, "%s%d", i ? "," : "", g_presets[g_cfg_preset].codes[i]);
                n++;
            }
        }
        if (!n)
            fprintf(f, "%d", K_MODE);
        fprintf(f, "\n");
        fprintf(f, "dim=%d\n", g_cfg.dim);
        fprintf(f, "step=%d\n", g_cfg.step);
        fprintf(f, "confirm_exit=%d\n", g_cfg_confirm ? 1 : 0);
        fprintf(f, "no_sigstop=%d\n", g_cfg_sigstop ? 1 : 0);
        fprintf(f, "keyboard_mode=%d\n", g_cfg_kb_mode);
        fprintf(f, "fps_hud=%d\n", g_cfg.fps_hud ? 1 : 0);
        fprintf(f, "fps_hud_pos=%d\n", g_cfg.fps_hud_pos);
        fprintf(f, "fps_hud_style=%d\n", g_cfg.fps_hud_style);
        fprintf(f, "fps_hud_fields=%u\n", g_cfg.fps_hud_fields);
        fprintf(f, "kbd_trigger=");
        {
            int n = 0;
            for (int i = 0; i < 4 && i < g_kb_custom_count; i++) {
                int code = g_keys[g_kb_custom[i]].code;
                if (!code)
                    continue;
                fprintf(f, "%s%d", n ? "," : "", code);
                n++;
            }
        }
        fprintf(f, "\n");
        fprintf(f, "lang=%s\n", g_lang == LANG_EN ? "en" : (g_lang == LANG_PT ? "pt" : "es"));
        fprintf(f, "upd_reinstall=%d\n", g_cfg.upd_reinstall ? 1 : 0);
        if (g_cfg.led_color_set)
            fprintf(f, "ledcolor=%06X\n", g_cfg.led_color);
        fclose(f);
        sync();
    }
    if (g_cfg_enabled) {
        unlink(DISABLE_FLAG);
        /* Also work when the app was simply copied to Apps/ without running
         * install.sh: install the autostart hook and start the daemon here. */
        run_cmd("mkdir -p /mnt/SDCARD/System/starts /mnt/SDCARD/System/etc >/dev/null 2>&1");
        run_cmd("cp -f /mnt/SDCARD/Apps/OverLyder/overLyder.start "
                "/mnt/SDCARD/System/starts/overLyder.sh >/dev/null 2>&1");
        run_cmd("chmod 755 /mnt/SDCARD/System/starts/overLyder.sh >/dev/null 2>&1");
        FILE *sf = fopen(HUD_SYNC_FILE, "w");
        if (sf) {
            fclose(sf);
            sync();
        }
        if (g_mode_cfg) {
            /* The config app is independent of the daemon: a restart there
             * applies every setting and respawns the HUD. */
            run_cmd("/mnt/SDCARD/Apps/OverLyder/overlayctl restart >/dev/null 2>&1");
        }
        /* From the in-game panel we must NOT restart the daemon: the panel is
         * its child and PDEATHSIG would close it. overlayd sees the sync flag
         * and re-reads the config / respawns the HUD instead. */
    } else {
        FILE *flag = fopen(DISABLE_FLAG, "w");
        if (flag) {
            fprintf(flag, "disabled\n");
            fclose(flag);
        }
        run_cmd("kill $(cat " PID_FILE " 2>/dev/null) >/dev/null 2>&1");
    }
}

static void learn_finish(void)
{
    int n = 0;
    for (int i = 0; i < NKEYS; i++) {
        int code = g_keys[i].code;
        if (code && (g_learn_mask & (1 << (code & 31)))) {
            if (n < 4)
                g_custom[n++] = i;
        }
    }
    while (n < 4)
        g_custom[n++] = 0;
    if (!g_learn_mask)
        custom_default();
    g_cfg_preset = PRESET_CUSTOM;
    g_learning = 0;
    config_save();
}

/* Capture the dedicated keyboard shortcut. B with no keys disables it. */
static void learn_finish_kb(void)
{
    int n = 0;
    for (int i = 0; i < NKEYS && n < 4; i++) {
        int code = g_keys[i].code;
        if (code && (g_kb_learn_mask & (1u << (code & 31))))
            g_kb_custom[n++] = i;
    }
    g_kb_custom_count = n;
    g_kb_learning = 0;
    config_save();
}

static const char *hud_field_label(int i)
{
    static const int keys[] = {
        S_HUD_FPS, S_HUD_FRAME, S_HUD_CPU, S_HUD_GPU,
        S_HUD_TEMP, S_HUD_CPUFREQ, S_HUD_GPUFREQ, S_HUD_BATT
    };
    if (i < 0 || i >= HUD_F_COUNT)
        return "";
    return TR(keys[i]);
}

static const char *hud_pos_label(void)
{
    switch (g_cfg.fps_hud_pos) {
    case 1: return TR(S_HUD_POS_TR);
    case 2: return TR(S_HUD_POS_BL);
    case 3: return TR(S_HUD_POS_BR);
    default: return TR(S_HUD_POS_TL);
    }
}

static void hud_sub_open(void)
{
    g_hud_sub = 1;
    g_hud_sel = HUD_R_ENABLED;
    g_dirty = 1;
}

static void hud_sub_activate(void)
{
    if (g_hud_sel == HUD_R_BACK) {
        g_hud_sub = 0;
        g_dirty = 1;
        return;
    }
    if (g_hud_sel == HUD_R_ENABLED) {
        g_cfg.fps_hud = !g_cfg.fps_hud;
    } else if (g_hud_sel == HUD_R_POS) {
        g_cfg.fps_hud_pos = (g_cfg.fps_hud_pos + 1) % 4;
    } else if (g_hud_sel == HUD_R_STYLE) {
        g_cfg.fps_hud_style = g_cfg.fps_hud_style == HUD_STYLE_SIMPLE
                                  ? HUD_STYLE_WINDOW : HUD_STYLE_SIMPLE;
    } else {
        unsigned bit = 1u << (g_hud_sel - HUD_R_FIELDS);
        g_cfg.fps_hud_fields ^= bit;
    }
    /* Same auto-save pattern as every other settings row. config_save()
     * restarts the daemon, which re-spawns the HUD with the new settings. */
    config_save();
    g_dirty = 1;
}

static void hud_sub_compose(void)
{
    /* Also called from the in-game overlay, so it must prepare the canvas. */
    if (g_bg_ok)
        memcpy(g_cv, g_bg_dim, sizeof g_cv);
    else {
        memset(g_cv, 0, sizeof g_cv);
        fill_rect_a(0, 0, SCR_W, SCR_H, 0x000000, g_dim * 255 / 100);
    }
    draw_text(TR(S_HUD_TITLE), SCR_W / 2, 150, g_f_big, 0xF0F2F8, 1);

    char labels[NHUDROWS][64], values[NHUDROWS][64];
    for (int i = 0; i < NHUDROWS; i++) {
        labels[i][0] = values[i][0] = '\0';
    }
    snprintf(labels[HUD_R_ENABLED], sizeof labels[0], "%s", TR(S_HUD_ENABLED));
    snprintf(values[HUD_R_ENABLED], sizeof values[0], "%s",
             g_cfg.fps_hud ? TR(S_SI) : TR(S_NO));
    snprintf(labels[HUD_R_POS], sizeof labels[0], "%s", TR(S_HUD_POS));
    snprintf(values[HUD_R_POS], sizeof values[0], "%s", hud_pos_label());
    snprintf(labels[HUD_R_STYLE], sizeof labels[0], "%s", TR(S_HUD_STYLE));
    snprintf(values[HUD_R_STYLE], sizeof values[0], "%s",
             g_cfg.fps_hud_style == HUD_STYLE_SIMPLE ? TR(S_HUD_SIMPLE)
                                                    : TR(S_HUD_WINDOW));
    for (int i = 0; i < HUD_F_COUNT; i++) {
        unsigned bit = 1u << i;
        snprintf(labels[HUD_R_FIELDS + i], sizeof labels[0], "%s", hud_field_label(i));
        snprintf(values[HUD_R_FIELDS + i], sizeof values[0], "%s",
                 (g_cfg.fps_hud_fields & bit) ? TR(S_SI) : TR(S_NO));
    }
    snprintf(labels[HUD_R_BACK], sizeof labels[0], "%s", TR(S_HUD_BACK));

    for (int i = 0; i < NHUDROWS; i++) {
        int y = 196 + i * 36;
        int focused = (i == g_hud_sel);
        fill_round_a(300, y, SCR_W - 600, 34, 10,
                     focused ? 0xE62037 : 0x1A1B22, focused ? 255 : 235);
        draw_text(labels[i], 340, y + 3, g_f_mid, focused ? 0xFFFFFF : 0xBCBEC8, 0);
        if (values[i][0])
            draw_text(values[i], SCR_W - 340, y + 3, g_f_mid,
                      focused ? 0xFFFFFF : 0xF0F2F8, 2);
    }
    draw_text(TR(S_HUD_HINT), SCR_W / 2, 684, g_f_small, 0x9698A2, 1);
}

static void config_activate(void)
{
    switch (g_cfg_sel) {
    case CFG_ESTADO:    g_cfg_enabled = !g_cfg_enabled; break;
    case CFG_GATILLO:   g_cfg_preset = (g_cfg_preset + 1) % NPRESETS; break;
    case CFG_K1:
    case CFG_K2:
    case CFG_K3:
    case CFG_K4: {
        int i = g_cfg_sel - CFG_K1;
        g_custom[i] = (g_custom[i] + 1) % NKEYS;
        break;
    }
    case CFG_UMBRAL:    g_cfg_hold = (g_cfg_hold + 1) % NHOLDS; break;
    case CFG_CONFIRMAR: g_cfg_confirm = !g_cfg_confirm; break;
    case CFG_IDIOMA:
        g_lang = (g_lang + 1) % NLANG;
        lang_load(g_lang);
        items_set_language();
        break;
    case CFG_SIGSTOP:   g_cfg_sigstop = !g_cfg_sigstop; break;
    case CFG_KBMODE:    g_cfg_kb_mode = (g_cfg_kb_mode == 1) ? 2 : 1; break;
    case CFG_KBHOTKEY:
        g_kb_learning = 1;
        g_kb_learn_mask = 0;
        g_kb_learn_at = now_ms();
        return; /* save only after the combination is captured */
    case CFG_LEARN:
        g_learning = 1;
        g_learn_mask = 0;
        g_learn_at = now_ms();
        return; /* do not save until the combination is captured */
    case CFG_RESET:
        cfg_defaults(&g_cfg);
        g_cfg_hold = 1;
        g_cfg_preset = 0;
        custom_from_codes(g_presets[0].codes, g_presets[0].count);
        g_cfg_confirm = 1;
        g_cfg_enabled = 1;
        g_cfg_sigstop = g_cfg.no_sigstop; /* default = layer, no SIGSTOP */
        g_cfg_kb_mode = g_cfg.keyboard_mode; /* default = compact */
        g_hud_sub = 0; /* cfg_defaults() above also resets the HUD fields */
        for (int i = 0; i < 4; i++)
            g_kb_custom[i] = key_index_from_code(g_cfg.kbd_trigger[i]);
        g_kb_custom_count = g_cfg.kbd_trigger_count;
        g_dim = g_cfg.dim;
        cfg_update_led_color(&g_cfg, 0xFFFFFF, 0);
        led_color_apply(0xFFFFFF);
        break;
    case CFG_ABOUT:
        g_about = 1;
        return;
    case CFG_UPDATE:
        /* The manifest can be edited while the app is open; re-read it so a
         * dummy/rolled-back version is picked up without restarting. */
        manifest_load(&g_manifest);
        upd_check();
        return;
    case CFG_LISTO:     do_close(EXIT_CLOSE); break;
    default: break;
    }
    config_save();
}

static void config_compose(void)
{
    memcpy(g_cv, g_bg_dim, sizeof g_cv);

    draw_text("OVERLYDER", SCR_W / 2, 55, g_f_big, 0xF0F2F8, 1);

    if (g_upd_state != UPD_NONE) {
        update_compose();
        return;
    }

    if (g_about) {
        char vb[64];
        snprintf(vb, sizeof vb, "OverLyder v%s", g_manifest.version);
        draw_text(vb, SCR_W / 2, 270, g_f_mid, 0xE62037, 1);
        draw_text(g_manifest.credits, SCR_W / 2, 330, g_f_mid, 0xF0F2F8, 1);
        draw_text(TR(S_ABOUT_TEXT), SCR_W / 2, 400, g_f_small, 0xC0C2CC, 1);
        draw_text(TR(S_ABOUT_HINT), SCR_W / 2, 600, g_f_small, 0x9698A2, 1);
        return;
    }

    if (g_hud_sub) {
        hud_sub_compose();
        return;
    }

    if (g_kb_learning) {
        draw_text(TR(S_KB_HOTKEY_TITLE), SCR_W / 2, 150, g_f_mid, 0xF0F2F8, 1);
        char names[256] = "";
        for (int i = 0; i < NKEYS; i++) {
            int code = g_keys[i].code;
            if (code && (g_kb_learn_mask & (1u << (code & 31)))) {
                if (*names)
                    strcat(names, " + ");
                strcat(names, key_name(i));
            }
        }
        draw_text(*names ? names : "...", SCR_W / 2, 300, g_f_big, 0xE62037, 1);
        return;
    }

    if (g_learning) {
        draw_text(TR(S_LEARN_TITLE), SCR_W / 2, 150, g_f_mid, 0xF0F2F8, 1);
        char names[256] = "";
        for (int i = 0; i < NKEYS; i++) {
            int code = g_keys[i].code;
            if (code && (g_learn_mask & (1 << (code & 31)))) {
                if (*names)
                    strcat(names, " + ");
                strcat(names, key_name(i));
            }
        }
        draw_text(*names ? names : "...", SCR_W / 2, 300, g_f_big, 0xE62037, 1);
        return;
    }

    draw_text(TR(S_CFG_SUBTITLE), SCR_W / 2, 112, g_f_mid, 0x9698A2, 1);

    char labels[NCFG][64], values[NCFG][64];
    snprintf(labels[CFG_ESTADO], sizeof labels[0], "%s", TR(S_CFG_ESTADO));
    snprintf(labels[CFG_GATILLO], sizeof labels[0], "%s", TR(S_CFG_GATILLO));
    for (int i = 0; i < 4; i++)
        snprintf(labels[CFG_K1 + i], sizeof labels[0], TR(S_KEYFMT), i + 1);
    snprintf(labels[CFG_UMBRAL], sizeof labels[0], "%s", TR(S_CFG_UMBRAL));
    snprintf(labels[CFG_CONFIRMAR], sizeof labels[0], "%s", TR(S_CFG_CONFIRMAR));
    snprintf(labels[CFG_IDIOMA], sizeof labels[0], "%s", TR(S_CFG_IDIOMA));
    snprintf(labels[CFG_SIGSTOP], sizeof labels[0], "%s", TR(S_CFG_SIGSTOP));
    snprintf(labels[CFG_KBMODE], sizeof labels[0], "%s", TR(S_CFG_KBMODE));
    snprintf(labels[CFG_KBHOTKEY], sizeof labels[0], "%s", TR(S_CFG_KBHOTKEY));
    snprintf(labels[CFG_LEARN], sizeof labels[0], "%s", TR(S_LEARN));
    snprintf(labels[CFG_RESET], sizeof labels[0], "%s", TR(S_RESET));
    snprintf(labels[CFG_ABOUT], sizeof labels[0], "%s", TR(S_ABOUT));
    snprintf(labels[CFG_UPDATE], sizeof labels[0], "%s", TR(S_UPDATE));
    snprintf(labels[CFG_LISTO], sizeof labels[0], "%s", TR(S_CFG_LISTO));

    snprintf(values[CFG_ESTADO], sizeof values[0], "%s",
             g_cfg_enabled ? TR(S_ENABLED) : TR(S_DISABLED_VAL));
    snprintf(values[CFG_GATILLO], sizeof values[0], "%s", preset_name(g_cfg_preset));
    for (int i = 0; i < 4; i++)
        snprintf(values[CFG_K1 + i], sizeof values[0], "%s", key_name(g_custom[i]));
    snprintf(values[CFG_UMBRAL], sizeof values[0], "%d ms", g_holds[g_cfg_hold]);
    snprintf(values[CFG_CONFIRMAR], sizeof values[0], "%s", g_cfg_confirm ? TR(S_SI) : TR(S_NO));
    snprintf(values[CFG_IDIOMA], sizeof values[0], "%s", LANG_NAME(g_lang));
    /* Displayed as "SIGSTOP": Sí = freeze the game, No = layer mode.
     * Internally g_cfg_sigstop is no_sigstop (1 = no SIGSTOP). */
    snprintf(values[CFG_SIGSTOP], sizeof values[0], "%s",
             g_cfg_sigstop ? TR(S_NO) : TR(S_SI));
    snprintf(values[CFG_KBMODE], sizeof values[0], "%s",
             g_cfg_kb_mode == 1 ? TR(S_KB_FULL) : TR(S_KB_COMPACT));
    if (g_kb_custom_count == 0) {
        snprintf(values[CFG_KBHOTKEY], sizeof values[0], "%s", TR(S_NONE));
    } else {
        char combo[64] = "";
        for (int i = 0; i < g_kb_custom_count; i++) {
            if (*combo)
                strcat(combo, "+");
            strcat(combo, key_name(g_kb_custom[i]));
        }
        snprintf(values[CFG_KBHOTKEY], sizeof values[0], "%s", combo);
    }
    values[CFG_LEARN][0] = '\0';
    values[CFG_RESET][0] = '\0';
    values[CFG_ABOUT][0] = '\0';
    snprintf(values[CFG_UPDATE], sizeof values[0], "v%s", g_manifest.version);
    values[CFG_LISTO][0] = '\0';

    /* 16 rows max: keep them above the footer at y=684. */
    int y0 = 140, row = 0;
    for (int i = 0; i < NCFG; i++) {
        if (!cfg_visible(i))
            continue;
        int y = y0 + row * 32;
        row++;
        int focused = (i == g_cfg_sel);
        fill_round_a(220, y, SCR_W - 440, 28, 10,
                     focused ? 0xE62037 : 0x1A1B22, focused ? 255 : 235);
        draw_text(labels[i], 260, y + 1, g_f_mid, focused ? 0xFFFFFF : 0xBCBEC8, 0);
        draw_text(values[i], SCR_W - 260, y + 1, g_f_mid, focused ? 0xFFFFFF : 0xF0F2F8, 2);
    }

    draw_text(TR(S_CFG_FOOTER), SCR_W / 2, 684, g_f_small, 0x9698A2, 1);
}

/* ------------------------------------------------------------------ */
/* self-updater (CrossMix app only - never in the in-game overlay)      */
/* ------------------------------------------------------------------ */

static void update_compose(void)
{
    draw_text(TR(S_UPDATE_TITLE), SCR_W / 2, 130, g_f_mid, 0xE62037, 1);

    if (g_upd_state == UPD_OPTIONS) {
        draw_text(TR(S_UPDATE_OPTIONS), SCR_W / 2, 210, g_f_mid, 0xF0F2F8, 1);
        const char *lbl[2] = { TR(S_UPDATE_REINSTALL), TR(S_UPDATE_BACK_ROW) };
        const char *val[2] = {
            g_cfg.upd_reinstall ? TR(S_ON) : TR(S_OFF), ""
        };
        for (int i = 0; i < 2; i++) {
            int y = 300 + i * 60;
            int focused = (i == g_upd_opt_sel);
            fill_round_a(220, y, SCR_W - 440, 44, 10,
                         focused ? 0xE62037 : 0x1A1B22, focused ? 255 : 235);
            draw_text(lbl[i], 260, y + 6, g_f_mid,
                      focused ? 0xFFFFFF : 0xBCBEC8, 0);
            draw_text(val[i], SCR_W - 260, y + 6, g_f_mid, 0xF0F2F8, 2);
        }
        draw_text(TR(S_UPDATE_OPTIONS_HINT), SCR_W / 2, 620, g_f_small, 0x9698A2, 1);
        return;
    }

    if (g_upd_state == UPD_CHECKING) {
        draw_text(TR(S_UPDATE_CHECKING), SCR_W / 2, 300, g_f_mid, 0xF0F2F8, 1);
        return;
    }
    if (g_upd_state == UPD_INSTALLING) {
        draw_text(TR(S_UPDATE_INSTALLING), SCR_W / 2, 300, g_f_mid, 0xF0F2F8, 1);
        return;
    }

    char buf[160];
    snprintf(buf, sizeof buf, "%s: v%s", TR(S_UPDATE_LOCAL), g_manifest.version);
    draw_text(buf, SCR_W / 2, 250, g_f_mid, 0xC0C2CC, 1);
    if (g_upd_latest[0]) {
        snprintf(buf, sizeof buf, "%s: v%s", TR(S_UPDATE_LATEST), g_upd_latest);
        draw_text(buf, SCR_W / 2, 300, g_f_mid, 0xC0C2CC, 1);
    }

    const char *msg = NULL;
    uint32_t color = 0xF0F2F8;
    switch (g_upd_state) {
    case UPD_UPTODATE:  msg = TR(S_UPDATE_UPTODATE); break;
    case UPD_AVAILABLE:
    case UPD_CONFIRM:   msg = g_upd_same ? TR(S_UPDATE_SAME_AVAIL)
                                         : TR(S_UPDATE_AVAILABLE); break;
    case UPD_INSTALLED: msg = TR(S_UPDATE_INSTALLED); break;
    case UPD_ERROR:     msg = TR(S_UPDATE_ERROR); color = 0xE62037; break;
    default: break;
    }
    if (msg)
        draw_text(msg, SCR_W / 2, 380, g_f_mid, color, 1);

    if (g_upd_state == UPD_AVAILABLE)
        draw_text(g_upd_same ? TR(S_UPDATE_SAME_CONFIRM) : TR(S_UPDATE_CONFIRM),
                  SCR_W / 2, 470, g_f_small, 0x9698A2, 1);
    draw_text(TR(S_UPDATE_HINT), SCR_W / 2, 540, g_f_small, 0x9698A2, 1);
    draw_text(TR(S_UPDATE_BACK), SCR_W / 2, 620, g_f_small, 0x9698A2, 1);
}

/* Numeric parts of "0.0.3" or "v0.0.3", so release tags and the manifest
 * version are compared the same way. */
static void ver_parts(const char *s, int *maj, int *min, int *pat)
{
    if (*s == 'v' || *s == 'V')
        s++;
    *maj = *min = *pat = 0;
    sscanf(s, "%d.%d.%d", maj, min, pat);
}

static int ver_newer(const char *a, const char *b)
{
    int am, an, ap, bm, bn, bp;
    ver_parts(a, &am, &an, &ap);
    ver_parts(b, &bm, &bn, &bp);
    if (am != bm) return am > bm;
    if (an != bn) return an > bn;
    return ap > bp;
}

static int ver_equal(const char *a, const char *b)
{
    int am, an, ap, bm, bn, bp;
    ver_parts(a, &am, &an, &ap);
    ver_parts(b, &bm, &bn, &bp);
    return am == bm && an == bn && ap == bp;
}

/* Decide what the updater offers from the last check. A release equal to the
 * installed one only offers a reinstall when that option is turned on. */
static void upd_recompute(void)
{
    if (!g_upd_latest[0]) {
        g_upd_state = UPD_UPTODATE;
        return;
    }
    g_upd_same = ver_equal(g_upd_latest, g_manifest.version);
    g_upd_state = (ver_newer(g_upd_latest, g_manifest.version) ||
                   (g_cfg.upd_reinstall && g_upd_same))
                      ? UPD_AVAILABLE : UPD_UPTODATE;
}

static void upd_options_open(void)
{
    g_upd_prev_state = g_upd_state;
    g_upd_opt_sel = 0;
    g_upd_state = UPD_OPTIONS;
    g_dirty = 1;
}

static void upd_options_back(void)
{
    if (g_upd_prev_state == UPD_UPTODATE || g_upd_prev_state == UPD_AVAILABLE)
        upd_recompute();
    else
        g_upd_state = g_upd_prev_state;
    g_dirty = 1;
}

static void upd_option_toggle(void)
{
    if (g_upd_opt_sel == 0)
        cfg_update_reinstall(&g_cfg, !g_cfg.upd_reinstall);
    g_dirty = 1;
}

/* updater.sh prints "OK <tag>" or "ERR <code>" on a single line. `arg` is the
 * release tag for install (the version currently running for check). */
static void upd_run(const char *op, const char *arg)
{
    char out[256] = "";
    char cmd[512];
    /* Last argument tells updater.sh whether reinstalling the same version
     * is allowed; check ignores it. */
    snprintf(cmd, sizeof cmd,
             "GITHUB_OWNER=%s GITHUB_REPO=%s sh " APP_DIR "/%s %s %s %s 2>/dev/null",
             g_manifest.owner, g_manifest.repo, g_manifest.updater, op, arg,
             g_cfg.upd_reinstall ? "1" : "0");
    if (read_cmd(out, sizeof out, "%s", cmd) != 0) {
        logm("updater %s %s: no output", op, arg);
        g_upd_state = UPD_ERROR;
        return;
    }
    char res[16] = "", got[64] = "";
    if (sscanf(out, "%15s %63s", res, got) < 1 || strcmp(res, "OK")) {
        logm("updater %s %s: %s", op, arg, out);
        g_upd_state = UPD_ERROR;
        return;
    }
    logm("updater %s %s: OK %s", op, arg, got);
    if (!strcmp(op, "check")) {
        snprintf(g_upd_latest, sizeof g_upd_latest, "%s", got);
        upd_recompute();
    } else {
        /* The zip replaced manifest.json on disk; pick up the new version. */
        manifest_load(&g_manifest);
        g_upd_state = UPD_INSTALLED;
    }
}

static void upd_check(void)
{
    g_upd_state = UPD_CHECKING;
    g_upd_latest[0] = '\0';
    g_dirty = 1;
    config_compose();
    display_flip();
    upd_run("check", g_manifest.version);
    g_dirty = 1;
}

static void upd_install(void)
{
    g_upd_state = UPD_INSTALLING;
    g_dirty = 1;
    config_compose();
    display_flip();
    upd_run("install", g_upd_latest);
    g_dirty = 1;
}

static int find_input_device(char *out, size_t outsz)
{
    const char *env = getenv("OVERLAY_INPUT_DEV");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return 0;
    }
    for (int i = 0; i < 40; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[256] = { 0 };
        int ok = ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0;
        close(fd);
        if (!ok)
            continue;
        if (strstr(name, "TRIMUI") || strstr(name, "Player1")) {
            snprintf(out, outsz, "%s", path);
            return 0;
        }
    }
    if (access("/dev/input/event3", F_OK) == 0) {
        snprintf(out, outsz, "%s", "/dev/input/event3");
        return 0;
    }
    return -1;
}

static void do_close(int code)
{
    g_exit_code = code;
    if (code == EXIT_CLOSE && !g_mode_cfg && !g_closing) {
        /* normal close: animate the drawer out to the right first */
        g_closing = 1;
        g_close_at = now_ms();
        g_dirty = 1;
    } else {
        g_quit = 1;
    }
}

static int read_stopped_pids(pid_t *out, int max)
{
    FILE *f = fopen(TARGETS_FILE, "r");
    if (!f)
        f = fopen(STOPPED_FILE, "r");
    if (!f)
        return 0;
    int n = 0, pid;
    while (n < max && fscanf(f, "%d", &pid) == 1)
        out[n++] = (pid_t)pid;
    fclose(f);
    return n;
}

static int mainui_running(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[64], buf[64];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
        if (!strcmp(buf, "MainUI")) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Close the game from the overlay: keep our image on screen while the
 * emulator dies and MainUI comes back, so the user never sees the
 * emulator splash / a frozen game frame. */
static void do_exit_game(void)
{
    pid_t pids[64];
    int n = read_stopped_pids(pids, 64);

    g_busy = 1;
    compose();
    display_flip();

    logm("closing game (%d pids)", n);
    for (int i = 0; i < n; i++)
        kill(pids[i], SIGCONT);
    for (int i = 0; i < n; i++)
        kill(pids[i], SIGTERM);

    for (int t = 0; t < 25; t++) {
        int alive = 0;
        for (int i = 0; i < n; i++)
            if (kill(pids[i], 0) == 0)
                alive++;
        if (!alive)
            break;
        usleep(100000);
    }
    for (int i = 0; i < n; i++)
        kill(pids[i], SIGKILL);
    sync();

    for (int t = 0; t < 70; t++) {
        if (mainui_running()) {
            g_keep_overlay = 1;
            break;
        }
        usleep(100000);
    }
    logm("mainui back: %d", g_keep_overlay);

    g_exit_code = EXIT_CLOSE;
    g_quit = 1;
}

static void move_sel(int d) { g_sel = (g_sel + d + NSEL) % NSEL; }

static void adjust_value(int d)
{
    item_t *it = &items[g_sel];
    if (it->kind == IT_TOGGLE) {
        it->value = it->value ? 0 : 1;
        item_apply(g_sel);
        logm("toggle %s = %d", it->label, it->value);
        return;
    }
    if (it->kind != IT_SLIDER)
        return;
    int v = clampi(it->value + d * it->step, 0, 100);
    /* system.json stores image/LED settings as 0..10 and MainUI multiplies
     * by 10 on boot. Snap to that grid so what is shown is what is restored. */
    if (g_sel >= I_CON && g_sel <= I_SAT)
        v = clampi(((v + 5) / 10) * 10, 0, 100);
    else if (g_sel == I_TMP)
        v = clampi(((v + 2) / 5) * 5, 0, 100);
    else if (g_sel == I_LEDBRI)
        v = clampi(((v + 5) / 10) * 10, 0, 100);
    if (v != it->value) {
        it->value = v;
        item_apply(g_sel);
        logm("set %s = %d", it->label, it->value);
    }
}

/* Left/Right adjust the focused slider directly (no extra "adjust mode");
 * on action rows they still move the selection. Up/Down only navigate. */
static void horiz_move(int dir)
{
    if (g_mode_cfg && (g_learning || g_about))
        return;
    if (g_hud_sub)
        return; /* the FPS submenu uses A/B, not left/right */
    if (g_mode_cfg && g_upd_state == UPD_OPTIONS) {
        if (g_upd_opt_sel == 0 && dir)
            upd_option_toggle();
        return;
    }
    if (g_mode_cfg && g_upd_state != UPD_NONE)
        return; /* no invisible config cursor movement behind the updater */
    if (g_kb_open) {
        kb_move(0, dir);
        return;
    }
    if (g_mode_cfg)
        cfg_move(dir);
    else if (!g_confirm) {
        if (SEL_IS_RADIO(g_sel))
            g_radio = (dir < 0) ? 0 : 1; /* left = WiFi, right = Bluetooth */
        else if (items[g_sel].kind == IT_SLIDER || items[g_sel].kind == IT_TOGGLE)
            adjust_value(dir);
        else
            move_sel(dir);
    }
}

static void vert_move(int dir)
{
    if (g_mode_cfg && (g_learning || g_about))
        return;
    if (g_hud_sub) {
        g_hud_sel = (g_hud_sel + (dir > 0 ? 1 : -1) + NHUDROWS) % NHUDROWS;
        g_dirty = 1;
        return;
    }
    if (g_mode_cfg && g_upd_state == UPD_OPTIONS) {
        g_upd_opt_sel = (g_upd_opt_sel + (dir > 0 ? 1 : -1) + 2) % 2;
        return;
    }
    if (g_mode_cfg && g_upd_state != UPD_NONE)
        return;
    if (g_kb_open) {
        kb_move(1, dir);
        return;
    }
    if (g_mode_cfg)
        cfg_move(dir);
    else if (!g_confirm)
        move_sel(dir);
}

/* The analog stick reports tiny values constantly. Only act when a direction
 * clearly dominates the other axis, and only on a change of direction, so a
 * held stick does not scroll through every item. */
static void stick_eval(void)
{
    int dead = g_stick_max ? g_stick_max / 4 : 32;
    int x = g_ax, y = g_ay;
    int d = 0; /* 1/-1 = right/left, 2/-2 = down/up */
    if (abs(x) > dead && abs(x) >= abs(y) * 2)
        d = x > 0 ? 1 : -1;
    else if (abs(y) > dead && abs(y) >= abs(x) * 2)
        d = y > 0 ? 2 : -2;
    if (d == g_stick_dir)
        return;
    g_stick_dir = d;
    if (d == 1 || d == -1)
        horiz_move(d);
    else if (d == 2)
        vert_move(1);
    else if (d == -2)
        vert_move(-1);
}

static void activate(void)
{
    if (g_confirm) {
        int act = g_confirm;
        g_confirm = ACT_NONE;
        if (act == ACT_EXIT_GAME) {
            do_exit_game();
        } else if (act == ACT_REBOOT) {
            do_close(EXIT_REBOOT);
        } else if (act == ACT_POWEROFF) {
            do_close(EXIT_POWEROFF);
        }
        return;
    }
    if (g_hud_sub) {
        hud_sub_activate();
        return;
    }
    if (SEL_IS_RADIO(g_sel)) {
        radio_toggle(g_radio);
        return;
    }

    item_t *it = &items[g_sel];
    if (g_debug)
        logm("activate sel=%d kind=%d action=%d", g_sel, it->kind, it->action);
    if (it->kind == IT_SLIDER)
        return; /* sliders are changed directly with < > */
    if (it->kind == IT_TOGGLE) {
        adjust_value(0);
        return;
    }
    if (it->action == ACT_CLOSE) {
        do_close(EXIT_CLOSE);
        return;
    }
    if (it->action == ACT_KEYBOARD) {
        kb_open();
        return;
    }
    if (it->action == ACT_FPSHUD) {
        hud_sub_open();
        return;
    }
    if (it->action == ACT_EXIT_GAME && !g_has_game)
        return;
    if (g_cfg.confirm_exit)
        g_confirm = it->action;
    else if (it->action == ACT_EXIT_GAME)
        do_exit_game();
    else if (it->action == ACT_REBOOT)
        do_close(EXIT_REBOOT);
    else if (it->action == ACT_POWEROFF)
        do_close(EXIT_POWEROFF);
}

static void go_back(void)
{
    if (g_hud_sub) {
        g_hud_sub = 0;
        g_dirty = 1;
        return;
    }
    if (g_kb_open) {
        kb_close();
        return;
    }
    if (g_confirm) {
        g_confirm = ACT_NONE;
        return;
    }
    do_close(EXIT_CLOSE);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void on_term(int sig)
{
    (void)sig;
    FILE *f = fopen(STOPPED_FILE, "r");
    if (f) {
        int pid;
        while (fscanf(f, "%d", &pid) == 1)
            kill(pid, SIGCONT);
        fclose(f);
    }
    if (g_use_layer) {
        overlay_layer_hide(&g_layer);
        unlink(LAYER_FLAG);
    }
    _exit(99);
}

static int find_icon_font(char *out, size_t outsz)
{
    const char *env = getenv("OVERLAY_ICON_FONT");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return access(out, R_OK) == 0 ? 0 : -1;
    }
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, outsz, "%s/../resources/MaterialIcons-Regular.ttf", exe);
            if (access(out, R_OK) == 0)
                return 0;
        }
    }
    snprintf(out, outsz, "%s",
             "/mnt/SDCARD/Apps/OverLyder/resources/MaterialIcons-Regular.ttf");
    return access(out, R_OK) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    g_mode_cfg = (argc > 1 && !strcmp(argv[1], "--config"));

    manifest_load(&g_manifest);
    logm("manifest: version %s, updater %s/%s", g_manifest.version,
         g_manifest.owner, g_manifest.repo);
    cfg_load(&g_cfg);
    g_kb_mode = g_cfg.keyboard_mode;
    g_dim = g_cfg.dim;
    g_lang = clampi(g_cfg.lang, 0, NLANG - 1);
    lang_load(g_lang);
    items_set_language();
    if (g_mode_cfg)
        config_load();
    const char *hg = getenv("OVERLAY_HAS_GAME");
    g_has_game = hg && atoi(hg);
    g_debug = getenv("OVERLAY_DEBUG") != NULL;
    g_text_to_gamepad = getenv("OVERLAY_TEXT_TO_GAMEPAD") != NULL;

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_term);

    const char *nos = getenv("OVERLAY_NO_SIGSTOP");
    if (!g_mode_cfg && nos && atoi(nos)) {
        if (overlay_layer_open(&g_layer, SCR_W, SCR_H) == 0) {
            g_use_layer = 1;
            overlay_layer_show(&g_layer);
            FILE *flag = fopen(LAYER_FLAG, "w");
            if (flag) {
                fprintf(flag, "%d %u\n", (int)getpid(), g_layer.channel);
                fclose(flag);
            }
        } else {
            logm("no hardware layer available; falling back to framebuffer");
        }
    }

    if (overlay_fb_open() < 0 && !g_use_layer) {
        logm("cannot open framebuffer");
        return EXIT_FAILED;
    }
    if (!g_use_layer) {
        fb_capture(g_bg);
        prepare_background();
    }

    SDL_Init(0);
    if (TTF_Init() != 0) {
        logm("TTF_Init failed: %s", TTF_GetError());
        return EXIT_FAILED;
    }
    g_f_big = TTF_OpenFont(FONT_PATH, 54);
    g_f_mid = TTF_OpenFont(FONT_PATH, 30);
    g_f_small = TTF_OpenFont(FONT_PATH, 19);
    if (!g_f_big || !g_f_mid || !g_f_small) {
        logm("font load failed: %s", TTF_GetError());
        return EXIT_FAILED;
    }
    char iconfont[512];
    if (find_icon_font(iconfont, sizeof iconfont) == 0)
        g_f_icon = TTF_OpenFont(iconfont, 30);
    if (!g_f_icon)
        logm("icon font unavailable (%s)", iconfont);

    if (!g_mode_cfg)
        item_init_values();

    char dev[64];
    int evfd = -1;
    if (find_input_device(dev, sizeof dev) == 0)
        evfd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (evfd < 0) {
        logm("no input device (%s)", dev);
    } else {
        g_gamepad_fd = evfd;
        /* exclusive access while the overlay is up: the (paused) game must
         * not receive the buttons we use to navigate */
        if (ioctl(evfd, EVIOCGRAB, 1) < 0)
            logm("EVIOCGRAB failed: %s", strerror(errno));
        struct input_absinfo ai;
        if (ioctl(evfd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > 0)
            g_stick_max = ai.maximum;
        if (ioctl(evfd, EVIOCGABS(ABS_RX), &ai) == 0 && ai.maximum > 0)
            g_rstick_max = ai.maximum;
        logm("input device %s (left max=%d, right max=%d)", dev,
             g_stick_max, g_rstick_max);
    }
    logm("ui ready (has_game=%d, bg=%d)", g_has_game, g_bg_ok);

    long dir_t = 0, dir_rep = 0;
    int dir = 0;
    long diry_t = 0, diry_rep = 0;
    int dir_y = 0;
    long last_clock = 0;
    long last_refresh = 0;
    long last_bl_refresh = 0;
    long last_radio_refresh = 0;
    long last_kb_blink = 0;

    g_open_at = now_ms();
    g_anim_start = g_open_at;
    if (!g_mode_cfg) {
        /* assume the configured trigger keys are held while the overlay opens */
        for (int i = 0; i < g_cfg.trigger_count; i++)
            g_trigger_held |= 1 << (g_cfg.trigger[i] & 31);
        const char *ok = getenv("OVERLAY_OPEN_KEYBOARD");
        if (ok && atoi(ok)) {
            g_kb_only = 1;
            kb_open();  /* dedicated shortcut: open straight into keyboard */
        }
    }
    while (!g_quit) {
        if (evfd >= 0) {
            struct input_event ev[32];
            ssize_t rd = read(evfd, ev, sizeof ev);
            if (rd > 0) {
                int cnt = (int)(rd / (ssize_t)sizeof(struct input_event));
                for (int i = 0; i < cnt; i++) {
                    struct input_event *e = &ev[i];
                    if (g_debug)
                        logm("ev type=%d code=%d value=%d", e->type, e->code, e->value);
                    if (g_busy || g_closing)
                        continue;
                    /* keyboard mode owns all button presses: B/Home go back to
                     * the overlay, A types, the rest are keyboard shortcuts. */
                    if (g_kb_open && e->type == EV_KEY) {
                        if (e->value == 1)
                            kb_key(e->code);
                        g_dirty = 1;
                        continue;
                    }
                    /* brief grace after returning so a held B/Enter does not
                     * immediately navigate (or close) the overlay again */
                    if (!g_mode_cfg && now_ms() - g_kb_closed_at < 400)
                        continue;
                    if (e->type == EV_KEY && e->value == 0) {
                        g_trigger_held &= ~(1 << (e->code & 31));
                        g_trigger_released_at = now_ms();
                        continue;
                    }
                    if (e->type == EV_KEY && e->value == 1) {
                        int bit = 1 << (e->code & 31);
                        /* The key that opened the overlay is still physically
                         * held, or was just released after a long hold. Ignore
                         * it (and any autorepeat tail) for a short grace
                         * period, otherwise a long Home hold closes the UI. */
                        if (g_trigger_held & bit) {
                            /* ignored */
                        } else if (!g_mode_cfg &&
                                   now_ms() - g_trigger_released_at < 600) {
                            /* ignored */
                        } else if (!g_mode_cfg && now_ms() - g_open_at < 300) {
                            /* ignore events queued from the opening gesture */
                        } else if (g_mode_cfg && g_upd_state != UPD_NONE) {
                            if (g_upd_state == UPD_OPTIONS) {
                                if (e->code == K_A)
                                    upd_option_toggle();
                                if (g_upd_opt_sel == 1 || e->code == K_B ||
                                    e->code == K_MODE || e->code == K_START ||
                                    e->code == K_SELECT)
                                    upd_options_back();
                            } else if (g_upd_state == UPD_CHECKING ||
                                       g_upd_state == UPD_INSTALLING) {
                                /* network/install in progress: ignore input */
                            } else if (e->code == K_START || e->code == K_SELECT) {
                                upd_options_open();
                            } else if (e->code == K_B || e->code == K_MODE) {
                                if (g_upd_state == UPD_CONFIRM)
                                    g_upd_state = UPD_AVAILABLE; /* cancel install */
                                else
                                    g_upd_state = UPD_NONE;      /* back to settings */
                            } else if (e->code == K_A) {
                                if (g_upd_state == UPD_AVAILABLE)
                                    g_upd_state = UPD_CONFIRM;
                                else if (g_upd_state == UPD_CONFIRM)
                                    upd_install();
                                else
                                    g_upd_state = UPD_NONE;
                            }
                        } else if (g_mode_cfg && g_about) {
                            if (e->code == K_A || e->code == K_B || e->code == K_MODE ||
                                e->code == K_START || e->code == K_SELECT)
                                g_about = 0;
                        } else if (g_mode_cfg && g_hud_sub) {
                            if (e->code == K_A) {
                                hud_sub_activate();
                            } else if (e->code == K_B || e->code == K_MODE ||
                                       e->code == K_START || e->code == K_SELECT) {
                                g_hud_sub = 0;
                                g_dirty = 1;
                            }
                        } else if (g_mode_cfg && g_kb_learning) {
                            if (e->code == K_B) {
                                learn_finish_kb();
                            } else {
                                for (int k = 0; k < NKEYS; k++) {
                                    if (g_keys[k].code == e->code) {
                                        g_kb_learn_mask |= 1u << (e->code & 31);
                                        g_kb_learn_at = now_ms();
                                        break;
                                    }
                                }
                            }
                        } else if (g_mode_cfg && g_learning) {
                            if (e->code == K_B) {
                                learn_finish();
                            } else {
                                for (int k = 0; k < NKEYS; k++) {
                                    if (g_keys[k].code == e->code) {
                                        g_learn_mask |= 1 << (e->code & 31);
                                        g_learn_at = now_ms();
                                        break;
                                    }
                                }
                            }
                        } else if (g_mode_cfg) {
                            if (e->code == K_A)
                                config_activate();
                            else if (e->code == K_B || e->code == K_MODE ||
                                     e->code == K_START || e->code == K_SELECT)
                                do_close(EXIT_CLOSE);
                        } else {
                            switch (e->code) {
                            case K_A:
                                activate();
                                break;
                            case K_B:
                                go_back();
                                break;
                            case K_MODE:
                            case K_START:
                            case K_SELECT:
                                if (g_hud_sub) {
                                    g_hud_sub = 0;
                                    g_dirty = 1;
                                } else if (g_confirm) {
                                    g_confirm = ACT_NONE;
                                } else {
                                    do_close(EXIT_CLOSE);
                                }
                                break;
                            default:
                                break;
                            }
                        }
                        g_dirty = 1;
                    } else if (e->type == EV_ABS && e->code == ABS_HAT0X) {
                        if (e->value != 0) {
                            dir = e->value > 0 ? 1 : -1;
                            horiz_move(dir);
                            dir_t = now_ms();
                            dir_rep = dir_t;
                            g_dirty = 1;
                        } else {
                            dir = 0;
                        }
                    } else if (e->type == EV_ABS && e->code == ABS_HAT0Y) {
                        if (e->value != 0) {
                            int d = e->value > 0 ? 1 : -1;
                            vert_move(d);
                            dir_y = d;
                            diry_t = now_ms();
                            diry_rep = diry_t;
                            g_dirty = 1;
                        } else {
                            dir_y = 0;
                        }
                    } else if (e->type == EV_ABS && e->code == ABS_X) {
                        g_ax = e->value;
                        stick_eval();
                        g_dirty = 1;
                    } else if (e->type == EV_ABS && e->code == ABS_Y) {
                        g_ay = e->value;
                        stick_eval();
                        g_dirty = 1;
                    } else if (e->type == EV_ABS && e->code == ABS_RX) {
                        g_rx = e->value;
                        kb_move_panel();
                    } else if (e->type == EV_ABS && e->code == ABS_RY) {
                        g_ry = e->value;
                        kb_move_panel();
                    } else if (e->type == EV_ABS &&
                               (e->code == ABS_Z || e->code == ABS_RZ)) {
                        /* L2 / R2 (LT / RT) can be part of a mapped shortcut. */
                        int acode = (e->code == ABS_Z) ? K_L2 : K_R2;
                        if (e->value >= 128 && g_mode_cfg &&
                            (g_kb_learning || g_learning)) {
                            for (int k = 0; k < NKEYS; k++) {
                                if (g_keys[k].code == acode) {
                                    if (g_kb_learning)
                                        g_kb_learn_mask |= 1u << (acode & 31);
                                    else
                                        g_learn_mask |= 1u << (acode & 31);
                                    g_kb_learn_at = g_learn_at = now_ms();
                                    break;
                                }
                            }
                        }
                        g_dirty = 1;
                    }
                }
            }
        }

        if (dir && !g_closing) {
            long t = now_ms();
            if (t - dir_t > 380 && t - dir_rep > 110) {
                horiz_move(dir);
                dir_rep = t;
                g_dirty = 1;
            }
        }
        if (dir_y && !g_closing) {
            long t = now_ms();
            if (t - diry_t > 380 && t - diry_rep > 110) {
                vert_move(dir_y);
                diry_rep = t;
                g_dirty = 1;
            }
        }

        long now = now_ms();
        if (g_kb_open && now - last_kb_blink > 400) {
            last_kb_blink = now;
            g_dirty = 1; /* blink the text cursor */
        }
        if (g_mode_cfg && g_learning && g_learn_mask && now - g_learn_at > 2500) {
            learn_finish();
            g_dirty = 1;
        }
        if (g_mode_cfg && g_kb_learning && g_kb_learn_mask &&
            now - g_kb_learn_at > 2500) {
            learn_finish_kb();
            g_dirty = 1;
        }
        if (now - last_clock > 30000) {
            last_clock = now;
            g_dirty = 1;
        }
        if (!g_mode_cfg && !g_closing && !g_kb_open && now - last_refresh > 300) {
            last_refresh = now;
            item_refresh_volume();
        }
        if (!g_mode_cfg && !g_closing && !g_kb_open && now - last_bl_refresh > 1000) {
            last_bl_refresh = now;
            item_refresh_brightness();
        }
        if (!g_mode_cfg && !g_closing && !g_kb_open && now - last_radio_refresh > 2000) {
            int w = wifi_state_get(), b = bt_state_get();
            last_radio_refresh = now;
            if (w != g_wifi_on || b != g_bt_on) {
                g_wifi_on = w;
                g_bt_on = b;
                g_dirty = 1;
            }
        }
        if (!g_mode_cfg && (g_closing || now - g_anim_start < ANIM_MS))
            g_dirty = 1;
        if (g_closing && now - g_close_at >= ANIM_MS)
            g_quit = 1;

        if (g_dirty) {
            if (g_hud_sub)
                hud_sub_compose();
            else if (g_mode_cfg)
                config_compose();
            else
                compose();
            display_flip();
            g_dirty = 0;
        }

        /* tighter loop while the drawer is animating for smoother motion */
        usleep((!g_mode_cfg && (now - g_anim_start < ANIM_MS || g_closing)) ? 3000 : 6000);
    }

    kb_device_close();
    if (g_use_layer) {
        overlay_layer_hide(&g_layer);
        overlay_layer_close(&g_layer);
        unlink(LAYER_FLAG);
    } else if (!g_keep_overlay) {
        fb_restore();
    }
    if (evfd >= 0) {
        ioctl(evfd, EVIOCGRAB, 0);
        close(evfd);
    }
    if (g_f_big)
        TTF_CloseFont(g_f_big);
    if (g_f_mid)
        TTF_CloseFont(g_f_mid);
    if (g_f_small)
        TTF_CloseFont(g_f_small);
    if (g_f_icon)
        TTF_CloseFont(g_f_icon);
    TTF_Quit();
    SDL_Quit();
    if (g_led_color_changed)
        cfg_update_led_color(&g_cfg, g_cfg.led_color, 1);
    logm("ui exit code %d", g_exit_code);
    return g_exit_code;
}
