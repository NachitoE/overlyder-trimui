/* colorlayer: enable a hardware layer in COLOR mode (no DMA buffer).
 * usage: colorlayer <channel> <seconds> [argb-hex] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

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

static int fd = -1;
static unsigned ch;
static void set_layer(int on, unsigned color)
{
    disp_layer_config c;
    memset(&c, 0, sizeof c);
    c.channel = ch; c.layer_id = 0;
    c.info.mode = 1;                 /* LAYER_MODE_COLOR */
    c.info.zorder = 20;
    c.info.alpha_mode = 1;           /* global alpha */
    c.info.alpha_value = 128;
    c.info.screen_win.x = 0; c.info.screen_win.y = 0;
    c.info.screen_win.width = 720; c.info.screen_win.height = 1280;
    c.info.u.color = color;
    c.enable = on ? 1 : 0;
    unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
    printf("set(%d)=%d errno=%s\n", on, ioctl(fd, DISP_LAYER_SET_CONFIG, a), strerror(errno));
    fflush(stdout);
}
static void cleanup(int sig) { (void)sig; set_layer(0, 0); _exit(0); }
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    ch = (unsigned)atoi(argv[1]);
    int secs = atoi(argv[2]);
    unsigned color = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 16) : 0x00FF0000;
    fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("disp"); return 1; }
    signal(SIGTERM, cleanup); signal(SIGINT, cleanup); signal(SIGALRM, cleanup);
    set_layer(1, color);
    alarm(secs);
    for (;;) { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
}
