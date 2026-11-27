/* fblayertest: memory layer backed by a spare fb0 page (safe RAM).
 * usage: fblayertest <channel> <page> <seconds> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

typedef struct { int x, y; unsigned int width, height; } disp_rect;
typedef struct { long long x, y, width, height; } disp_rect64;
typedef struct { unsigned int width, height; } disp_rectsz;
typedef struct {
    unsigned long long addr[3]; disp_rectsz size[3]; unsigned int align[3];
    int format, color_space; unsigned int trd_right_addr[3];
    unsigned char pre_multiply; disp_rect64 crop; int flags, scan;
} disp_fb_info;
typedef struct {
    int mode; unsigned char zorder, alpha_mode, alpha_value;
    disp_rect screen_win; unsigned char b_trd_out; int out_trd_mode;
    union { unsigned int color; disp_fb_info fb; } u; unsigned int id;
} disp_layer_info;
typedef struct { disp_layer_info info; unsigned char enable; unsigned int channel, layer_id; } disp_layer_config;

static int dfd = -1, ffd = -1;
static unsigned channel;
static unsigned long long addr;
static void set_layer(int on)
{
    disp_layer_config c; memset(&c, 0, sizeof c);
    c.channel = channel; c.layer_id = 0;
    c.info.mode = 0; c.info.zorder = 20;
    c.info.alpha_mode = 2; c.info.alpha_value = 200;
    c.info.screen_win.x = 0; c.info.screen_win.y = 0;
    c.info.screen_win.width = 1280; c.info.screen_win.height = 720;
    c.info.u.fb.addr[0] = addr;
    c.info.u.fb.size[0].width = 1280; c.info.u.fb.size[0].height = 720;
    c.info.u.fb.align[0] = 4; c.info.u.fb.format = 0;
    c.info.u.fb.crop.width = 1280LL << 32; c.info.u.fb.crop.height = 720LL << 32;
    c.enable = on ? 1 : 0;
    unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
    printf("set(%d)=%d\n", on, ioctl(dfd, DISP_LAYER_SET_CONFIG, a)); fflush(stdout);
}
static void cleanup(int sig) { (void)sig; set_layer(0); _exit(0); }
int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    channel = (unsigned)atoi(argv[1]);
    int page = atoi(argv[2]);
    int secs = atoi(argv[3]);
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    ffd = open("/dev/fb0", O_RDWR);
    dfd = open("/dev/disp", O_RDWR);
    if (ffd < 0 || dfd < 0) { perror("open"); return 1; }
    ioctl(ffd, FBIOGET_FSCREENINFO, &f);
    ioctl(ffd, FBIOGET_VSCREENINFO, &v);
    size_t mapsz = (size_t)f.line_length * v.yres_virtual;
    unsigned char *mem = mmap(NULL, mapsz, PROT_READ|PROT_WRITE, MAP_SHARED, ffd, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    size_t off = (size_t)page * v.yres * f.line_length;
    printf("smem=0x%llx line=%u yres=%u vres=%u page=%d off=%zu yoff=%u\n",
           (unsigned long long)f.smem_start, f.line_length, v.yres, v.yres_virtual, page, off, v.yoffset);
    addr = ((unsigned long long)f.smem_start) + off;
    uint32_t *p = (uint32_t*)(mem + off);
    for (unsigned y = 0; y < v.yres; y++) {
        for (unsigned x = 0; x < v.xres; x++) {
            p[(size_t)y * (f.line_length / 4) + x] =
                (x > 100 && x < 1180 && y > 80 && y < 640) ? 0xC8FF2020u : 0u;
        }
    }
    signal(SIGTERM, cleanup); signal(SIGINT, cleanup); signal(SIGALRM, cleanup);
    set_layer(1);
    alarm(secs);
    for (;;) { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
}
