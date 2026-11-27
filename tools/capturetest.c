/* capturetest: enable a translucent layer over a spare fb page, capture the
 * composited screen into another fb page and print sample pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_CAPTURE_START    0x140
#define DISP_CAPTURE_STOP     0x141
#define DISP_CAPTURE_COMMIT   0x142

typedef struct { int x, y; unsigned int width, height; } disp_rect;
typedef struct { long long x, y, width, height; } disp_rect64;
typedef struct { unsigned int width, height; } disp_rectsz;
typedef struct { unsigned long long addr[3]; disp_rectsz size[3]; unsigned int align[3];
  int format, color_space; unsigned int trd_right_addr[3];
  unsigned char pre_multiply; disp_rect64 crop; int flags, scan; } disp_fb_info;
typedef struct { int mode; unsigned char zorder, alpha_mode, alpha_value;
  disp_rect screen_win; unsigned char b_trd_out; int out_trd_mode;
  union { unsigned int color; disp_fb_info fb; } u; unsigned int id; } disp_layer_info;
typedef struct { disp_layer_info info; unsigned char enable; unsigned int channel, layer_id; } disp_layer_config;
typedef int disp_pixel_format;
typedef struct { disp_pixel_format format; disp_rectsz size[3]; disp_rect crop;
  unsigned long long addr[3]; } disp_s_frame;
typedef struct { disp_rect window; disp_s_frame out_frame; } disp_capture_info;

int main(int argc, char **argv)
{
    unsigned ch = argc > 1 ? (unsigned)atoi(argv[1]) : 3;
    int page = argc > 2 ? atoi(argv[2]) : 15;
    int cpage = argc > 3 ? atoi(argv[3]) : 14;
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    int ffd = open("/dev/fb0", O_RDWR), dfd = open("/dev/disp", O_RDWR);
    if (ffd < 0 || dfd < 0) { perror("open"); return 1; }
    ioctl(ffd, FBIOGET_FSCREENINFO, &f); ioctl(ffd, FBIOGET_VSCREENINFO, &v);
    size_t mapsz = (size_t)f.line_length * v.yres_virtual;
    unsigned char *mem = mmap(NULL, mapsz, PROT_READ|PROT_WRITE, MAP_SHARED, ffd, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    unsigned long long phys = (unsigned long long)f.smem_start +
        (unsigned long long)page * v.yres * f.line_length;
    printf("layer phys=0x%llx capture page=%d\n", phys, cpage);
    uint32_t *lp = (uint32_t*)(mem + (size_t)page * v.yres * f.line_length);
    for (int y = 0; y < (int)v.yres; y++)
        for (int x = 0; x < (int)v.xres; x++)
            lp[(size_t)y * (f.line_length / 4) + x] =
                (x > 100 && x < 1180 && y > 80 && y < 640) ? 0xC8FF2020u : 0u;

    disp_layer_config c; memset(&c, 0, sizeof c);
    c.channel = ch; c.layer_id = 0; c.info.mode = 0; c.info.zorder = 20;
    c.info.alpha_mode = 2; c.info.alpha_value = 255;
    c.info.screen_win.width = 1280; c.info.screen_win.height = 720;
    c.info.u.fb.addr[0] = phys;
    c.info.u.fb.size[0].width = 1280; c.info.u.fb.size[0].height = 720;
    c.info.u.fb.align[0] = 4; c.info.u.fb.format = 0;
    c.info.u.fb.crop.width = 1280LL << 32; c.info.u.fb.crop.height = 720LL << 32;
    c.enable = 1;
    unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
    printf("set layer=%d\n", ioctl(dfd, DISP_LAYER_SET_CONFIG, a));

    disp_capture_info ci; memset(&ci, 0, sizeof ci);
    ci.window.x = 0; ci.window.y = 0;
    ci.window.width = 1280; ci.window.height = 720;
    ci.out_frame.format = 0;
    ci.out_frame.size[0].width = 1280; ci.out_frame.size[0].height = 720;
    ci.out_frame.crop.width = 1280; ci.out_frame.crop.height = 720;
    ci.out_frame.addr[0] = (unsigned long long)f.smem_start +
        (unsigned long long)cpage * v.yres * f.line_length;
    int cs = ioctl(dfd, DISP_CAPTURE_START, a);
    usleep(300000);
    printf("cap start=%d commit=%d\n", cs,
           ioctl(dfd, DISP_CAPTURE_COMMIT, (unsigned long)&ci));
    sleep(2);
    uint32_t *cap = (uint32_t*)(mem + (size_t)cpage * v.yres * f.line_length);
    for (int y = 100; y < 640; y += 180) {
        printf("y=%d: ", y);
        for (int x = 200; x < 1100; x += 225)
            printf("%08x ", cap[(size_t)y * (f.line_length/4) + x]);
        printf("\n");
    }
    c.enable = 0;
    ioctl(dfd, DISP_LAYER_SET_CONFIG, a);
    ioctl(dfd, DISP_CAPTURE_STOP, a);
    close(dfd); close(ffd);
    return 0;
}
