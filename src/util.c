#include "common.h"

#include <ctype.h>
#include <math.h>
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
        { "l2", K_L2 }, { "r2", K_R2 }, { "lt", K_L2 }, { "rt", K_R2 },
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

/* Minimal JSON string reader: our JSON files are flat objects of string
 * values, so pulling them out with strstr is enough and avoids a dependency.
 * Returns 1 when the key was found. */
static int json_string(const char *json, const char *key, char *out, size_t n)
{
    char needle[96];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return 0;
    p = strchr(p, ':');
    if (!p)
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p++ != '"')
        return 0;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            out[i++] = (*p == 'n') ? '\n' : *p;
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return 1;
}

/* Load a flat JSON object of string values into a table indexed by `keys`.
 * Missing keys are left empty. Returns the number of values loaded. */
int json_load_strings(const char *path, const char *const *keys,
                      char (*out)[256], int n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[32768];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    buf[len] = '\0';
    fclose(f);
    int loaded = 0;
    for (int i = 0; i < n; i++) {
        out[i][0] = '\0';
        if (json_string(buf, keys[i], out[i], 256))
            loaded++;
    }
    return loaded;
}

void manifest_load(app_manifest *m)
{
    memset(m, 0, sizeof *m);
    /* Everything comes from manifest.json: it is the single file to edit when
     * bumping the version. This is only a placeholder for a missing manifest. */
    snprintf(m->version, sizeof m->version, "0.0.0");
    snprintf(m->owner, sizeof m->owner, "NachitoE");
    snprintf(m->repo, sizeof m->repo, "overlyder-trimui");
    snprintf(m->updater, sizeof m->updater, "updater.sh");
    snprintf(m->credits, sizeof m->credits, "github.com/nachitoe");

    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f)
        return;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);

    json_string(buf, "version", m->version, sizeof m->version);
    json_string(buf, "owner", m->owner, sizeof m->owner);
    json_string(buf, "repo", m->repo, sizeof m->repo);
    json_string(buf, "updater", m->updater, sizeof m->updater);
    json_string(buf, "credits", m->credits, sizeof m->credits);
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
    c->upd_reinstall = 0; /* never reinstall the same version unless asked */
    c->no_sigstop = 1;    /* draw on a hardware layer, leave the game running */
    c->keyboard_mode = 2; /* compact keyboard by default */
    c->kbd_trigger[0] = K_SELECT;
    c->kbd_trigger[1] = K_L1;
    c->kbd_trigger_count = 2; /* SELECT + L1 opens the keyboard */
    c->fps_hud = 0;           /* off until enabled in the settings screen */
    c->fps_hud_pos = 0;       /* top-left */
    c->fps_hud_style = HUD_STYLE_SIMPLE; /* no box over the game */
    c->fps_hud_fields = HUD_F_FPS | HUD_F_FRAME | HUD_F_CPU |
                        HUD_F_GPU | HUD_F_TEMP;
    c->fps_limit = 0;         /* no cap by default */
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

/* Parse "code,code,..." (or key names) into a trigger array. */
static int parse_trigger(char *v, int *out)
{
    int cnt = 0;
    char *tok = strtok(v, ",+ ");
    while (tok && cnt < 4) {
        int code = keycode_from_name(tok);
        if (code > 0)
            out[cnt++] = code;
        tok = strtok(NULL, ",+ ");
    }
    for (int i = cnt; i < 4; i++)
        out[i] = 0;
    return cnt;
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
            int cnt = parse_trigger(v, c->trigger);
            if (cnt > 0)
                c->trigger_count = cnt;
        } else if (!strcasecmp(k, "kbd_trigger") || !strcasecmp(k, "keyboard_hotkey")) {
            c->kbd_trigger_count = parse_trigger(v, c->kbd_trigger);
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
        } else if (!strcasecmp(k, "upd_reinstall")) {
            c->upd_reinstall = atoi(v) ? 1 : 0;
        } else if (!strcasecmp(k, "no_sigstop")) {
            c->no_sigstop = atoi(v) ? 1 : 0;
        } else if (!strcasecmp(k, "keyboard_mode")) {
            int m = atoi(v);
            c->keyboard_mode = (m == 1 || m == 2) ? m : 2;
        } else if (!strcasecmp(k, "fps_hud")) {
            c->fps_hud = atoi(v) ? 1 : 0;
        } else if (!strcasecmp(k, "fps_hud_pos")) {
            int p = atoi(v);
            c->fps_hud_pos = (p >= 0 && p <= 3) ? p : 0;
        } else if (!strcasecmp(k, "fps_hud_style")) {
            c->fps_hud_style = atoi(v) == HUD_STYLE_WINDOW ? HUD_STYLE_WINDOW
                                                          : HUD_STYLE_SIMPLE;
        } else if (!strcasecmp(k, "fps_hud_fields")) {
            unsigned long bits = strtoul(v, NULL, 0);
            /* Unknown bits are dropped so a future field cannot leak in. */
            c->fps_hud_fields = (unsigned)bits & HUD_FIELDS_ALL;
        } else if (!strcasecmp(k, "fps_limit")) {
            int n = atoi(v);
            c->fps_limit = (n == 30 || n == 20) ? n : 0;
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

/* Full-saturation RGB for a hue in degrees (0 = red). Only used to generate
 * the color list of the "Rainbow fade" script. */
uint32_t led_hue_rgb(int hue)
{
    hue = ((hue % 360) + 360) % 360;
    float h = (float)hue;
    float x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if (h < 60)        { r = 1; g = x; }
    else if (h < 120)  { r = x; g = 1; }
    else if (h < 180)  { g = 1; b = x; }
    else if (h < 240)  { g = x; b = 1; }
    else if (h < 300)  { r = x; b = 1; }
    else               { r = 1; b = x; }
    return ((uint32_t)(r * 255.0f + 0.5f) << 16) |
           ((uint32_t)(g * 255.0f + 0.5f) << 8) |
            (uint32_t)(b * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------ */
/* Set the stick rings to a fixed color. The driver latches a new
 * effect_rgb_hex only when the effect is (re)written, so writing 4 again is
 * enough even when it is already static. It must stay on static: restoring the
 * previous effect (usually 1 = linear) re-enables the animation and the rings
 * start pulsing/cycling back to the firmware's default color instead of
 * holding the one we chose. Cycles, duration, the center LED and effect_enable
 * are untouched. "lr" is the combined left+right stick zone (same one System
 * Tools writes). */
void led_color_apply(uint32_t rgb)
{
    char hex[16];
    snprintf(hex, sizeof hex, "%06X ", rgb & 0xFFFFFF);
    write_str_file(LED_SYS "effect_rgb_hex_lr", hex);
    write_int_file(LED_SYS "effect_lr", 4); /* static: latches hex, no animation */
}

/* Rewrite a single setting in overlay.cfg, leaving every other key untouched
 * (the config screen rewrites the whole file on its own). */
static void cfg_set_line(const char *drop1, const char *drop2, const char *line)
{
    char lines[64][256];
    int n = 0;
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) {
        /* Fresh install: still write the setting so the daemon can pick it up
         * and other keys fall back to their defaults. */
        snprintf(lines[n++], sizeof lines[0], "# OverLyder configuration\n");
    } else
    while (n < 64 && fgets(lines[n], sizeof lines[0], f)) {
        char key[512], *eq;
        snprintf(key, sizeof key, "%s", lines[n]);
        eq = strchr(key, '=');
        if (eq) {
            *eq = '\0';
            char *k = key;
            while (*k && isspace((unsigned char)*k))
                k++;
            if (!strcasecmp(k, drop1) || (drop2 && !strcasecmp(k, drop2)))
                continue;
        }
        n++;
    }
    if (f)
        fclose(f);

    FILE *out = fopen(CFG_PATH ".tmp", "w");
    if (!out)
        return;
    for (int i = 0; i < n; i++)
        fputs(lines[i], out);
    if (line && *line)
        fputs(line, out);
    fclose(out);
    if (rename(CFG_PATH ".tmp", CFG_PATH) == 0)
        sync();
}

/* Persist the color in overlay.cfg. When disabled the line is removed, so the
 * daemon leaves CrossMix's LED preset alone at boot. */
void cfg_update_led_color(overlay_cfg *c, uint32_t rgb, int enabled)
{
    c->led_color = rgb & 0xFFFFFF;
    c->led_color_set = enabled ? 1 : 0;

    char line[64] = "";
    if (enabled)
        snprintf(line, sizeof line, "ledcolor=%06X\n", c->led_color);
    cfg_set_line("ledcolor", "ledmode", line); /* ledmode is obsolete */
}

/* Persist the updater's "reinstall same version" switch. */
void cfg_update_reinstall(overlay_cfg *c, int enabled)
{
    c->upd_reinstall = enabled ? 1 : 0;

    char line[64];
    snprintf(line, sizeof line, "upd_reinstall=%d\n", c->upd_reinstall);
    cfg_set_line("upd_reinstall", NULL, line);
}
