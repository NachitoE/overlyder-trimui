/* vpad: virtual gamepad used for automated testing of the overlay.
 * Creates a uinput device named "TRIMUI VPAD TEST" and replays simple
 * commands from stdin.  Not part of the installed app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>

static int ufd;

static void emit(int type, int code, int val)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.code = code;
    e.value = val;
    if (write(ufd, &e, sizeof e) < 0)
        perror("write uinput");
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static int find_event(const char *name, char *out, size_t n)
{
    for (int i = 0; i < 40; i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char got[256] = { 0 };
        int ok = ioctl(fd, EVIOCGNAME(sizeof got - 1), got) >= 0;
        close(fd);
        if (ok && strstr(got, name)) {
            snprintf(out, n, "%s", p);
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        perror("open /dev/uinput");
        return 1;
    }
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    int keys[] = { 305, 304, 308, 307, 310, 311, 314, 315, 316 };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
        ioctl(ufd, UI_SET_KEYBIT, keys[i]);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    ioctl(ufd, UI_SET_ABSBIT, ABS_HAT0X);
    ioctl(ufd, UI_SET_ABSBIT, ABS_HAT0Y);
    ioctl(ufd, UI_SET_ABSBIT, ABS_X);
    ioctl(ufd, UI_SET_ABSBIT, ABS_Y);
    ioctl(ufd, UI_SET_ABSBIT, ABS_RX);
    ioctl(ufd, UI_SET_ABSBIT, ABS_RY);
    ioctl(ufd, UI_SET_ABSBIT, ABS_Z);   /* L2 / LT */
    ioctl(ufd, UI_SET_ABSBIT, ABS_RZ);  /* R2 / RT */

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, sizeof ud.name, "TRIMUI VPAD TEST");
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor = 0x1234;
    ud.id.product = 0x5678;
    ud.id.version = 1;
    ud.absmin[ABS_HAT0X] = -1;
    ud.absmax[ABS_HAT0X] = 1;
    ud.absmin[ABS_HAT0Y] = -1;
    ud.absmax[ABS_HAT0Y] = 1;
    ud.absmin[ABS_X] = -32767;
    ud.absmax[ABS_X] = 32767;
    ud.absmin[ABS_Y] = -32767;
    ud.absmax[ABS_Y] = 32767;
    ud.absmin[ABS_RX] = -32767;
    ud.absmax[ABS_RX] = 32767;
    ud.absmin[ABS_RY] = -32767;
    ud.absmax[ABS_RY] = 32767;
    ud.absmin[ABS_Z] = 0;
    ud.absmax[ABS_Z] = 255;
    ud.absmin[ABS_RZ] = 0;
    ud.absmax[ABS_RZ] = 255;
    if (write(ufd, &ud, sizeof ud) < 0)
        perror("write uinput_user_dev");
    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        return 1;
    }
    usleep(400000);

    char path[64];
    if (find_event("TRIMUI VPAD TEST", path, sizeof path) < 0) {
        fprintf(stderr, "virtual device not found\n");
        return 1;
    }
    printf("%s\n", path);
    fflush(stdout);
    fprintf(stderr, "vpad ready at %s\n", path);

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[64];
        int a = 0, b = 0;
        if (sscanf(line, "%63s", cmd) != 1)
            continue;
        if (!strcmp(cmd, "quit"))
            break;
        if (!strcmp(cmd, "sleep")) {
            sscanf(line, "%*s %d", &a);
            usleep(a * 1000);
        } else if (!strcmp(cmd, "tap")) {
            sscanf(line, "%*s %d", &a);
            emit(EV_KEY, a, 1);
            syn();
            usleep(70000);
            emit(EV_KEY, a, 0);
            syn();
        } else if (!strcmp(cmd, "hold")) {
            sscanf(line, "%*s %d %d", &a, &b);
            emit(EV_KEY, a, 1);
            syn();
            usleep(b * 1000);
            emit(EV_KEY, a, 0);
            syn();
        } else if (!strcmp(cmd, "press")) {
            sscanf(line, "%*s %d", &a);
            emit(EV_KEY, a, 1);
            syn();
        } else if (!strcmp(cmd, "release")) {
            sscanf(line, "%*s %d", &a);
            emit(EV_KEY, a, 0);
            syn();
        } else if (!strcmp(cmd, "axis")) {
            sscanf(line, "%*s %d %d", &a, &b);
            emit(EV_ABS, a, b);
            syn();
        } else if (!strcmp(cmd, "ax")) {
            /* analog stick: ax X Y */
            int x = 0, y = 0;
            sscanf(line, "%*s %d %d", &x, &y);
            emit(EV_ABS, ABS_X, x);
            emit(EV_ABS, ABS_Y, y);
            syn();
        }
    }

    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}
