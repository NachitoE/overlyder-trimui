/* absinfo <event-device>: print analog axis ranges of an evdev device. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/input/event4";
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }
    static const int axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ };
    static const char *names[] = { "ABS_X", "ABS_Y", "ABS_Z(L2?)",
                                   "ABS_RX", "ABS_RY", "ABS_RZ(R2?)" };
    for (unsigned i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        struct input_absinfo a;
        int r = ioctl(fd, EVIOCGABS(axes[i]), &a);
        printf("%-12s code=%2d ret=%d min=%d max=%d value=%d\n",
               names[i], axes[i], r, a.minimum, a.maximum, a.value);
    }
    close(fd);
    return 0;
}
