#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
int main(void) {
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_fix_screeninfo f; struct fb_var_screeninfo v;
    printf("ioctl fix=%d var=%d\n", ioctl(fd, FBIOGET_FSCREENINFO, &f), ioctl(fd, FBIOGET_VSCREENINFO, &v));
    printf("id=%s smem_start=0x%lx smem_len=%u line_length=%u\n", f.id, f.smem_start, f.smem_len, f.line_length);
    printf("xres=%u yres=%u xvres=%u yvres=%u xoff=%u yoff=%u bpp=%u\n", v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, v.bits_per_pixel);
    return 0;
}
