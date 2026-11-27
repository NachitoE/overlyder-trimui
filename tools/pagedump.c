/* pagedump <page>: print sample pixels of an fb0 virtual page. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
int main(int argc, char **argv) {
    int page = argc > 1 ? atoi(argv[1]) : 15;
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    ioctl(fd, FBIOGET_FSCREENINFO, &f); ioctl(fd, FBIOGET_VSCREENINFO, &v);
    size_t mapsz = (size_t)f.line_length * v.yres_virtual;
    unsigned char *mem = mmap(NULL, mapsz, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    uint32_t *p = (uint32_t*)(mem + (size_t)page*v.yres*f.line_length);
    printf("page=%d yoff=%u: ", page, v.yoffset);
    for (int i = 0; i < 8; i++)
        printf("%08x ", p[(size_t)(100+i*40) * (f.line_length/4) + 200+i*100]);
    printf("\n");
    return 0;
}
