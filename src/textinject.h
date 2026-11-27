#ifndef OVERLAY_TEXTINJECT_H
#define OVERLAY_TEXTINJECT_H

#include "common.h"

#define TEXT_FILE "/tmp/overlyder.keyboard.txt"

/* Best-effort delivery of the typed string to the foreground app.
 *  - always writes TEXT_FILE (a stable side channel for scripts)
 *  - replays the text as key events on the evdev node the overlay grabbed
 *    (games read the same "TRIMUI Player1" virtual device)
 *  - also creates a short-lived uinput keyboard as a fallback for apps that
 *    listen for evdev keyboards
 * Returns 0 when at least one event path accepted the events. */
int text_inject(const char *text, int gamepad_fd);
void text_save(const char *text);

/* Create the persistent uinput keyboard. The fd is intentionally NOT
 * CLOEXEC so overlay_ui (forked by overlayd) inherits it. Because the device
 * exists from daemon startup, games launched later can enumerate it. */
int keyboard_device_open(void);

#endif
