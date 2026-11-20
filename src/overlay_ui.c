#include "common.h"

#include <math.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <dirent.h>
#include <ctype.h>

#define SCR_W 1280
#define SCR_H 720
#define CANVAS_PX (SCR_W * SCR_H)

/* ------------------------------------------------------------------ */
/* items                                                              */
/* ------------------------------------------------------------------ */

enum { IT_SLIDER = 0, IT_ACTION = 1, IT_TOGGLE = 2 };
enum { ACT_NONE = 0, ACT_CLOSE, ACT_EXIT_GAME, ACT_REBOOT, ACT_POWEROFF };
enum {
    I_VOL = 0, I_BRI, I_CON, I_EXP, I_SAT, I_TMP,
    I_LEDBRI, I_LEDCOLOR, I_MUTE, I_PLAY, I_EXIT, I_REBOOT, I_POWER
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
#define GL_BATTERY 0xE1A4u  /* battery_full */

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
    { "Continuar",   IT_ACTION, ACT_CLOSE,     0,  0, GL_PLAY },
    { "Salir juego", IT_ACTION, ACT_EXIT_GAME, 0,  0, GL_EXIT },
    { "Reiniciar",   IT_ACTION, ACT_REBOOT,    0,  0, GL_REBOOT },
    { "Apagar",      IT_ACTION, ACT_POWEROFF,  0,  0, GL_POWER },
};

#define NITEMS ((int)(sizeof items / sizeof items[0]))

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

/* ------------------------------------------------------------------ */
/* configuration screen (CrossMix app)                                */
/* ------------------------------------------------------------------ */

enum {
    CFG_ESTADO = 0, CFG_GATILLO,
    CFG_K1, CFG_K2, CFG_K3, CFG_K4,
    CFG_UMBRAL, CFG_CONFIRMAR, CFG_IDIOMA,
    CFG_LEARN, CFG_RESET, CFG_ABOUT, CFG_UPDATE, CFG_LISTO, NCFG
};

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
};
#define NKEYS ((int)(sizeof g_keys / sizeof g_keys[0]))

static const int g_holds[] = { 400, 700, 1000, 1500 };
#define NHOLDS ((int)(sizeof g_holds / sizeof g_holds[0]))

static int g_cfg_sel = 0;
static int g_cfg_enabled = 1;
static int g_cfg_preset = 0;
static int g_cfg_hold = 1;
static int g_cfg_confirm = 1;
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
    UPD_INSTALLING, UPD_INSTALLED, UPD_ERROR
};
static int g_upd_state = UPD_NONE;
static char g_upd_latest[32] = "";

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

enum {
    S_AJUSTES, S_HINT_CLOSE,
    S_VOL, S_BRI, S_CON, S_EXP, S_SAT, S_TMP,
    S_PLAY, S_EXIT, S_REBOOT, S_POWER,
    S_ACCEPT, S_DISABLED_TXT, S_HINT_SLIDER, S_HINT_ACTION,
    S_CONFIRM_EXIT, S_CONFIRM_REBOOT, S_CONFIRM_POWER, S_CONFIRM_KEYS, S_BUSY,
    S_CFG_SUBTITLE, S_CFG_ESTADO, S_CFG_GATILLO, S_CFG_UMBRAL,
    S_CFG_CONFIRMAR, S_CFG_LISTO, S_CFG_IDIOMA, S_KEYFMT,
    S_ENABLED, S_DISABLED_VAL, S_SI, S_NO, S_NONE,
    S_PRESET_HOLD_HOME, S_PRESET_HOME_SELECT, S_PRESET_HOME_START, S_PRESET_L1R1,
    S_PRESET_HOLD_SELECT, S_PRESET_HOLD_START, S_PRESET_CUSTOM,
    S_CFG_FOOTER,
    S_LED, S_LED_BRI, S_LED_COLOR, S_MUTE, S_ON, S_OFF,
    S_LEARN, S_RESET, S_ABOUT, S_LEARN_TITLE, S_ABOUT_TEXT, S_ABOUT_HINT,
    S_UPDATE, S_UPDATE_TITLE, S_UPDATE_LOCAL, S_UPDATE_LATEST,
    S_UPDATE_CHECKING, S_UPDATE_UPTODATE, S_UPDATE_AVAILABLE, S_UPDATE_ERROR,
    S_UPDATE_CONFIRM, S_UPDATE_INSTALLING, S_UPDATE_INSTALLED, S_UPDATE_BACK,
    S_COUNT
};

static const char *const g_tr[NLANG][S_COUNT] = {
    { /* Spanish */
        "AJUSTES", "HOME / B: cerrar",
        "Volumen", "Brillo", "Contraste", "Exposición", "Saturación", "Temperatura",
        "Continuar", "Salir juego", "Reiniciar", "Apagar",
        "A: Aceptar", "no disponible",
        "^ v: Elegir   < >: Ajustar   B: Cerrar",
        "^ v / < >: Elegir   A: Aceptar   B: Cerrar",
        "Salir del juego?", "Reiniciar el dispositivo?", "Apagar el dispositivo?",
        "A: Si      B: No", "Saliendo del juego...",
        "Ajustes del overlay", "Estado", "Gatillo", "Umbral (pulsacion larga)",
        "Confirmar salida", "Listo", "Idioma", "Tecla %d",
        "Activado", "Desactivado", "Si", "No", "Ninguna",
        "Mantener Home", "Home + Select", "Home + Start", "L1 + R1",
        "Mantener Select", "Mantener Start", "Personalizado",
        "Arriba/Abajo o < >: Elegir    A: Cambiar    B: Salir (guardado automatico)",
        "LED", "Brillo LED", "Color LED", "Silenciar", "Encendido", "Apagado",
        "Aprender", "Restablecer", "Acerca de",
        "Pulsa los botones (B: listo)", "OverLyder v0.0.2 - Material Icons (Apache 2.0)",
        "A/B: volver",
        "Actualizar", "Actualizador", "Versión instalada", "Última versión",
        "Consultando GitHub...", "Estás al día", "Hay una versión nueva",
        "No se pudo consultar", "A: Instalar   B: Cancelar", "Instalando...",
        "Instalado. Reinicia la app", "B: Volver",
    },
    { /* English */
        "SETTINGS", "HOME / B: close",
        "Volume", "Brightness", "Contrast", "Exposure", "Saturation", "Temperature",
        "Continue", "Exit game", "Reboot", "Power off",
        "A: Accept", "unavailable",
        "^ v: Select   < >: Adjust   B: Close",
        "^ v / < >: Select   A: Accept   B: Close",
        "Exit game?", "Restart the device?", "Power off the device?",
        "A: Yes      B: No", "Exiting game...",
        "Overlay settings", "Status", "Trigger", "Threshold (long press)",
        "Confirm exit", "Done", "Language", "Key %d",
        "Enabled", "Disabled", "Yes", "No", "None",
        "Hold Home", "Home + Select", "Home + Start", "L1 + R1",
        "Hold Select", "Hold Start", "Custom",
        "Up/Down or < >: Select    A: Change    B: Exit (auto-saved)",
        "LED", "LED brightness", "LED color", "Mute", "On", "Off",
        "Learn", "Reset", "About",
        "Press buttons (B: done)", "OverLyder v0.0.2 - Material Icons (Apache 2.0)",
        "A/B: back",
        "Update", "Updater", "Installed version", "Latest version",
        "Checking GitHub...", "You are up to date", "A new version is available",
        "Could not check", "A: Install   B: Cancel", "Installing...",
        "Installed. Restart the app", "B: Back",
    },
    { /* Portuguese */
        "AJUSTES", "HOME / B: fechar",
        "Volume", "Brilho", "Contraste", "Exposição", "Saturação", "Temperatura",
        "Continuar", "Sair do jogo", "Reiniciar", "Desligar",
        "A: Aceitar", "indisponível",
        "^ v: Escolher   < >: Ajustar   B: Fechar",
        "^ v / < >: Escolher   A: Aceitar   B: Fechar",
        "Sair do jogo?", "Reiniciar o dispositivo?", "Desligar o dispositivo?",
        "A: Sim      B: Não", "Saindo do jogo...",
        "Ajustes do overlay", "Estado", "Gatilho", "Limiar (pressão longa)",
        "Confirmar saída", "Concluído", "Idioma", "Tecla %d",
        "Ativado", "Desativado", "Sim", "Não", "Nenhuma",
        "Manter Home", "Home + Select", "Home + Start", "L1 + R1",
        "Manter Select", "Manter Start", "Personalizado",
        "Cima/Baixo ou < >: Escolher    A: Alterar    B: Sair (salvamento automático)",
        "LED", "LED brilho", "Cor do LED", "Silenciar", "Ligado", "Desligado",
        "Aprender", "Redefinir", "Sobre",
        "Pressione os botões (B: pronto)", "OverLyder v0.0.2 - Material Icons (Apache 2.0)",
        "A/B: voltar",
        "Atualizar", "Atualizador", "Versão instalada", "Última versão",
        "Consultando o GitHub...", "Você está em dia", "Há uma versão nova",
        "Não foi possível verificar", "A: Instalar   B: Cancelar", "Instalando...",
        "Instalado. Reinicie o app", "B: Voltar",
    },
};

#define TR(id) (g_tr[g_lang][(id)])
#define LANG_NAME(l) (l == LANG_EN ? "English" : (l == LANG_PT ? "Português" : "Español"))

static void items_set_language(void)
{
    static const int ids[NITEMS] = {
        S_VOL, S_BRI, S_CON, S_EXP, S_SAT, S_TMP,
        S_LED_BRI, S_LED_COLOR, S_MUTE,
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
#define ROW_H        37
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

static void compose(void)
{
    memcpy(g_cv, g_bg_dim, sizeof g_cv);

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
                    draw_color_slider(px + 76, y + 27, PANEL_W - 104, it->value, focused);
                else
                    draw_row_slider(px + 76, y + 27, PANEL_W - 104, it->value, focused);
            }
        } else {
            draw_text(disabled ? "-" : (focused ? TR(S_ACCEPT) : ">"),
                      px + PANEL_W - 28, y + 2, g_f_small,
                      focused ? 0xE62037 : 0x7E808C, 2);
        }
    }

    fill_round_a(px + 20, PANEL_TOP + PANEL_H - 46, PANEL_W - 40, 34, 10, 0x1A1B22, 255);
    if (items[g_sel].kind == IT_SLIDER || items[g_sel].kind == IT_TOGGLE)
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
        fprintf(f, "lang=%s\n", g_lang == LANG_EN ? "en" : (g_lang == LANG_PT ? "pt" : "es"));
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
        run_cmd("/mnt/SDCARD/Apps/OverLyder/overlayctl start >/dev/null 2>&1");
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
    case CFG_IDIOMA:    g_lang = (g_lang + 1) % NLANG; break;
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
        g_dim = g_cfg.dim;
        cfg_update_led_color(&g_cfg, 0xFFFFFF, 0);
        led_color_apply(0xFFFFFF);
        break;
    case CFG_ABOUT:
        g_about = 1;
        return;
    case CFG_UPDATE:
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
    values[CFG_LEARN][0] = '\0';
    values[CFG_RESET][0] = '\0';
    values[CFG_ABOUT][0] = '\0';
    snprintf(values[CFG_UPDATE], sizeof values[0], "v%s", g_manifest.version);
    values[CFG_LISTO][0] = '\0';

    int y0 = 150, row = 0;
    for (int i = 0; i < NCFG; i++) {
        if (!cfg_visible(i))
            continue;
        int y = y0 + row * 40;
        row++;
        int focused = (i == g_cfg_sel);
        fill_round_a(220, y, SCR_W - 440, 36, 10,
                     focused ? 0xE62037 : 0x1A1B22, focused ? 255 : 235);
        draw_text(labels[i], 260, y + 4, g_f_mid, focused ? 0xFFFFFF : 0xBCBEC8, 0);
        draw_text(values[i], SCR_W - 260, y + 4, g_f_mid, focused ? 0xFFFFFF : 0xF0F2F8, 2);
    }

    draw_text(TR(S_CFG_FOOTER), SCR_W / 2, 684, g_f_small, 0x9698A2, 1);
}

/* ------------------------------------------------------------------ */
/* self-updater (CrossMix app only - never in the in-game overlay)      */
/* ------------------------------------------------------------------ */

static void update_compose(void)
{
    draw_text(TR(S_UPDATE_TITLE), SCR_W / 2, 130, g_f_mid, 0xE62037, 1);

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
    case UPD_CONFIRM:   msg = TR(S_UPDATE_AVAILABLE); break;
    case UPD_INSTALLED: msg = TR(S_UPDATE_INSTALLED); break;
    case UPD_ERROR:     msg = TR(S_UPDATE_ERROR); color = 0xE62037; break;
    default: break;
    }
    if (msg)
        draw_text(msg, SCR_W / 2, 380, g_f_mid, color, 1);

    if (g_upd_state == UPD_AVAILABLE)
        draw_text(TR(S_UPDATE_CONFIRM), SCR_W / 2, 470, g_f_small, 0x9698A2, 1);
    draw_text(TR(S_UPDATE_BACK), SCR_W / 2, 620, g_f_small, 0x9698A2, 1);
}

/* true when version a is newer than b ("0.0.2" or "v0.0.2" both accepted). */
static int ver_newer(const char *a, const char *b)
{
    int am = 0, an = 0, ap = 0, bm = 0, bn = 0, bp = 0;
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    sscanf(a, "%d.%d.%d", &am, &an, &ap);
    sscanf(b, "%d.%d.%d", &bm, &bn, &bp);
    if (am != bm) return am > bm;
    if (an != bn) return an > bn;
    return ap > bp;
}

/* updater.sh prints "OK <tag>" or "ERR <code>" on a single line. */
static void upd_run(const char *op)
{
    char out[256] = "";
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "GITHUB_OWNER=%s GITHUB_REPO=%s sh " APP_DIR "/%s %s %s 2>/dev/null",
             g_manifest.owner, g_manifest.repo, g_manifest.updater,
             op, g_manifest.version);
    if (read_cmd(out, sizeof out, "%s", cmd) != 0) {
        g_upd_state = UPD_ERROR;
        return;
    }
    char res[16] = "", arg[64] = "";
    if (sscanf(out, "%15s %63s", res, arg) < 1 || strcmp(res, "OK")) {
        g_upd_state = UPD_ERROR;
        return;
    }
    if (!strcmp(op, "check")) {
        snprintf(g_upd_latest, sizeof g_upd_latest, "%s", arg);
        g_upd_state = ver_newer(g_upd_latest, g_manifest.version) ? UPD_AVAILABLE
                                                           : UPD_UPTODATE;
    } else {
        g_upd_state = UPD_INSTALLED;
    }
}

static void upd_check(void)
{
    g_upd_state = UPD_CHECKING;
    g_upd_latest[0] = '\0';
    g_dirty = 1;
    config_compose();
    fb_flip();
    upd_run("check");
    g_dirty = 1;
}

static void upd_install(void)
{
    g_upd_state = UPD_INSTALLING;
    g_dirty = 1;
    config_compose();
    fb_flip();
    upd_run("install");
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
    FILE *f = fopen(STOPPED_FILE, "r");
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
    fb_flip();

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

static void move_sel(int d) { g_sel = (g_sel + d + NITEMS) % NITEMS; }

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
    if (g_mode_cfg)
        cfg_move(dir);
    else if (!g_confirm) {
        if (items[g_sel].kind == IT_SLIDER || items[g_sel].kind == IT_TOGGLE)
            adjust_value(dir);
        else
            move_sel(dir);
    }
}

static void vert_move(int dir)
{
    if (g_mode_cfg && (g_learning || g_about))
        return;
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
    item_t *it = &items[g_sel];

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
    g_dim = g_cfg.dim;
    g_lang = clampi(g_cfg.lang, 0, NLANG - 1);
    items_set_language();
    if (g_mode_cfg)
        config_load();
    const char *hg = getenv("OVERLAY_HAS_GAME");
    g_has_game = hg && atoi(hg);
    g_debug = getenv("OVERLAY_DEBUG") != NULL;

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_term);

    if (overlay_fb_open() < 0) {
        logm("cannot open framebuffer");
        return EXIT_FAILED;
    }
    fb_capture(g_bg);
    prepare_background();

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
        /* exclusive access while the overlay is up: the (paused) game must
         * not receive the buttons we use to navigate */
        if (ioctl(evfd, EVIOCGRAB, 1) < 0)
            logm("EVIOCGRAB failed: %s", strerror(errno));
        struct input_absinfo ai;
        if (ioctl(evfd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > 0)
            g_stick_max = ai.maximum;
        logm("input device %s (stick max=%d)", dev, g_stick_max);
    }
    logm("ui ready (has_game=%d, bg=%d)", g_has_game, g_bg_ok);

    long dir_t = 0, dir_rep = 0;
    int dir = 0;
    long diry_t = 0, diry_rep = 0;
    int dir_y = 0;
    long last_clock = 0;
    long last_refresh = 0;
    long last_bl_refresh = 0;

    g_open_at = now_ms();
    g_anim_start = g_open_at;
    if (!g_mode_cfg) {
        /* assume the configured trigger keys are held while the overlay opens */
        for (int i = 0; i < g_cfg.trigger_count; i++)
            g_trigger_held |= 1 << (g_cfg.trigger[i] & 31);
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
                            if (g_upd_state == UPD_CHECKING ||
                                g_upd_state == UPD_INSTALLING) {
                                /* network/install in progress: ignore input */
                            } else if (e->code == K_B || e->code == K_MODE ||
                                       e->code == K_START || e->code == K_SELECT) {
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
                                if (g_confirm)
                                    g_confirm = ACT_NONE;
                                else
                                    do_close(EXIT_CLOSE);
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
        if (g_mode_cfg && g_learning && g_learn_mask && now - g_learn_at > 2500) {
            learn_finish();
            g_dirty = 1;
        }
        if (now - last_clock > 30000) {
            last_clock = now;
            g_dirty = 1;
        }
        if (!g_mode_cfg && !g_closing && now - last_refresh > 300) {
            last_refresh = now;
            item_refresh_volume();
        }
        if (!g_mode_cfg && !g_closing && now - last_bl_refresh > 1000) {
            last_bl_refresh = now;
            item_refresh_brightness();
        }
        if (!g_mode_cfg && (g_closing || now - g_anim_start < ANIM_MS))
            g_dirty = 1;
        if (g_closing && now - g_close_at >= ANIM_MS)
            g_quit = 1;

        if (g_dirty) {
            if (g_mode_cfg)
                config_compose();
            else
                compose();
            fb_flip();
            g_dirty = 0;
        }

        /* tighter loop while the drawer is animating for smoother motion */
        usleep((!g_mode_cfg && (now - g_anim_start < ANIM_MS || g_closing)) ? 3000 : 6000);
    }

    if (!g_keep_overlay)
        fb_restore();
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
