/* Frames of the native Linux NUI host, shared with the game under Wine.
 *
 * The host (Linux, CEF off-screen rendering) writes BGRA frames into a file of /dev/shm; the
 * game maps the same file through Z:\dev\shm\... (Wine maps it shared with the Linux side).
 * Three buffers: the host writes into one that is neither the last complete frame ("front")
 * nor the one the game is copying ("reading"), so neither side ever waits for the other.
 *
 * Plain C, compiled by both sides (gcc on Linux, MSVC/clang for the game). Kept identical to
 * wine-fivem nui-host/include/fivem_nui_shm.h. */
#ifndef FIVEM_NUI_SHM_H
#define FIVEM_NUI_SHM_H

#include <stdint.h>

#define FNUI_MAGIC    0x494e5546u   /* "FUNI" */
#define FNUI_VERSION  1u   /* fields appended inside the 4096-byte header keep version 1 */
#define FNUI_BUFFERS  3u
#define FNUI_HEADER   4096u         /* buffers start at this offset, page aligned */
#define FNUI_NONE     0xffffffffu

struct fnui_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t max_width;             /* allocated size of each buffer */
    uint32_t max_height;
    uint32_t stride;                /* bytes per row, max_width * 4 */
    uint32_t host_pid;
    volatile uint64_t seq;          /* incremented after each complete frame */
    volatile uint32_t front;        /* buffer of the last complete frame, FNUI_NONE before the first */
    volatile uint32_t reading;      /* buffer the game is copying, FNUI_NONE when idle */
    volatile uint32_t width[FNUI_BUFFERS];   /* size of the frame in each buffer */
    volatile uint32_t height[FNUI_BUFFERS];
    volatile uint32_t host_alive;   /* 1 while the host runs */
    volatile uint32_t input_port;   /* appended: TCP port on 127.0.0.1 for input (fnui_input), 0 = none */
};

/* Input, from the game to the host: a stream of fixed-size little-endian messages over TCP
 * (127.0.0.1:input_port, one client). Coordinates are in host view pixels (the game scales
 * from its resolution to max_width x max_height); modifiers are CEF EVENTFLAG_* bits. */
enum fnui_input_type
{
    FNUI_INPUT_MOUSE_MOVE  = 1,     /* x, y */
    FNUI_INPUT_MOUSE_DOWN  = 2,     /* button (0 left, 1 right, 2 middle), x, y, a = click count */
    FNUI_INPUT_MOUSE_UP    = 3,     /* same */
    FNUI_INPUT_MOUSE_WHEEL = 4,     /* x, y, a = delta x, b = delta y (120 per notch) */
    FNUI_INPUT_MOUSE_LEAVE = 5,     /* x, y */
    FNUI_INPUT_KEY         = 6,     /* button = CEF key event type (0 RAWKEYDOWN, 1 KEYDOWN, 2 KEYUP,
                                       3 CHAR), a = Windows virtual key (or the character for CHAR),
                                       b = Windows scan code | 0x100 when extended,
                                       x = character, y = unmodified character (UTF-16) */
    FNUI_INPUT_FOCUS       = 7,     /* a = 1 focused, 0 not */
};

struct fnui_input
{
    uint8_t  type;
    uint8_t  button;
    uint16_t modifiers;
    int32_t  x;
    int32_t  y;
    int32_t  a;
    int32_t  b;
};                                  /* 20 bytes */

static inline uint64_t fnui_buffer_size( const struct fnui_header *h )
{
    return (uint64_t)h->stride * h->max_height;
}

static inline uint64_t fnui_file_size( uint32_t max_width, uint32_t max_height )
{
    return FNUI_HEADER + (uint64_t)max_width * 4 * max_height * FNUI_BUFFERS;
}

static inline unsigned char *fnui_buffer( struct fnui_header *h, uint32_t index )
{
    return (unsigned char *)h + FNUI_HEADER + fnui_buffer_size( h ) * index;
}

#endif
