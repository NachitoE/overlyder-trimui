/* fakegame: continuously pan fb0 between two pages, like an emulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    ioctl(fd, FBIOGET_FSCREENINFO, &f); ioctl(fd, FBIOGET_VSCREENINFO, &v);
    size_t mapsz = (size_t)f.line_length * v.yres_virtual;
    unsigned char *mem = mmap(NULL, mapsz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) return 1;
    for (int i = 0; i < 20000; i++) {
        int page = i & 1;
        uint32_t *p = (uint32_t*)(mem + (size_t)page * v.yres * f.line_length);
        uint32_t color = page ? 0xFF002040 : 0xFF102040;
        for (int y = 0; y < (int)v.yres; y += 20)
            for (int x = 0; x < (int)v.xres; x += 20)
                p[(size_t)y * (f.line_length/4) + x] = color;
        struct fb_var_screeninfo nv = v;
        nv.yoffset = page * v.yres;
        ioctl(fd, FBIOPAN_DISPLAY, &nv);
        usleep(20000);
    }
    return 0;
}
