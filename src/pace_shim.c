/* liboverlyder_pace.so - global frame limiter for OverLyder.
 *
 * Intercepts the four ways games on this device present a frame:
 *   eglSwapBuffers (EGL/Vulkan/GL), glXSwapBuffers, SDL_RenderPresent,
 *   SDL_GL_SwapWindow and fbdev's FBIOPAN_DISPLAY.
 * It sleeps to the next frame deadline, like frame-pacer/fpsvita do on other
 * platforms. No clocks, no game files, no state beyond the frame deadline.
 *
 * The library is listed in /etc/ld.so.preload (one line, installed and removed
 * by OverLyder). It is a NO-OP unless:
 *   - /tmp/overlyder_fps_limit contains 30 or 20 (written by overlayd), and
 *   - the executable lives under /mnt/SDCARD/ but is not part of OverLyder.
 * OVERLYDER_PACE_TEST=<fps> forces pacing for any process (test harness only).
 *
 * Caveat: capping a game whose simulation advances per frame will change its
 * speed. Default/Off is always safe. See docs/FRAME_LIMITER.md. */
#define _GNU_SOURCE
#include "common.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static dev_t g_fb_dev;
static volatile long long g_next_us;

__attribute__((constructor))
static void pace_init(void)
{
    struct stat st;
    if (stat("/dev/fb0", &st) == 0)
        g_fb_dev = st.st_rdev;
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/* Never pace OverLyder itself, the firmware UI or system tools. PortMaster
 * ports and emulator binaries live under /mnt/SDCARD/ and are included. */
static int is_game(void)
{
    const char *force = getenv("OVERLYDER_PACE_TEST");
    if (force && atoi(force) > 0)
        return 1;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return 0;
    exe[n] = '\0';
    if (strncmp(exe, "/mnt/SDCARD/", 12))
        return 0;
    if (strstr(exe, "/Apps/OverLyder/"))
        return 0;
    return 1;
}

static int target_fps(void)
{
    static long long checked;
    static int cached;
    long long t = now_us();
    if (t - checked < 500000LL)
        return cached;
    checked = t;
    cached = 0;

    int want = 0;
    const char *force = getenv("OVERLYDER_PACE_TEST");
    if (force && *force)
        want = atoi(force);
    else {
        FILE *f = fopen(FPS_LIMIT_FILE, "r");
        if (f) {
            if (fscanf(f, "%d", &want) != 1)
                want = 0;
            fclose(f);
        }
    }
    if (want != 30 && want != 20)
        return 0;
    if (!is_game())
        return 0;
    cached = want;
    return want;
}

static void pace(int fps)
{
    long long period = 1000000LL / fps;
    long long now = now_us();
    long long next = __sync_add_and_fetch(&g_next_us, 0);

    /* First frame, or a long stall: start a fresh schedule instead of trying
     * to pay back a debt that would cause a burst of frames. */
    if (next == 0 || now > next + period * 3) {
        next = now;
    }
    if (now < next) {
        struct timespec req = { next / 1000000LL,
                                (long)((next % 1000000LL) * 1000LL) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL) < 0 &&
               errno == EINTR)
            ;
        now = now_us();
    }
    long long due = next > now ? next : now;
    long long new_next = due + period;
    long long old;
    do {
        old = __sync_add_and_fetch(&g_next_us, 0);
    } while (!__sync_bool_compare_and_swap(&g_next_us, old, new_next));
}

/* --------------------------- swap hooks --------------------------- */

void eglSwapBuffers(void *display, void *surface)
{
    static void (*real_fn)(void *, void *);
    if (!real_fn)
        real_fn = (void (*)(void *, void *))dlsym(RTLD_NEXT, "eglSwapBuffers");
    int fps = target_fps();
    if (fps)
        pace(fps);
    if (real_fn)
        real_fn(display, surface);
}

void glXSwapBuffers(void *display, void *drawable)
{
    static void (*real_fn)(void *, void *);
    if (!real_fn)
        real_fn = (void (*)(void *, void *))dlsym(RTLD_NEXT, "glXSwapBuffers");
    int fps = target_fps();
    if (fps)
        pace(fps);
    if (real_fn)
        real_fn(display, drawable);
}

void SDL_RenderPresent(void *renderer)
{
    static void (*real_fn)(void *);
    if (!real_fn)
        real_fn = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_RenderPresent");
    int fps = target_fps();
    if (fps)
        pace(fps);
    if (real_fn)
        real_fn(renderer);
}

void SDL_GL_SwapWindow(void *window)
{
    static void (*real_fn)(void *);
    if (!real_fn)
        real_fn = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    int fps = target_fps();
    if (fps)
        pace(fps);
    if (real_fn)
        real_fn(window);
}

int ioctl(int fd, unsigned long request, ...)
{
    typedef int (*ioctl_fn)(int, unsigned long, ...);
    static ioctl_fn real_fn;
    if (!real_fn)
        real_fn = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (request == FBIOPAN_DISPLAY && target_fps()) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_rdev == g_fb_dev && g_fb_dev != 0)
            pace(target_fps());
    }
    if (real_fn)
        return real_fn(fd, request, arg);
    return (int)syscall(SYS_ioctl, fd, request, arg);
}
