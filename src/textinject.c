#include "textinject.h"

#include <linux/uinput.h>

#define KB_NAME "OverLyder Virtual Keyboard"

static const unsigned short g_kb_codes[26] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
};

#define KEY_LEFTSHIFT 42

typedef struct { unsigned char ch; unsigned short code; bool shift; } keymap_t;

static const keymap_t g_map[] = {
    { ' ', 57, false },
    { '0', 11, false }, { '1', 2, false }, { '2', 3, false },
    { '3', 4, false },  { '4', 5, false }, { '5', 6, false },
    { '6', 7, false },  { '7', 8, false }, { '8', 9, false },
    { '9', 10, false },
    { 'a', 30, false }, { 'b', 48, false }, { 'c', 46, false },
    { 'd', 32, false }, { 'e', 18, false }, { 'f', 33, false },
    { 'g', 34, false }, { 'h', 35, false }, { 'i', 23, false },
    { 'j', 36, false }, { 'k', 37, false }, { 'l', 38, false },
    { 'm', 50, false }, { 'n', 49, false }, { 'o', 24, false },
    { 'p', 25, false }, { 'q', 16, false }, { 'r', 19, false },
    { 's', 31, false }, { 't', 20, false }, { 'u', 22, false },
    { 'v', 47, false }, { 'w', 17, false }, { 'x', 45, false },
    { 'y', 21, false }, { 'z', 44, false },
    { '.', 52, false }, { ',', 51, false }, { '-', 12, false },
    { '_', 12, true },  { '/', 53, false }, { ':', 39, true },
    { ';', 39, false }, { '!', 2, true },  { '?', 53, true },
    { '@', 3, true },   { '#', 4, true },  { '+', 13, true },
    { '=', 13, false }, { '(', 10, true }, { ')', 11, true },
    { '\'', 40, false }, { '"', 40, true }, { '\n', 28, false },
};
#define NMAP ((int)(sizeof g_map / sizeof g_map[0]))

static int lookup(unsigned char ch, unsigned short *code, bool *shift)
{
    for (int i = 0; i < NMAP; i++) {
        if (g_map[i].ch == ch) {
            *code = g_map[i].code;
            *shift = g_map[i].shift;
            return 1;
        }
    }
    if (ch >= 'A' && ch <= 'Z') {
        *code = g_map[ch - 'A' + 11].code;
        *shift = true;
        return 1;
    }
    return 0;
}

static void emit_key(int fd, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    (void)write(fd, &ev, sizeof ev);
    memset(&ev, 0, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    (void)write(fd, &ev, sizeof ev);
}

static void replay(int fd, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned short code;
        bool shift;
        if (!lookup(*p, &code, &shift))
            continue;
        if (fd < 0)
            continue;
        if (shift)
            emit_key(fd, KEY_LEFTSHIFT, 1);
        emit_key(fd, code, 1);
        usleep(20000);
        emit_key(fd, code, 0);
        if (shift)
            emit_key(fd, KEY_LEFTSHIFT, 0);
    }
}

static int open_uinput_keyboard(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
        close(fd);
        return -1;
    }
    for (int key = 1; key < 128; key++)
        ioctl(fd, UI_SET_KEYBIT, key);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, sizeof dev.name, "OverLyder Keyboard");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x1234;
    dev.id.product = 0x5678;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof dev) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    usleep(250000);
    return fd;
}

int keyboard_device_open(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        logm("cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
        close(fd);
        return -1;
    }
    for (unsigned i = 0; i < sizeof g_kb_codes / sizeof g_kb_codes[0]; i++)
        ioctl(fd, UI_SET_KEYBIT, g_kb_codes[i]);
    for (int c = KEY_1; c <= KEY_0; c++)
        ioctl(fd, UI_SET_KEYBIT, c);
    static const int extra[] = {
        KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT,
        KEY_SLASH, KEY_BACKSLASH, KEY_LEFTBRACE, KEY_RIGHTBRACE,
        KEY_MINUS, KEY_EQUAL, KEY_SPACE, KEY_ENTER, KEY_BACKSPACE,
        KEY_LEFTSHIFT
    };
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++)
        ioctl(fd, UI_SET_KEYBIT, extra[i]);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, sizeof dev.name, "%s", KB_NAME);
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x1234;
    dev.id.product = 0x5679;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof dev) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        logm("uinput keyboard setup failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    logm("virtual keyboard ready (fd %d)", fd);
    return fd;
}

void text_save(const char *text)
{
    FILE *f = fopen(TEXT_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", text ? text : "");
        fclose(f);
    }
}

int text_inject(const char *text, int gamepad_fd)
{
    if (!text || !*text)
        return -1;
    text_save(text);

    int ok = 0;
    /* The gamepad node is the one games actually read. Writing to an evdev
     * node injects events into every handler of that input device, including
     * a grab held by us. */
    if (gamepad_fd >= 0) {
        replay(gamepad_fd, text);
        ok = 1;
    }

    /* Some apps enumerate keyboards separately; give them a real one. */
    int ufd = open_uinput_keyboard();
    if (ufd >= 0) {
        replay(ufd, text);
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
        ok = 1;
    }

    logm("injected %d chars (%d event path(s))", (int)strlen(text), ok);
    return ok ? 0 : -1;
}
