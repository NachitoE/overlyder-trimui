#ifndef OVERLAY_LAYER_H
#define OVERLAY_LAYER_H

#include "common.h"

/* Allwinner display2 hardware-layer backend.
 *
 * The game and MainUI draw into /dev/fb0; we do NOT touch that buffer. Instead
 * we claim an unused display channel and point it at a spare page of fb0's
 * large virtual framebuffer, then blend our canvas over the game with the
 * display engine. This avoids the flicker caused by two processes panning the
 * same framebuffer, so the game never needs SIGSTOP.
 *
 * A spare fb0 page is used instead of DISP_MEM_REQUEST: on this firmware
 * DISP_MEM_GETADR returns an address the display engine cannot safely read
 * and the device resets. */
typedef struct {
    int disp_fd;
    int fb_fd;
    unsigned char *map;      /* whole virtual framebuffer */
    size_t map_size;
    int page;                /* page index used for our layer */
    size_t page_off;
    unsigned long long phys; /* physical address of our page */
    uint32_t *pixels;        /* into map + page_off */
    unsigned channel;
    unsigned layer_id;
    int width, height;
    int stride_px;           /* fb line_length / 4 */
    int zorder;
    int alpha_mode;
    int alpha_value;
    int enabled;
} overlay_layer;

/* Cheap check used by overlayd before deciding whether to SIGSTOP the game. */
int overlay_layer_probe(void);

/* Claim a free channel and a spare fb page. 0 = success.
 * _ex lets a second consumer (the persistent HUD) prefer the other channel
 * and a different spare page. prefer_high=1 tries 3 then 2, 0 tries 2 then 3;
 * pages_from_end selects page (total_pages - N). */
int overlay_layer_open(overlay_layer *l, int width, int height);
int overlay_layer_open_ex(overlay_layer *l, int width, int height,
                          int prefer_high, int pages_from_end);

int overlay_layer_show(overlay_layer *l);
int overlay_layer_hide(overlay_layer *l);
void overlay_layer_close(overlay_layer *l);

#endif
