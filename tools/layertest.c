/* layertest: claim a disp layer, paint a translucent rectangle, then clean up.
 * Diagnostic tool, not installed with the app.
 * usage: layertest <channel> <mem_id> <seconds> [color-hex] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48
#define DISP_MEM_REQUEST      0x2c0
#define DISP_MEM_RELEASE      0x2c1
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

static int fd = -1, memid = -1;
static unsigned long phys;
static size_t size;
static unsigned char *mem;
static unsigned channel;

static void set_layer(int on)
{
    disp_layer_config c;
    memset(&c, 0, sizeof c);
    c.channel = channel; c.layer_id = 0;
    c.info.mode = 0;
    c.info.zorder = 20;
    c.info.alpha_mode = 2;
    c.info.alpha_value = 255;
    c.info.screen_win.x = 0; c.info.screen_win.y = 0;
    c.info.screen_win.width = 720; c.info.screen_win.height = 1280;
    c.info.u.fb.addr[0] = phys;
    c.info.u.fb.size[0].width = 720; c.info.u.fb.size[0].height = 1280;
    c.info.u.fb.align[0] = 4;
    c.info.u.fb.format = 0;
    c.info.u.fb.crop.width = 720LL << 32;
    c.info.u.fb.crop.height = 1280LL << 32;
    c.enable = on ? 1 : 0;
    unsigned long a[4] = { 0, (unsigned long)&c, 1, 0 };
    int r = ioctl(fd, DISP_LAYER_SET_CONFIG, a);
    printf("set_layer(%d) = %d (%s)\n", on, r, strerror(errno));
    fflush(stdout);
}

static void cleanup(int sig)
{
    (void)sig;
    if (fd >= 0 && memid >= 0) {
        set_layer(0);
        unsigned long a[4] = { (unsigned long)memid, 0, 0, 0 };
        ioctl(fd, DISP_MEM_RELEASE, a);
    }
    _exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <channel> <mem_id> <seconds> [color-hex]\n", argv[0]);
        return 2;
    }
    channel = (unsigned)atoi(argv[1]);
    memid = atoi(argv[2]);
    int secs = atoi(argv[3]);
    unsigned color = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 16) : 0x80FF0000;

    fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("open disp"); return 1; }
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    signal(SIGALRM, cleanup);

    size = 720u * 1280u * 4u;
    unsigned long a[4] = { (unsigned long)memid, (unsigned long)size, 0, 0 };
    printf("mem request ch%u slot%d size=%zu: %d\n", channel, memid, size,
           ioctl(fd, DISP_MEM_REQUEST, a));
    a[0] = (unsigned long)memid;
    /* ioctl() returns int: a 32-bit physical address above 2 GiB comes back
     * sign-extended. Mask it before using it as an mmap offset. */
    phys = (uint32_t)(int)ioctl(fd, DISP_MEM_GETADR, a);
    printf("phys=0x%lx\n", phys);
    if (!phys) return 1;
    int mf = open("/dev/mem", O_RDWR);
    if (mf < 0) { perror("open mem"); return 1; }
    mem = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, mf, (off_t)(phys & ~4095UL));
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    /* fill a centered rectangle, rest transparent */
    for (int y = 0; y < 1280; y++) {
        for (int x = 0; x < 720; x++) {
            uint32_t v = (x > 60 && x < 660 && y > 100 && y < 1180) ? color : 0;
            ((uint32_t*)mem)[(size_t)y * 720 + x] = v;
        }
    }
    set_layer(1);
    alarm(secs > 0 ? secs : 5);
    for (;;) pause();
}
