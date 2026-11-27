/* dispprobe: read-only probe of Allwinner display2 layers and DISP_MEM slots.
 * Diagnostic tool, not installed with the app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#define DISP_LAYER_GET_CONFIG 0x48
#define DISP_MEM_GETADR       0x2c2

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
    unsigned char pre_multiply;
    disp_rect64 crop;
    int flags;
    int scan;
} disp_fb_info;
typedef struct {
    int mode;
    unsigned char zorder, alpha_mode, alpha_value;
    disp_rect screen_win;
    unsigned char b_trd_out;
    int out_trd_mode;
    union { unsigned int color; disp_fb_info fb; } u;
    unsigned int id;
} disp_layer_info;
typedef struct {
    disp_layer_info info;
    unsigned char enable;
    unsigned int channel, layer_id;
} disp_layer_config;

int main(void)
{
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("open /dev/disp"); return 1; }
    printf("sizeof(disp_layer_config)=%zu\n", sizeof(disp_layer_config));

    for (unsigned ch = 0; ch < 8; ch++) {
        for (unsigned lid = 0; lid < 2; lid++) {
            disp_layer_config c;
            memset(&c, 0, sizeof c);
            c.channel = ch; c.layer_id = lid;
            unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
            int r = ioctl(fd, DISP_LAYER_GET_CONFIG, a);
            printf("ch%u lid%u get=%d enable=%u mode=%d z=%u am=%u av=%u win=(%d,%d,%ux%u) addr0=0x%llx size=%ux%u fmt=%d crop=(%lld,%lld)\n",
                   ch, lid, r, c.enable, c.info.mode, c.info.zorder,
                   c.info.alpha_mode, c.info.alpha_value,
                   c.info.screen_win.x, c.info.screen_win.y,
                   c.info.screen_win.width, c.info.screen_win.height,
                   c.info.u.fb.addr[0], c.info.u.fb.size[0].width,
                   c.info.u.fb.size[0].height, c.info.u.fb.format,
                   c.info.u.fb.crop.width >> 32, c.info.u.fb.crop.height >> 32);
        }
    }
    for (int sel = 0; sel < 10; sel++) {
        unsigned long a[4] = { (unsigned long)sel, 0, 0, 0 };
        long r = ioctl(fd, DISP_MEM_GETADR, a);
        printf("mem[%d]=0x%lx (ioctl=%ld)\n", sel, (unsigned long)r, r);
    }
    close(fd);
    return 0;
}
