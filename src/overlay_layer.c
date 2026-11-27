#include "overlay_layer.h"

#include <linux/fb.h>
#include <sys/mman.h>

/* Allwinner display2 (BSP 4.9) ioctls and structs. The layout below matches
 * the stock trimui_osdd binary and brick-tube's rectfix shim. */
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

#define LAYER_MODE_BUFFER     0
#define DISP_FORMAT_ARGB8888  0

typedef struct { int x, y; unsigned int width, height; } disp_rect;
typedef struct { long long x, y, width, height; } disp_rect64;
typedef struct { unsigned int width, height; } disp_rectsz;

typedef struct {
    unsigned long long addr[3];
    disp_rectsz size[3];
    unsigned int align[3];
    int format;
    int color_space;
    unsigned int trd_right_addr[3];
    bool pre_multiply;
    disp_rect64 crop;
    int flags;
    int scan;
} disp_fb_info;

typedef struct {
    int mode;
    unsigned char zorder, alpha_mode, alpha_value;
    disp_rect screen_win;
    bool b_trd_out;
    int out_trd_mode;
    union {
        unsigned int color;
        disp_fb_info fb;
    } u;
    unsigned int id;
} disp_layer_info;

typedef struct {
    disp_layer_info info;
    bool enable;
    unsigned int channel;
    unsigned int layer_id;
} disp_layer_config;

#define DISP_DEV "/dev/disp"
#define FB_DEV   "/dev/fb0"

/* Channel 1 is the UI/fb0 layer (z=16); channel 0 is video. Stock trimui_osdd
 * uses channels 2 and 3 when it runs, so prefer the higher ones. */
static const unsigned int g_channels[] = { 3, 2 };
#define NCHANNELS ((int)(sizeof g_channels / sizeof g_channels[0]))

static int disp_get_config(int fd, unsigned channel, unsigned layer_id,
                           disp_layer_config *cfg)
{
    unsigned long args[4];
    memset(cfg, 0, sizeof *cfg);
    cfg->channel = channel;
    cfg->layer_id = layer_id;
    args[0] = 0;
    args[1] = (unsigned long)cfg;
    args[2] = 1;
    args[3] = 0;
    return ioctl(fd, DISP_LAYER_GET_CONFIG, args);
}

static int disp_set_config(int fd, disp_layer_config *cfg)
{
    unsigned long args[4];
    args[0] = 0;
    args[1] = (unsigned long)cfg;
    args[2] = 1;
    args[3] = 0;
    return ioctl(fd, DISP_LAYER_SET_CONFIG, args);
}

static int claim_channel(int *channel_out)
{
    int fd = open(DISP_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logm("cannot open %s: %s", DISP_DEV, strerror(errno));
        return -1;
    }
    for (int i = 0; i < NCHANNELS; i++) {
        disp_layer_config cfg;
        /* Channels 4..7 on this firmware return 0 from both GET and SET but
         * silently ignore writes. Only 2 and 3 are real hardware layers. */
        if (disp_get_config(fd, g_channels[i], 0, &cfg) != 0 || cfg.enable)
            continue;
        close(fd);
        *channel_out = (int)g_channels[i];
        return 0;
    }
    close(fd);
    return -1;
}

/* A UI killed with SIGKILL cannot hide its layer. The flag file records the
 * PID and channel so the next instance can clean up after it. */
static void clear_stale_layer(void)
{
    FILE *f = fopen(LAYER_FLAG, "r");
    if (!f)
        return;
    int pid = -1, ch = -1;
    if (fscanf(f, "%d %d", &pid, &ch) != 2 || pid <= 0 || ch < 0) {
        fclose(f);
        unlink(LAYER_FLAG);
        return;
    }
    fclose(f);
    if (kill(pid, 0) == 0 || errno == EPERM)
        return; /* still alive */
    int fd = open(DISP_DEV, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        disp_layer_config cfg;
        if (disp_get_config(fd, (unsigned)ch, 0, &cfg) == 0 && cfg.enable) {
            cfg.enable = false;
            disp_set_config(fd, &cfg);
            logm("cleared stale layer on ch%d (pid %d gone)", ch, pid);
        }
        close(fd);
    }
    unlink(LAYER_FLAG);
}

int overlay_layer_probe(void)
{
    int ch;
    clear_stale_layer();
    return claim_channel(&ch);
}

static void fill_config(overlay_layer *l, disp_layer_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->info.mode = LAYER_MODE_BUFFER;
    /* Above the UI layer (zorder 16 on this firmware). */
    cfg->info.zorder = (unsigned char)l->zorder;
    cfg->info.alpha_mode = (unsigned char)l->alpha_mode;
    cfg->info.alpha_value = (unsigned char)l->alpha_value;
    cfg->info.screen_win.x = 0;
    cfg->info.screen_win.y = 0;
    cfg->info.screen_win.width = (unsigned)l->width;
    cfg->info.screen_win.height = (unsigned)l->height;
    cfg->info.u.fb.addr[0] = l->phys;
    cfg->info.u.fb.size[0].width = (unsigned)l->width;
    cfg->info.u.fb.size[0].height = (unsigned)l->height;
    cfg->info.u.fb.align[0] = 4;
    cfg->info.u.fb.format = DISP_FORMAT_ARGB8888;
    cfg->info.u.fb.crop.width = ((long long)l->width) << 32;
    cfg->info.u.fb.crop.height = ((long long)l->height) << 32;
    cfg->enable = true;
    cfg->channel = l->channel;
    cfg->layer_id = l->layer_id;
}

static int set_enable(overlay_layer *l, int enable)
{
    disp_layer_config cfg;
    if (!l->disp_fd || !l->pixels)
        return -1;
    fill_config(l, &cfg);
    cfg.enable = enable ? true : false;
    if (disp_set_config(l->disp_fd, &cfg) != 0) {
        logm("layer set_config(enable=%d) failed: %s", enable, strerror(errno));
        return -1;
    }
    l->enabled = enable;
    return 0;
}

int overlay_layer_open(overlay_layer *l, int width, int height)
{
    memset(l, 0, sizeof *l);
    clear_stale_layer();
    int ch = -1;
    if (claim_channel(&ch) != 0) {
        logm("no free disp hardware channel");
        return -1;
    }
    l->fb_fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
    l->disp_fd = open(DISP_DEV, O_RDWR | O_CLOEXEC);
    if (l->fb_fd < 0 || l->disp_fd < 0) {
        logm("cannot open framebuffer/display: %s", strerror(errno));
        goto fail;
    }

    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(l->fb_fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
        ioctl(l->fb_fd, FBIOGET_VSCREENINFO, &var) != 0) {
        logm("FBIOGET failed: %s", strerror(errno));
        goto fail;
    }
    if (var.bits_per_pixel != 32 || var.yres <= 0 || var.yres_virtual <= var.yres) {
        logm("unusable framebuffer geometry %ux%u/%ux%u bpp=%u",
             var.xres, var.yres, var.xres_virtual, var.yres_virtual,
             var.bits_per_pixel);
        goto fail;
    }
    l->map_size = (size_t)fix.line_length * var.yres_virtual;
    l->map = mmap(NULL, l->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  l->fb_fd, 0);
    if (l->map == MAP_FAILED) {
        logm("fb mmap failed: %s", strerror(errno));
        l->map = NULL;
        goto fail;
    }

    int pages = (int)(var.yres_virtual / var.yres);
    int cur = (int)(var.yoffset / var.yres);
    /* Stay well away from the page currently on screen. The emulators use
     * only a handful of the ~18 available pages. */
    int page = pages > 6 ? pages - 3 : (cur + pages / 2) % pages;
    if (page == cur)
        page = (cur + 1) % pages;
    /* Match the live UI layer's geometry exactly. On this firmware it is
     * 720x1280 (the display controller rotates it), not the fb var's
     * 1280x720. Using a different geometry makes the composited image look
     * skewed/glitched. */
    disp_layer_config ui;
    if (disp_get_config(l->disp_fd, 1, 0, &ui) == 0 && ui.enable &&
        ui.info.u.fb.size[0].width > 0 && ui.info.u.fb.size[0].height > 0) {
        l->width = (int)ui.info.u.fb.size[0].width;
        l->height = (int)ui.info.u.fb.size[0].height;
    } else {
        l->width = (int)var.xres;
        l->height = (int)var.yres;
    }
    logm("layer canvas %dx%d -> layer geometry %dx%d", width, height,
         l->width, l->height);

    l->page_off = (size_t)page * var.yres * fix.line_length;
    size_t need = (size_t)l->width * (size_t)l->height * 4u;
    if (l->page_off + need > l->map_size) {
        page = 0;
        l->page_off = 0;
        if (need > l->map_size) {
            logm("layer buffer too large");
            goto fail;
        }
    }
    l->page = page;
    l->pixels = (uint32_t *)(l->map + l->page_off);
    l->stride_px = l->width;
    l->phys = (unsigned long long)fix.smem_start + (unsigned long long)l->page_off;

    l->channel = (unsigned)ch;
    l->layer_id = 0;
    l->zorder = 20;
    l->alpha_mode = 2;   /* global * pixel alpha */
    l->alpha_value = 255;
    if (set_enable(l, 0) != 0)
        goto fail;

    /* Verify the channel really accepted our config before relying on it. */
    if (set_enable(l, 1) != 0) {
        logm("channel %u refused to enable", l->channel);
        goto fail;
    }
    {
        disp_layer_config back;
        if (disp_get_config(l->disp_fd, l->channel, l->layer_id, &back) != 0 ||
            !back.enable || back.info.u.fb.addr[0] != l->phys) {
            logm("channel %u did not retain our layer config", l->channel);
            set_enable(l, 0);
            goto fail;
        }
    }
    set_enable(l, 0);

    memset(l->pixels, 0, (size_t)l->width * (size_t)l->height * 4);
    logm("disp layer ch%d/l%d page=%d phys=0x%llx %dx%d z=%d",
         l->channel, l->layer_id, l->page, l->phys, l->width, l->height,
         l->zorder);
    /* Need a smaller canvas than the fb? The caller rotates/scales into us. */
    (void)width;
    (void)height;
    return 0;

fail:
    overlay_layer_close(l);
    return -1;
}

int overlay_layer_show(overlay_layer *l) { return set_enable(l, 1); }
int overlay_layer_hide(overlay_layer *l) { return set_enable(l, 0); }

void overlay_layer_close(overlay_layer *l)
{
    if (!l)
        return;
    if (l->enabled && l->pixels)
        set_enable(l, 0);
    if (l->map && l->map_size)
        munmap(l->map, l->map_size);
    if (l->disp_fd >= 0)
        close(l->disp_fd);
    if (l->fb_fd >= 0)
        close(l->fb_fd);
    memset(l, 0, sizeof *l);
}
