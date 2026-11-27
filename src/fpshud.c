/* fpshud - persistent, read-only performance overlay for OverLyder.
 *
 * The settings panel only exists while the hotkey is held. This small resident
 * process keeps a HUD on a second hardware display channel so the numbers are
 * visible during normal play. It never changes clocks, game state or any
 * system setting: everything here is a read from procfs/sysfs plus a count of
 * /dev/fb0 page flips (the way emulators on this device present frames).
 *
 * See docs/FPS_HUD.md for the measurements and limitations. */
#include "common.h"
#include "overlay_layer.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <linux/fb.h>
#include <math.h>

#define SCR_W 1280
#define SCR_H 720
#define HUD_STEP_MS 250
#define FLIP_POLL_US 2000

static volatile sig_atomic_t g_run = 1;
static overlay_cfg g_cfg;
static overlay_layer g_layer;
static uint32_t g_cv[SCR_W * SCR_H];
static TTF_Font *g_font, *g_small;
static int g_fb_fd = -1;
static int g_layer_ok = 0;

/* statistics published on screen and in HUD_STATUS_FILE for testing */
static double g_fps = -1, g_frame_ms = -1;
static double g_cpu = -1, g_gpu = -1;
static int g_cpu_temp = -1, g_gpu_temp = -1, g_ddr_temp = -1;
static int g_cpu_mhz = -1, g_gpu_mhz = -1, g_batt = -1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
}

/* ---------------------------------------------------------------- */
/* telemetry                                                         */
/* ---------------------------------------------------------------- */

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/* CPU busy percentage from the aggregate /proc/stat line. */
static double cpu_usage(void)
{
    static unsigned long long prev_total, prev_idle;
    unsigned long long user, nice, sys, idle, iowait, irq, soft, steal;
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &soft, &steal);
    fclose(f);
    if (n < 8)
        return -1;
    unsigned long long total = user + nice + sys + idle + iowait + irq + soft + steal;
    unsigned long long idled = idle + iowait;
    double pct = -1;
    if (prev_total && total > prev_total) {
        unsigned long long dt = total - prev_total;
        unsigned long long di = idled > prev_idle ? idled - prev_idle : 0;
        pct = 100.0 * (double)(dt - di) / (double)dt;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    prev_total = total;
    prev_idle = idled;
    return pct;
}

/* PowerVR's debugfs status exposes a real GPU utilisation counter:
 *   GPU Utilisation: 3%
 * The old approach (sampling "idle:on/off" in sunxi_gpu/dump) always read 0
 * on this firmware because that flag tracks the power domain, not the GPU.
 * The clock still comes from sunxi_gpu/dump. */
static void gpu_sample(double *busy_pct, int *mhz)
{
    char status[1024] = "";
    FILE *f = fopen("/sys/kernel/debug/pvr/status", "r");
    if (f) {
        size_t n = fread(status, 1, sizeof status - 1, f);
        status[n] = '\0';
        fclose(f);
    }
    char *p = strstr(status, "GPU Utilisation:");
    if (p) {
        int pct = -1;
        if (sscanf(p, "GPU Utilisation: %d%%", &pct) == 1 && pct >= 0) {
            *busy_pct = pct;
        }
    }

    char buf[512] = "";
    f = fopen("/sys/kernel/debug/sunxi_gpu/dump", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    if (strstr(buf, "Frequency:")) {
        int m = -1;
        if (sscanf(strstr(buf, "Frequency:"), "Frequency:%dMHz", &m) == 1)
            *mhz = m;
    }
}

/* Count live page flips. The UI/game layers on this firmware present by
 * panning /dev/fb0, so a yoffset change is one presented frame. */
static void sample_flips(double *fps, double *frame_ms)
{
    static unsigned prev_offset;
    static int seeded;
    static double ema_ms = -1;
    int flips = 0;
    long long start = now_us(), last_flip = 0;

    struct fb_var_screeninfo var;
    while (now_us() - start < HUD_STEP_MS * 1000LL) {
        if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &var) != 0)
            break;
        if (!seeded) {
            prev_offset = var.yoffset;
            last_flip = now_us();
            seeded = 1;
        } else if (var.yoffset != prev_offset) {
            long long t = now_us();
            if (last_flip) {
                double dt = (double)(t - last_flip) / 1000.0;
                if (dt > 0 && dt < 500) {
                    ema_ms = ema_ms < 0 ? dt : ema_ms * 0.8 + dt * 0.2;
                }
            }
            last_flip = t;
            prev_offset = var.yoffset;
            flips++;
        }
        usleep(FLIP_POLL_US);
    }
    /* Only divide by the time spent polling. Rendering the HUD and reading
     * sysfs happens after this function and would otherwise dilute the rate. */
    long long elapsed = now_us() - start;
    if (elapsed > 0) {
        *fps = (double)flips * 1000000.0 / (double)elapsed;
        *frame_ms = ema_ms;
    }
}

/* ---------------------------------------------------------------- */
/* drawing                                                           */
/* ---------------------------------------------------------------- */

static void px_blend(int x, int y, uint32_t rgb, int a)
{
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H || a <= 0)
        return;
    uint32_t *d = &g_cv[y * SCR_W + x];
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
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb, int a)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            px_blend(xx, yy, rgb, a);
}

static void draw_text(const char *txt, int x, int y, TTF_Font *f,
                      uint32_t rgb, int right)
{
    if (!f || !txt || !*txt)
        return;
    SDL_Color c = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(f, txt, c);
    if (!s)
        return;
    int dx = right ? x - s->w : x;
    SDL_LockSurface(s);
    for (int yy = 0; yy < s->h; yy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)yy * s->pitch);
        for (int xx = 0; xx < s->w; xx++) {
            int a = (int)((row[xx] >> 24) & 0xFF);
            if (a)
                px_blend(dx + xx, y + yy, row[xx] & 0xFFFFFF, a);
        }
    }
    SDL_UnlockSurface(s);
    SDL_FreeSurface(s);
}

static void rotate_to_layer(void)
{
    if (!g_layer_ok)
        return;
    if (g_layer.width == SCR_W && g_layer.height == SCR_H) {
        memcpy(g_layer.pixels, g_cv, sizeof g_cv);
        return;
    }
    if (g_layer.width != SCR_H || g_layer.height != SCR_W) {
        logm("unexpected HUD layer geometry %dx%d", g_layer.width, g_layer.height);
        return;
    }
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int dx = y;
            int dy = SCR_W - 1 - x;
            g_layer.pixels[(size_t)dy * (size_t)g_layer.width + (size_t)dx] =
                g_cv[(size_t)y * SCR_W + (size_t)x];
        }
    }
}

static int text_width(const char *s)
{
    int w = 0, h = 0;
    if (TTF_SizeUTF8(g_font, s, &w, &h) != 0)
        return 0;
    return w;
}

/* "Simple" style: short tokens side by side, no window over the game. A one
 * pixel black shadow keeps them readable on bright scenes. */
static void compose_simple(void)
{
    char tok[HUD_F_COUNT][48], lines[3][256];
    int nt = 0;
    if (g_cfg.fps_hud_fields & HUD_F_FPS) {
        if (g_fps >= 0)
            snprintf(tok[nt], sizeof tok[0], "FPS %.0f", g_fps);
        else
            snprintf(tok[nt], sizeof tok[0], "FPS --");
        nt++;
    }
    if (g_cfg.fps_hud_fields & HUD_F_FRAME)
        snprintf(tok[nt++], sizeof tok[0], g_frame_ms >= 0 ? "%.1fms" : "--ms", g_frame_ms);
    if (g_cfg.fps_hud_fields & HUD_F_CPU)
        snprintf(tok[nt++], sizeof tok[0], g_cpu >= 0 ? "CPU %.0f%%" : "CPU --", g_cpu);
    if (g_cfg.fps_hud_fields & HUD_F_GPU)
        snprintf(tok[nt++], sizeof tok[0], g_gpu >= 0 ? "GPU %.0f%%" : "GPU --", g_gpu);
    if (g_cfg.fps_hud_fields & HUD_F_TEMP)
        snprintf(tok[nt++], sizeof tok[0], "TMP %dC", g_cpu_temp);
    if (g_cfg.fps_hud_fields & HUD_F_CPUFREQ)
        snprintf(tok[nt++], sizeof tok[0], "CFRQ %dM", g_cpu_mhz);
    if (g_cfg.fps_hud_fields & HUD_F_GPUFREQ)
        snprintf(tok[nt++], sizeof tok[0], "GFX %dM", g_gpu_mhz);
    if (g_cfg.fps_hud_fields & HUD_F_BATT)
        snprintf(tok[nt++], sizeof tok[0], "BAT %d%%", g_batt);

    int nlines = 0;
    lines[0][0] = '\0';
    for (int i = 0; i < nt; i++) {
        char candidate[256];
        snprintf(candidate, sizeof candidate, "%s%s%s", lines[nlines],
                 lines[nlines][0] ? "  " : "", tok[i]);
        if (text_width(candidate) > SCR_W - 32 && lines[nlines][0] && nlines < 2) {
            nlines++;
            snprintf(lines[nlines], sizeof lines[nlines], "%s", tok[i]);
        } else {
            snprintf(lines[nlines], sizeof lines[nlines], "%s", candidate);
        }
    }
    int nl = nlines + 1;
    int lh = 27, maxw = 0;
    for (int i = 0; i < nl; i++) {
        int w = text_width(lines[i]);
        if (w > maxw) maxw = w;
    }
    int x = (g_cfg.fps_hud_pos == 1 || g_cfg.fps_hud_pos == 3) ? SCR_W - 16 - maxw : 16;
    int y = (g_cfg.fps_hud_pos == 2 || g_cfg.fps_hud_pos == 3) ? SCR_H - 16 - nl * lh : 16;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    for (int i = 0; i < nl; i++) {
        draw_text(lines[i], x + 1, y + i * lh + 1, g_font, 0x000000, 0);
        draw_text(lines[i], x, y + i * lh, g_font, 0xF0F2F8, 0);
    }
}

static void compose_window(void)
{
    char rows[HUD_F_COUNT][64];
    int n = 0;
    if (g_cfg.fps_hud_fields & HUD_F_FPS)
        snprintf(rows[n++], sizeof rows[0], g_fps >= 0 ? "FPS     %5.1f" : "FPS        --", g_fps);
    if (g_cfg.fps_hud_fields & HUD_F_FRAME)
        snprintf(rows[n++], sizeof rows[0], g_frame_ms >= 0 ? "FRAME  %5.1f ms" : "FRAME       -- ms", g_frame_ms);
    if (g_cfg.fps_hud_fields & HUD_F_CPU)
        snprintf(rows[n++], sizeof rows[0], g_cpu >= 0 ? "CPU     %3.0f %%" : "CPU         -- %%", g_cpu);
    if (g_cfg.fps_hud_fields & HUD_F_GPU)
        snprintf(rows[n++], sizeof rows[0], g_gpu >= 0 ? "GPU     %3.0f %%" : "GPU         -- %%", g_gpu);
    if (g_cfg.fps_hud_fields & HUD_F_TEMP)
        snprintf(rows[n++], sizeof rows[0], "TEMP    %3d C", g_cpu_temp);
    if (g_cfg.fps_hud_fields & HUD_F_CPUFREQ)
        snprintf(rows[n++], sizeof rows[0], "CPUFRQ  %4d MHz", g_cpu_mhz);
    if (g_cfg.fps_hud_fields & HUD_F_GPUFREQ)
        snprintf(rows[n++], sizeof rows[0], "GPUFRQ  %4d MHz", g_gpu_mhz);
    if (g_cfg.fps_hud_fields & HUD_F_BATT)
        snprintf(rows[n++], sizeof rows[0], "BATT    %3d %%", g_batt);

    int box_w = 252, line_h = 28, box_h = n * line_h + 48;
    if (n == 0)
        box_h = 48;
    int margin = 24;
    int x = (g_cfg.fps_hud_pos == 1 || g_cfg.fps_hud_pos == 3) ? SCR_W - margin - box_w : margin;
    int y = (g_cfg.fps_hud_pos == 2 || g_cfg.fps_hud_pos == 3) ? SCR_H - margin - box_h : margin;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fill_rect(x, y, box_w, box_h, 0x0A0B12, 205);
    fill_rect(x, y, 4, box_h, 0xE62037, 255);
    draw_text("OVERLYDER  PERF", x + 16, y + 10, g_small, 0xE62037, 0);
    for (int i = 0; i < n; i++)
        draw_text(rows[i], x + 16, y + 34 + i * line_h, g_font, 0xF0F2F8, 0);
}

static void compose(void)
{
    memset(g_cv, 0, sizeof g_cv);
    if (g_cfg.fps_hud_style == HUD_STYLE_SIMPLE)
        compose_simple();
    else
        compose_window();

    /* While the settings panel is up it owns a display channel and covers the
     * screen; hiding the HUD avoids two apps writing layers at the same time. */
    if (access(OVERLAY_OPEN_FLAG, F_OK) == 0 || access(LAYER_FLAG, F_OK) == 0)
        overlay_layer_hide(&g_layer);
    else
        overlay_layer_show(&g_layer);
    rotate_to_layer();

    FILE *st = fopen(HUD_STATUS_FILE, "w");
    if (st) {
        fprintf(st, "pid=%d fps=%.2f frame=%.2f cpu=%.1f gpu=%.1f "
                    "cputemp=%d gputemp=%d ddrtemp=%d cpumhz=%d gpumhz=%d batt=%d "
                    "style=%d pos=%d\n",
                (int)getpid(), g_fps, g_frame_ms, g_cpu, g_gpu, g_cpu_temp,
                g_gpu_temp, g_ddr_temp, g_cpu_mhz, g_gpu_mhz, g_batt,
                g_cfg.fps_hud_style, g_cfg.fps_hud_pos);
        fclose(st);
    }
}

/* ---------------------------------------------------------------- */

int main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    cfg_load(&g_cfg);
    if (!g_cfg.fps_hud)
        return 0;

    g_fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0) {
        logm("fpshud: cannot open /dev/fb0: %s", strerror(errno));
        return EXIT_FAILED;
    }
    /* Prefer channel 2 / page 14. The settings panel uses 3 / 15. */
    if (overlay_layer_open_ex(&g_layer, SCR_W, SCR_H, 0, 4) != 0) {
        logm("fpshud: no free hardware layer, HUD disabled");
        close(g_fb_fd);
        return EXIT_FAILED;
    }
    g_layer_ok = 1;
    overlay_layer_show(&g_layer);
    {
        FILE *f = fopen(HUD_LAYER_FLAG, "w");
        if (f) {
            fprintf(f, "%d %u\n", (int)getpid(), g_layer.channel);
            fclose(f);
        }
    }

    SDL_Init(0);
    if (TTF_Init() != 0) {
        logm("fpshud: TTF_Init failed: %s", TTF_GetError());
        goto fail;
    }
    g_font = TTF_OpenFont(FONT_PATH, 23);
    g_small = TTF_OpenFont(FONT_PATH, 16);
    if (!g_font || !g_small) {
        logm("fpshud: font load failed: %s", TTF_GetError());
        goto fail;
    }

    {
        FILE *f = fopen(HUD_PID_FILE, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
    logm("fpshud started (ch=%u page=%d, pos=%d, fields=0x%x)",
         g_layer.channel, g_layer.page, g_cfg.fps_hud_pos, g_cfg.fps_hud_fields);

    while (g_run) {
        /* The in-game submenu writes overlay.cfg and a sync flag; re-read it
         * here so style/position/field changes show up without a restart. */
        cfg_load(&g_cfg);
        if (!g_cfg.fps_hud)
            break;
        sample_flips(&g_fps, &g_frame_ms);
        double cpu = cpu_usage();
        if (cpu >= 0)
            g_cpu = cpu;
        gpu_sample(&g_gpu, &g_gpu_mhz);
        int t = read_int_file("/sys/class/thermal/thermal_zone0/temp", -1);
        if (t > 0) g_cpu_temp = t / 1000;
        t = read_int_file("/sys/class/thermal/thermal_zone1/temp", -1);
        if (t > 0) g_gpu_temp = t / 1000;
        t = read_int_file("/sys/class/thermal/thermal_zone2/temp", -1);
        if (t > 0) g_ddr_temp = t / 1000;
        t = read_int_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", -1);
        if (t > 0) g_cpu_mhz = t / 1000;
        t = read_int_file("/sys/class/power_supply/axp2202-battery/capacity", -1);
        if (t >= 0) g_batt = t;
        compose();
    }

    logm("fpshud exiting");
fail:
    if (g_layer_ok)
        overlay_layer_close(&g_layer);
    unlink(HUD_PID_FILE);
    unlink(HUD_STATUS_FILE);
    unlink(HUD_LAYER_FLAG);
    if (g_font) TTF_CloseFont(g_font);
    if (g_small) TTF_CloseFont(g_small);
    TTF_Quit();
    SDL_Quit();
    if (g_fb_fd >= 0) close(g_fb_fd);
    return 0;
}
