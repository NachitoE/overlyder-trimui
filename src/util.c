#include "common.h"

#include <ctype.h>
#include <sys/stat.h>

long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void logm(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);

    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s %s\n", ts, buf);
        fclose(f);
    }
    fprintf(stderr, "%s %s\n", ts, buf);
    fflush(stderr);
}

int read_int_file(const char *path, int def)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return def;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return def;
    buf[n] = '\0';
    return atoi(buf);
}

int write_int_file(const char *path, int v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", v);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return (w == n) ? 0 : -1;
}

int write_str_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, s, strlen(s));
    close(fd);
    return (w == (ssize_t)strlen(s)) ? 0 : -1;
}

int run_cmd(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    int st = system(cmd);
    if (st == -1)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}

int read_cmd(char *out, size_t outsz, const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);

    if (outsz)
        out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t used = 0;
    char line[2048];
    while (fgets(line, sizeof line, p)) {
        size_t l = strlen(line);
        if (used + l + 1 >= outsz)
            break;
        memcpy(out + used, line, l + 1);
        used += l;
    }
    int st = pclose(p);
    return (st == 0) ? 0 : -1;
}

int keycode_from_name(const char *name)
{
    static const struct { const char *n; int code; } map[] = {
        { "home", K_MODE },  { "menu", K_MODE },   { "mode", K_MODE },
        { "select", K_SELECT }, { "start", K_START },
        { "a", K_A }, { "b", K_B }, { "x", K_X }, { "y", K_Y },
        { "l1", K_L1 }, { "r1", K_R1 }, { "l", K_L1 }, { "r", K_R1 },
    };
    if (!name || !*name)
        return -1;
    if (isdigit((unsigned char)name[0]))
        return atoi(name);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(name, map[i].n))
            return map[i].code;
    return -1;
}

void cfg_defaults(overlay_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->hold_ms = 700;
    c->trigger[0] = K_MODE;
    c->trigger_count = 1;
    c->dim = 55;
    c->step = 5;
    c->confirm_exit = 1;
    c->lang = 0; /* Spanish by default */
    c->led_color = 0xFFFFFF;
    c->led_color_set = 0; /* do not override CrossMix LED presets by default */
}

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

int cfg_load(overlay_cfg *c)
{
    cfg_defaults(c);
    FILE *f = fopen(CFG_PATH, "r");
    if (!f)
        return -1;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcasecmp(k, "hold_ms")) {
            int n = atoi(v);
            if (n >= 100 && n <= 5000)
                c->hold_ms = n;
        } else if (!strcasecmp(k, "trigger") || !strcasecmp(k, "hotkey")) {
            int cnt = 0;
            char *tok = strtok(v, ",+ ");
            while (tok && cnt < 4) {
                int code = keycode_from_name(tok);
                if (code > 0)
                    c->trigger[cnt++] = code;
                tok = strtok(NULL, ",+ ");
            }
            if (cnt > 0) {
                c->trigger_count = cnt;
                /* clear unused slots */
                for (int i = cnt; i < 4; i++)
                    c->trigger[i] = 0;
            }
        } else if (!strcasecmp(k, "dim")) {
            int n = atoi(v);
            if (n >= 0 && n <= 90)
                c->dim = n;
        } else if (!strcasecmp(k, "step")) {
            int n = atoi(v);
            if (n >= 1 && n <= 25)
                c->step = n;
        } else if (!strcasecmp(k, "confirm_exit")) {
            c->confirm_exit = atoi(v) ? 1 : 0;
        } else if (!strcasecmp(k, "lang")) {
            if (!strcasecmp(v, "en") || !strcmp(v, "1"))
                c->lang = 1;
            else if (!strcasecmp(v, "pt") || !strcmp(v, "2"))
                c->lang = 2;
            else if (!strcasecmp(v, "es") || !strcmp(v, "0"))
                c->lang = 0;
        } else if (!strcasecmp(k, "ledcolor")) {
            const char *hex = v;
            if (*hex == '#')
                hex++;
            else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
                hex += 2;
            if (*hex && isxdigit((unsigned char)hex[0])) {
                unsigned long rgb = strtoul(hex, NULL, 16);
                if (rgb <= 0xFFFFFF) {
                    c->led_color = (uint32_t)rgb;
                    c->led_color_set = 1;
                }
            }
        }
    }
    fclose(f);
    return 0;
}

/* Apply a static color to the stick rings and the center LED. The driver
 * exposes these as RRGGBB strings; effect 4 is "static" and cycles -1 means
 * keep it until something else changes it. Brightness stays under the control
 * of the LED brightness row (max_scale / effect_enable). */
void led_color_apply(uint32_t rgb)
{
    char hex[16];
    snprintf(hex, sizeof hex, "%06X ", rgb & 0xFFFFFF);

    write_str_file(LED_SYS "effect_rgb_hex_lr", hex);
    write_str_file(LED_SYS "effect_rgb_hex_l", hex);
    write_str_file(LED_SYS "effect_rgb_hex_r", hex);
    write_str_file(LED_SYS "effect_rgb_hex_m", hex);

    int effect = rgb ? 4 : 0; /* 0 = disable, 4 = static */
    write_int_file(LED_SYS "effect_lr", effect);
    write_int_file(LED_SYS "effect_l", effect);
    write_int_file(LED_SYS "effect_r", effect);
    write_int_file(LED_SYS "effect_m", effect);
    write_int_file(LED_SYS "effect_cycles_lr", -1);
    write_int_file(LED_SYS "effect_cycles_m", -1);
}

/* Persist the color in overlay.cfg without touching the other settings (the
 * config screen rewrites the whole file on its own). When enabled is 0 the
 * ledcolor line is removed, so the daemon leaves CrossMix's LED preset alone
 * at boot. */
void cfg_update_led_color(overlay_cfg *c, uint32_t rgb, int enabled)
{
    c->led_color = rgb & 0xFFFFFF;
    c->led_color_set = enabled ? 1 : 0;

    FILE *f = fopen(CFG_PATH, "r");
    if (!f)
        return;
    char lines[64][256];
    int n = 0;
    while (n < 64 && fgets(lines[n], sizeof lines[0], f)) {
        char key[512], *eq;
        snprintf(key, sizeof key, "%s", lines[n]);
        eq = strchr(key, '=');
        if (eq) {
            *eq = '\0';
            char *k = key;
            while (*k && isspace((unsigned char)*k))
                k++;
            if (!strcasecmp(k, "ledcolor"))
                continue; /* drop old value */
        }
        n++;
    }
    fclose(f);

    FILE *out = fopen(CFG_PATH ".tmp", "w");
    if (!out)
        return;
    for (int i = 0; i < n; i++)
        fputs(lines[i], out);
    if (enabled)
        fprintf(out, "ledcolor=%06X\n", c->led_color);
    fclose(out);
    if (rename(CFG_PATH ".tmp", CFG_PATH) == 0)
        sync();
}
