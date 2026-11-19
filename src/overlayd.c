#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/prctl.h>

#define MAX_STOP 512

static volatile sig_atomic_t g_run = 1;
static overlay_cfg g_cfg;
static pid_t g_stopped[MAX_STOP];
static int g_nstopped = 0;
static int g_has_game = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
}

/* ---------------------------------------------------------------- */
/* input device                                                    */
/* ---------------------------------------------------------------- */

static int find_input_device(char *out, size_t outsz)
{
    const char *env = getenv("OVERLAY_INPUT_DEV");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return 0;
    }
    for (int i = 0; i < 40; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[256] = { 0 };
        int ok = ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0;
        close(fd);
        if (!ok)
            continue;
        if (strstr(name, "TRIMUI") || strstr(name, "Player1")) {
            snprintf(out, outsz, "%s", path);
            return 0;
        }
    }
    if (access("/dev/input/event3", F_OK) == 0) {
        snprintf(out, outsz, "%s", "/dev/input/event3");
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* /proc helpers                                                   */
/* ---------------------------------------------------------------- */

static pid_t read_comm_pid(const char *want)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    pid_t found = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
        if (!strcmp(buf, want)) {
            found = (pid_t)atoi(e->d_name);
            if (found != getpid())
                break;
            found = 0;
        }
    }
    closedir(d);
    return found;
}

static pid_t find_pid_cmdline(const char *needle)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    pid_t found = 0;

    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == getpid())
            continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\0')
                buf[i] = ' ';
        if (strstr(buf, needle)) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static pid_t get_ppid(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p)
        return 0;
    p++;
    while (*p == ' ')
        p++;
    if (*p)
        p++; /* state */
    while (*p == ' ')
        p++;
    return (pid_t)atoi(p);
}

static int collect_tree(pid_t root, pid_t *out, int max)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    static pid_t pids[1024], ppids[1024];
    int np = 0;

    struct dirent *e;
    while ((e = readdir(d)) && np < 1024) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        pid_t pp = get_ppid(pid);
        if (pp <= 0)
            continue;
        pids[np] = pid;
        ppids[np] = pp;
        np++;
    }
    closedir(d);

    int n = 0;
    out[n++] = root;
    for (int i = 0; i < n && n < max; i++) {
        for (int j = 0; j < np && n < max; j++) {
            if (ppids[j] != out[i])
                continue;
            int dup = 0;
            for (int k = 0; k < n; k++)
                if (out[k] == pids[j])
                    dup = 1;
            if (!dup)
                out[n++] = pids[j];
        }
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* stop / resume / kill                                            */
/* ---------------------------------------------------------------- */

static void save_stopped(void)
{
    FILE *f = fopen(STOPPED_FILE, "w");
    if (!f)
        return;
    for (int i = 0; i < g_nstopped; i++)
        fprintf(f, "%d\n", (int)g_stopped[i]);
    fclose(f);
}

static int is_system_daemon(pid_t pid)
{
    static const char *skip[] = {
        "keymon", "trimui_inputd", "trimui_scened", "MainUI",
        "runtrimui.sh", "btmanager", "overlayd", "overlay_ui",
        "sh", "ash", "bash", "udpbcast", "ntpd", "dropbear", NULL
    };
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    for (int i = 0; skip[i]; i++)
        if (!strcmp(buf, skip[i]))
            return 1;
    return 0;
}

static void stop_targets(void)
{
    pid_t pids[MAX_STOP];
    int n = 0;
    g_nstopped = 0;
    g_has_game = 0;

    pid_t anchor = find_pid_cmdline("cmd_to_run.sh");
    if (anchor > 0) {
        n = collect_tree(anchor, pids, MAX_STOP);
        g_has_game = 1;
    }
    if (n == 0) {
        /* fallback: children of runtrimui.sh (when the launcher script exec'd) */
        pid_t rt = find_pid_cmdline("runtrimui.sh");
        pid_t tmp[MAX_STOP];
        int m = 0;
        if (rt > 0)
            m = collect_tree(rt, tmp, MAX_STOP);
        for (int i = 0; i < m && n < MAX_STOP; i++) {
            if (tmp[i] == getpid() || is_system_daemon(tmp[i]))
                continue;
            if (tmp[i] == rt)
                continue;
            pids[n++] = tmp[i];
            g_has_game = 1;
        }
    }
    /* always pause MainUI too (prevents repaint while overlay is up) */
    pid_t mainui = read_comm_pid("MainUI");
    if (mainui > 0) {
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (pids[i] == mainui)
                dup = 1;
        if (!dup && n < MAX_STOP)
            pids[n++] = mainui;
    }

    for (int i = 0; i < n; i++) {
        if (pids[i] == getpid())
            continue;
        if (kill(pids[i], SIGSTOP) == 0) {
            g_stopped[g_nstopped++] = pids[i];
            logm("stopped pid %d", (int)pids[i]);
        }
    }
    save_stopped();
}

static void resume_targets(void)
{
    for (int i = g_nstopped - 1; i >= 0; i--)
        kill(g_stopped[i], SIGCONT);
    if (g_nstopped)
        logm("resumed %d process(es)", g_nstopped);
    g_nstopped = 0;
    unlink(STOPPED_FILE);
}

static void kill_game(void)
{
    logm("exit game requested: %d process(es)", g_nstopped);
    for (int i = 0; i < g_nstopped; i++)
        kill(g_stopped[i], SIGCONT);
    for (int i = 0; i < g_nstopped; i++)
        kill(g_stopped[i], SIGTERM);

    for (int waited = 0; waited < 30; waited++) {
        int alive = 0;
        for (int i = 0; i < g_nstopped; i++)
            if (kill(g_stopped[i], 0) == 0)
                alive++;
        if (!alive)
            break;
        usleep(100000);
    }
    for (int i = 0; i < g_nstopped; i++)
        kill(g_stopped[i], SIGKILL);
    g_nstopped = 0;
    unlink(STOPPED_FILE);
    sync();
}

/* ---------------------------------------------------------------- */
/* overlay                                                         */
/* ---------------------------------------------------------------- */

static void ui_path(char *out, size_t outsz)
{
    const char *env = getenv("OVERLAY_UI");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return;
    }
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, outsz, "%s/overlay_ui", exe);
            if (access(out, X_OK) == 0)
                return;
        }
    }
    snprintf(out, outsz, "/mnt/SDCARD/Apps/OverLyder/bin/overlay_ui");
}

static void run_overlay(int fd)
{
    stop_targets();

    char ui[512];
    ui_path(ui, sizeof ui);
    logm("opening overlay (%s), has_game=%d", ui, g_has_game);

    pid_t pid = fork();
    if (pid == 0) {
        /* If the daemon dies, the UI gets SIGTERM and its handler resumes the
         * paused game before exiting: no frozen game can be left behind. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setenv("OVERLAY_HAS_GAME", g_has_game ? "1" : "0", 1);
        setenv("OVERLAY_CFG", CFG_PATH, 1);
        execl(ui, "overlay_ui", (char *)NULL);
        logm("exec failed: %s", strerror(errno));
        _exit(EXIT_FAILED);
    }
    if (pid < 0) {
        logm("fork failed");
        resume_targets();
        return;
    }

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : EXIT_FAILED;
    logm("overlay closed with code %d", code);

    if (code == EXIT_GAME) {
        kill_game();
    } else if (code == EXIT_REBOOT) {
        resume_targets();
        sync();
        logm("rebooting");
        run_cmd("reboot");
    } else if (code == EXIT_POWEROFF) {
        resume_targets();
        sync();
        logm("powering off");
        run_cmd("poweroff");
    } else {
        resume_targets();
    }
}

/* ---------------------------------------------------------------- */
/* main                                                            */
/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (access(DISABLE_FLAG, F_OK) == 0) {
        logm("overlay disabled by flag, exiting");
        return 0;
    }

    cfg_load(&g_cfg);
    logm("overlayd starting (hold=%dms, trigger_count=%d)", g_cfg.hold_ms,
         g_cfg.trigger_count);

    /* crash recovery: resume anything a previous instance left paused */
    {
        FILE *f = fopen(STOPPED_FILE, "r");
        if (f) {
            int pid;
            while (fscanf(f, "%d", &pid) == 1) {
                logm("resuming stale paused pid %d", pid);
                kill(pid, SIGCONT);
            }
            fclose(f);
            unlink(STOPPED_FILE);
        }
    }

    /* single instance */
    int pidfd = open(PID_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (pidfd >= 0 && flock(pidfd, LOCK_EX | LOCK_NB) < 0) {
        logm("another instance is running, exiting");
        return 0;
    }
    if (pidfd >= 0) {
        char b[32];
        int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
        if (ftruncate(pidfd, 0) == 0)
            write(pidfd, b, (size_t)n);
    }
    if (access("/dev/uinput", F_OK) != 0) { /* sanity, not fatal */
        logm("warning: /dev/uinput missing");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    char dev[64];
    int waited = 0;
    while (find_input_device(dev, sizeof dev) < 0) {
        if (!g_run)
            goto out;
        usleep(500000);
        if (++waited > 120) {
            logm("input device not found, aborting");
            goto out;
        }
    }
    logm("using input device %s", dev);

    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logm("cannot open %s: %s", dev, strerror(errno));
        goto out;
    }

    int full_mask = 0;
    for (int i = 0; i < g_cfg.trigger_count; i++)
        full_mask |= 1 << (g_cfg.trigger[i] & 31);

    int pressed_mask = 0;
    long trig_at = 0;
    int armed = 1;
    int in_overlay = 0;

    while (g_run) {
        struct input_event ev[32];
        ssize_t rd = read(fd, ev, sizeof ev);
        if (rd < 0 && errno != EAGAIN && errno != EINTR)
            break;
        if (rd > 0) {
            int cnt = (int)(rd / (ssize_t)sizeof(struct input_event));
            for (int i = 0; i < cnt; i++) {
                struct input_event *e = &ev[i];
                if (e->type != EV_KEY)
                    continue;
                int bit = 1 << (e->code & 31);
                if (!(full_mask & bit))
                    continue;
                if (e->value == 1) {
                    if (!(pressed_mask & bit))
                        pressed_mask |= bit;
                    if (armed && pressed_mask == full_mask)
                        trig_at = now_ms();
                } else if (e->value == 0) {
                    pressed_mask &= ~bit;
                    if (pressed_mask == 0) {
                        armed = 1;
                        trig_at = 0;
                    }
                }
            }
        }

        if (!in_overlay && armed && trig_at && pressed_mask == full_mask &&
            now_ms() - trig_at >= g_cfg.hold_ms) {
            long held = now_ms() - trig_at;
            (void)held;
            armed = 0;
            trig_at = 0;
            if (access(DISABLE_FLAG, F_OK) == 0) {
                logm("hotkey pressed but overlay is disabled");
                continue;
            }
            in_overlay = 1;
            run_overlay(fd);
            in_overlay = 0;

            /* The overlay grabs the input device, so the release of the key
             * may never have reached this fd. Drain stale events and re-arm
             * so one fresh long press always reopens the overlay. */
            usleep(100000);
            {
                struct input_event junk[32];
                while (read(fd, junk, sizeof junk) > 0)
                    ;
            }
            pressed_mask = 0;
            trig_at = 0;
            armed = 1;
            if (g_run)
                logm("ready again");
        }

        usleep(8000);
    }

    resume_targets();
    close(fd);

out:
    if (pidfd >= 0) {
        unlink(PID_FILE);
        close(pidfd);
    }
    logm("overlayd exiting");
    return 0;
}
