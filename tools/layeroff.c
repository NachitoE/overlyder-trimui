/* layeroff <channel>: disable a display layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int fd = open("/dev/disp", O_RDWR);
    disp_layer_config c; memset(&c, 0, sizeof c);
    c.channel = (unsigned)atoi(argv[1]); c.layer_id = 0;
    unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
    ioctl(fd, 0x48, a);
    c.enable = 0;
    printf("disable ch%u = %d\n", c.channel, ioctl(fd, 0x47, a));
    close(fd); return 0;
}
