/*
 * Copyright 2026, Bill Nguyen
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Framebuffer console: mirrors everything the kernel prints (boot log, printf,
 * seL4_DebugPutChar) onto the framebuffer handed over by a multiboot 1 or 2
 * boot loader. Supports linear RGB framebuffers and EGA text mode.
 *
 * Output produced before the framebuffer is known is buffered and replayed
 * once fbcon_init() is called, so the complete boot log ends up on screen.
 */

#pragma once

#include <config.h>
#include <types.h>

/* Framebuffer types, identical for multiboot 1 and 2 */
#define FBCON_TYPE_INDEXED  0
#define FBCON_TYPE_RGB      1
#define FBCON_TYPE_EGA_TEXT 2

typedef struct fbcon_info {
    uint64_t addr;      /* physical address, 0 if no framebuffer was provided */
    uint32_t pitch;     /* bytes per scanline (EGA text: bytes per text row) */
    uint32_t width;     /* pixels (EGA text: characters) */
    uint32_t height;    /* pixels (EGA text: characters) */
    uint8_t  bpp;
    uint8_t  type;      /* FBCON_TYPE_* */
    /* colour layout, only valid for FBCON_TYPE_RGB. All zero if unknown */
    uint8_t  red_pos;
    uint8_t  red_size;
    uint8_t  green_pos;
    uint8_t  green_size;
    uint8_t  blue_pos;
    uint8_t  blue_size;
} fbcon_info_t;

#ifdef CONFIG_PC99_FRAMEBUFFER_CONSOLE

/* Feed one character of kernel output into the framebuffer console. */
void fbcon_putchar(unsigned char c);

/* Take over the given framebuffer and replay all output buffered so far.
 * Returns false if the framebuffer cannot be used; the console is then off. */
bool_t fbcon_init(fbcon_info_t const *info);

/* Stop buffering and never draw anything, e.g. if there is no framebuffer. */
void fbcon_disable(void);

struct pde;

/* Called while building the kernel window: map the framebuffer into the
 * kernel device window, whose page directory is given. */
void fbcon_map_kernel_window(struct pde *kdev_pd);

/* Called once the kernel window is live. If the framebuffer could not be
 * mapped in the boot page tables, it becomes drawable from this point on. */
void fbcon_kernel_window_ready(void);

#else /* !CONFIG_PC99_FRAMEBUFFER_CONSOLE */

struct pde;

static inline void fbcon_putchar(unsigned char c) {}
static inline void fbcon_map_kernel_window(struct pde *kdev_pd) {}
static inline void fbcon_kernel_window_ready(void) {}

#endif /* CONFIG_PC99_FRAMEBUFFER_CONSOLE */
