/*
 * Copyright 2026, Bill Nguyen
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Framebuffer console for the boot loader provided (multiboot) framebuffer.
 *
 * All kernel output is fed through fbcon_putchar(). Until the boot code has
 * parsed the multiboot information the output is buffered; fbcon_init() then
 * takes over the framebuffer and replays the buffer, so the whole boot log
 * appears on screen.
 *
 * The console keeps a shadow copy of the text grid in normal RAM. This lets it
 * scroll without ever reading back from the framebuffer (slow on real
 * hardware, where the framebuffer is usually uncached) and only redraw the
 * cells whose contents actually changed. It also lets it collect output
 * while the framebuffer is not mapped yet and paint it later.
 *
 * A small subset of ANSI escape sequences is understood (SGR colours, cursor
 * positioning, erase in display/line), anything else is swallowed so it does
 * not end up as garbage on the screen.
 */

#include <config.h>

#ifdef CONFIG_PC99_FRAMEBUFFER_CONSOLE

#include <types.h>
#include <util.h>
#include <linker.h>
#include <hardware.h>
#include <machine.h>
#include <machine/io.h>
#include <object/structures.h>
#include <plat/machine/fbcon.h>
#include <plat/machine/fbcon_font.h>

/* Upper bound for the text grid, this sizes the shadow buffer (~62 KiB).
 * Enough for 2560x1600 with 8x16 glyphs; 4K screens use 16x32 glyphs. */
#define FBCON_MAX_COLS 320
#define FBCON_MAX_ROWS 100

/* Kernel output that arrives before fbcon_init(), must be a power of two. */
#define FBCON_EARLY_BUF_SIZE 8192

/* While the kernel still runs on the boot page tables, PPTR_BASE + [0, 4 GiB)
 * maps 1:1 onto physical memory (see setup_pml4 in arch/x86/64/head.S). That
 * is how the EGA text buffer is accessed. */
#define FBCON_BOOT_MAP_TOP (UL_CONST(1) << 32)

/* Pixel framebuffers get a mapping of their own in the kernel device window,
 * both in the boot page tables and in the kernel's. This works wherever the
 * framebuffer is (with a discrete GPU it is often far above 4 GiB, possibly
 * beyond the kernel window) and maps it uncached (PAT UC-, write-combining if
 * the MTRRs say so) instead of relying on the MTRRs to keep it out of the
 * cache. Device window PD slots 0 and 1 hold the kernel device PT and the user
 * log buffer, the last one the benchmark log, 256 to 510 are free. */
#define FBCON_PD_FIRST  256
#define FBCON_PD_COUNT  255
#define FBCON_VADDR     (KDEV_BASE + FBCON_PD_FIRST * BIT(seL4_LargePageBits))

/* x86 paging structure entry bits */
#define FBCON_PTE_PRESENT   BIT(0)
#define FBCON_PTE_RW        BIT(1)
#define FBCON_PTE_PCD       BIT(4)
#define FBCON_PTE_PS        BIT(7)
#define FBCON_PTE_ADDR_MASK UL_CONST(0x000ffffffffff000)

/* Page directory for the framebuffer in the boot page tables, these are only
 * used until the kernel window is up. */
static uint64_t fbcon_boot_pd[BIT(PD_INDEX_BITS)] ALIGN(BIT(seL4_PageBits)) BOOT_BSS;

/* Cells are a character in the low byte and an attribute in the high byte.
 * Attributes are foreground in the low nibble, background in the high nibble,
 * both as ANSI colour indices (bit 3 = bright). */
#define FBCON_ATTR(fg, bg)    ((uint8_t)(((bg) << 4) | (fg)))
#define FBCON_DEFAULT_ATTR    FBCON_ATTR(7, 0)
#define FBCON_CELL(ch, attr)  ((uint16_t)((uint16_t)(ch) | ((uint16_t)(attr) << 8)))
#define FBCON_BLANK           FBCON_CELL(' ', FBCON_DEFAULT_ATTR)

#define FBCON_MAX_PARAMS 8
#define FBCON_TAB_WIDTH  8

typedef enum {
    FBCON_EARLY = 0, /* framebuffer not known yet, buffer output (BSS default) */
    FBCON_ON,
    FBCON_OFF
} fbcon_state_t;

typedef enum {
    FBCON_ESC_NONE = 0,
    FBCON_ESC_ESC,   /* got ESC */
    FBCON_ESC_CSI    /* got ESC [ */
} fbcon_esc_state_t;

typedef struct fbcon {
    fbcon_state_t state;
    bool_t drawable;        /* framebuffer is mapped and may be written to */

    paddr_t paddr;
    word_t size;
    paddr_t map_base;       /* 2 MiB aligned start of the mapping at FBCON_VADDR */
    word_t map_pages;       /* number of 2 MiB pages mapped, 0 for EGA text */
    uint8_t *base;          /* kernel virtual address of the framebuffer */
    word_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t type;
    uint8_t bytespp;
    uint32_t scale;         /* glyph magnification for high resolution screens */
    uint32_t palette[16];   /* pixel value for every ANSI colour index */

    uint32_t cols;
    uint32_t rows;
    uint32_t cx;            /* cursor column, may be == cols (pending wrap) */
    uint32_t cy;            /* cursor row */
    uint8_t attr;           /* attribute for new characters */

    fbcon_esc_state_t esc;
    uint32_t nparams;
    uint32_t params[FBCON_MAX_PARAMS];
} fbcon_t;

static fbcon_t fbcon;
static uint16_t fbcon_cells[FBCON_MAX_ROWS][FBCON_MAX_COLS];
static unsigned char fbcon_early_buf[FBCON_EARLY_BUF_SIZE];
static word_t fbcon_early_len;

/* The classic VGA palette, in ANSI colour order. */
static const uint32_t fbcon_rgb[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
    0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

/* ANSI colour index -> VGA text mode colour index (low 3 bits only). */
static const uint8_t fbcon_ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/*
 *------------------------------------------------------------------------------
 * Drawing
 *------------------------------------------------------------------------------
 */

static uint32_t fbcon_channel(uint32_t value8, uint8_t size, uint8_t pos)
{
    if (size == 0) {
        return 0;
    }
    if (size <= 8) {
        value8 >>= 8 - size;
    } else {
        value8 <<= size - 8;
    }
    return value8 << pos;
}

static uint32_t fbcon_rgb_to_pixel(uint32_t rgb, fbcon_info_t const *info)
{
    return fbcon_channel((rgb >> 16) & 0xff, info->red_size, info->red_pos) |
           fbcon_channel((rgb >> 8) & 0xff, info->green_size, info->green_pos) |
           fbcon_channel(rgb & 0xff, info->blue_size, info->blue_pos);
}

static inline void fbcon_put_pixel(uint8_t *p, uint32_t px)
{
    switch (fbcon.bytespp) {
    case 4:
        *(volatile uint32_t *)p = px;
        break;
    case 3:
        ((volatile uint8_t *)p)[0] = px;
        ((volatile uint8_t *)p)[1] = px >> 8;
        ((volatile uint8_t *)p)[2] = px >> 16;
        break;
    case 2:
        *(volatile uint16_t *)p = px;
        break;
    default:
        *(volatile uint8_t *)p = px;
        break;
    }
}

static void fbcon_draw_cell(uint32_t col, uint32_t row, uint16_t cell)
{
    uint8_t ch = cell & 0xff;
    uint8_t attr = cell >> 8;

    if (!fbcon.drawable || col >= fbcon.cols || row >= fbcon.rows) {
        return;
    }
    if (ch < FBCON_FONT_FIRST || ch > FBCON_FONT_LAST) {
        ch = '?';
    }

    if (fbcon.type == FBCON_TYPE_EGA_TEXT) {
        uint8_t fg = fbcon_ansi_to_vga[attr & 0x7] | (attr & 0x8);
        /* bit 3 of the background is 'blink' on most hardware, drop it */
        uint8_t bg = fbcon_ansi_to_vga[(attr >> 4) & 0x7];
        *(volatile uint16_t *)(fbcon.base + row * fbcon.pitch + col * 2) =
            (uint16_t)ch | ((uint16_t)((bg << 4) | fg) << 8);
        return;
    }

    uint8_t const *glyph = fbcon_font[ch - FBCON_FONT_FIRST];
    uint32_t fg = fbcon.palette[attr & 0xf];
    uint32_t bg = fbcon.palette[attr >> 4];
    uint32_t scale = fbcon.scale;
    uint8_t *line = fbcon.base
                    + (word_t)row * FBCON_FONT_HEIGHT * scale * fbcon.pitch
                    + (word_t)col * FBCON_FONT_WIDTH * scale * fbcon.bytespp;

    for (uint32_t y = 0; y < FBCON_FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t sy = 0; sy < scale; sy++, line += fbcon.pitch) {
            if (fbcon.bytespp == 4 && scale == 1) {
                /* common case: 32bpp, two pixels per store */
                volatile uint64_t *q = (volatile uint64_t *)line;
                for (uint32_t x = 0; x < FBCON_FONT_WIDTH; x += 2) {
                    uint64_t left = (bits & (0x80 >> x)) ? fg : bg;
                    uint64_t right = (bits & (0x40 >> x)) ? fg : bg;
                    q[x / 2] = left | (right << 32);
                }
            } else {
                uint8_t *p = line;
                for (uint32_t x = 0; x < FBCON_FONT_WIDTH; x++) {
                    uint32_t px = (bits & (0x80 >> x)) ? fg : bg;
                    for (uint32_t sx = 0; sx < scale; sx++, p += fbcon.bytespp) {
                        fbcon_put_pixel(p, px);
                    }
                }
            }
        }
    }
}

/* Paint the whole framebuffer (including the margin outside the text grid)
 * in the default background colour. */
static void fbcon_clear_framebuffer(void)
{
    if (!fbcon.drawable) {
        return;
    }
    if (fbcon.type == FBCON_TYPE_EGA_TEXT) {
        for (uint32_t r = 0; r < fbcon.rows; r++) {
            for (uint32_t c = 0; c < fbcon.cols; c++) {
                fbcon_draw_cell(c, r, FBCON_BLANK);
            }
        }
        return;
    }
    uint32_t bg = fbcon.palette[FBCON_DEFAULT_ATTR >> 4];
    for (uint32_t y = 0; y < fbcon.height; y++) {
        uint8_t *p = fbcon.base + (word_t)y * fbcon.pitch;
        for (uint32_t x = 0; x < fbcon.width; x++, p += fbcon.bytespp) {
            fbcon_put_pixel(p, bg);
        }
    }
}

static void fbcon_redraw_all(void)
{
    fbcon_clear_framebuffer();
    for (uint32_t r = 0; r < fbcon.rows; r++) {
        for (uint32_t c = 0; c < fbcon.cols; c++) {
            if (fbcon_cells[r][c] != FBCON_BLANK) {
                fbcon_draw_cell(c, r, fbcon_cells[r][c]);
            }
        }
    }
}

/*
 *------------------------------------------------------------------------------
 * Text grid
 *------------------------------------------------------------------------------
 */

/* Update a cell in the shadow buffer and draw it if it changed. */
static void fbcon_set_cell(uint32_t col, uint32_t row, uint16_t cell)
{
    if (col >= fbcon.cols || row >= fbcon.rows) {
        return;
    }
    if (fbcon_cells[row][col] != cell) {
        fbcon_cells[row][col] = cell;
        fbcon_draw_cell(col, row, cell);
    }
}

/* Blank cells in the linear range [from, to) of the grid. */
static void fbcon_erase(word_t from, word_t to)
{
    for (word_t i = from; i < to; i++) {
        fbcon_set_cell(i % fbcon.cols, i / fbcon.cols, FBCON_BLANK);
    }
}

static void fbcon_scroll(void)
{
    for (uint32_t r = 0; r + 1 < fbcon.rows; r++) {
        for (uint32_t c = 0; c < fbcon.cols; c++) {
            fbcon_set_cell(c, r, fbcon_cells[r + 1][c]);
        }
    }
    fbcon_erase((word_t)(fbcon.rows - 1) * fbcon.cols, (word_t)fbcon.rows * fbcon.cols);
}

static void fbcon_newline(void)
{
    fbcon.cx = 0;
    if (fbcon.cy + 1 >= fbcon.rows) {
        fbcon.cy = fbcon.rows - 1;
        fbcon_scroll();
    } else {
        fbcon.cy++;
    }
}

static void fbcon_print(unsigned char c)
{
    if (fbcon.cx >= fbcon.cols) {
        fbcon_newline();
    }
    fbcon_set_cell(fbcon.cx, fbcon.cy, FBCON_CELL(c, fbcon.attr));
    fbcon.cx++;
}

/*
 *------------------------------------------------------------------------------
 * ANSI escape sequences
 *------------------------------------------------------------------------------
 */

static uint32_t fbcon_param(uint32_t i, uint32_t def)
{
    return (i < fbcon.nparams && fbcon.params[i] != 0) ? fbcon.params[i] : def;
}

static void fbcon_sgr(void)
{
    uint8_t fg = fbcon.attr & 0xf;
    uint8_t bg = fbcon.attr >> 4;

    if (fbcon.nparams == 0) {
        fbcon.attr = FBCON_DEFAULT_ATTR;
        return;
    }
    for (uint32_t i = 0; i < fbcon.nparams; i++) {
        uint32_t p = fbcon.params[i];
        if (p == 0) {
            fg = FBCON_DEFAULT_ATTR & 0xf;
            bg = FBCON_DEFAULT_ATTR >> 4;
        } else if (p == 1) {
            fg |= 0x8;
        } else if (p == 22) {
            fg &= 0x7;
        } else if (p >= 30 && p <= 37) {
            fg = (fg & 0x8) | (p - 30);
        } else if (p == 39) {
            fg = (fg & 0x8) | (FBCON_DEFAULT_ATTR & 0x7);
        } else if (p >= 40 && p <= 47) {
            bg = p - 40;
        } else if (p == 49) {
            bg = FBCON_DEFAULT_ATTR >> 4;
        } else if (p >= 90 && p <= 97) {
            fg = 0x8 | (p - 90);
        } else if (p >= 100 && p <= 107) {
            bg = 0x8 | (p - 100);
        } else if (p == 38 || p == 48) {
            /* 256 colour / true colour, the remaining parameters belong to it */
            break;
        }
    }
    fbcon.attr = FBCON_ATTR(fg, bg);
}

static void fbcon_csi(unsigned char final)
{
    word_t cursor = (word_t)fbcon.cy * fbcon.cols + MIN(fbcon.cx, fbcon.cols - 1);
    word_t cells = (word_t)fbcon.rows * fbcon.cols;
    word_t line = (word_t)fbcon.cy * fbcon.cols;
    uint32_t n = fbcon_param(0, 1);

    switch (final) {
    case 'm':
        fbcon_sgr();
        break;
    case 'H':
    case 'f':
        fbcon.cy = MIN(fbcon_param(0, 1), fbcon.rows) - 1;
        fbcon.cx = MIN(fbcon_param(1, 1), fbcon.cols) - 1;
        break;
    case 'A':
        fbcon.cy = fbcon.cy >= n ? fbcon.cy - n : 0;
        break;
    case 'B':
        fbcon.cy = MIN(fbcon.cy + n, fbcon.rows - 1);
        break;
    case 'C':
        fbcon.cx = MIN(fbcon.cx + n, fbcon.cols - 1);
        break;
    case 'D':
        fbcon.cx = MIN(fbcon.cx, fbcon.cols - 1);
        fbcon.cx = fbcon.cx >= n ? fbcon.cx - n : 0;
        break;
    case 'J':
        switch (fbcon_param(0, 0)) {
        case 0:
            fbcon_erase(cursor, cells);
            break;
        case 1:
            fbcon_erase(0, cursor + 1);
            break;
        default:
            fbcon_erase(0, cells);
            break;
        }
        break;
    case 'K':
        switch (fbcon_param(0, 0)) {
        case 0:
            fbcon_erase(cursor, line + fbcon.cols);
            break;
        case 1:
            fbcon_erase(line, cursor + 1);
            break;
        default:
            fbcon_erase(line, line + fbcon.cols);
            break;
        }
        break;
    default:
        /* not supported, swallow it */
        break;
    }
}

static void fbcon_process(unsigned char c)
{
    switch (fbcon.esc) {
    case FBCON_ESC_ESC:
        if (c == '[') {
            fbcon.esc = FBCON_ESC_CSI;
            fbcon.nparams = 0;
            fbcon.params[0] = 0;
        } else {
            /* other escape sequences are not supported, drop them */
            fbcon.esc = FBCON_ESC_NONE;
        }
        return;

    case FBCON_ESC_CSI:
        if (c >= '0' && c <= '9') {
            if (fbcon.nparams == 0) {
                fbcon.nparams = 1;
            }
            uint32_t *p = &fbcon.params[fbcon.nparams - 1];
            if (*p < 10000) {
                *p = *p * 10 + (c - '0');
            }
        } else if (c == ';') {
            if (fbcon.nparams == 0) {
                fbcon.nparams = 1;
            }
            if (fbcon.nparams < FBCON_MAX_PARAMS) {
                fbcon.params[fbcon.nparams++] = 0;
            }
        } else if (c >= 0x40 && c <= 0x7e) {
            fbcon_csi(c);
            fbcon.esc = FBCON_ESC_NONE;
        } else if (c < 0x20 || c > 0x3f) {
            /* not a parameter or intermediate byte: malformed, give up */
            fbcon.esc = FBCON_ESC_NONE;
        }
        return;

    default:
        break;
    }

    switch (c) {
    case 0x1b:
        fbcon.esc = FBCON_ESC_ESC;
        break;
    case '\n':
        fbcon_newline();
        break;
    case '\r':
        fbcon.cx = 0;
        break;
    case '\b':
        fbcon.cx = MIN(fbcon.cx, fbcon.cols - 1);
        if (fbcon.cx > 0) {
            fbcon.cx--;
        }
        break;
    case '\t':
        do {
            fbcon_print(' ');
        } while (fbcon.cx % FBCON_TAB_WIDTH != 0 && fbcon.cx < fbcon.cols);
        break;
    default:
        if (c >= 0x20) {
            fbcon_print(c);
        }
        /* other control characters are ignored */
        break;
    }
}

/*
 *------------------------------------------------------------------------------
 * Interface
 *------------------------------------------------------------------------------
 */

void fbcon_putchar(unsigned char c)
{
    switch (fbcon.state) {
    case FBCON_ON:
        fbcon_process(c);
        break;
    case FBCON_EARLY:
        fbcon_early_buf[fbcon_early_len % FBCON_EARLY_BUF_SIZE] = c;
        fbcon_early_len++;
        break;
    default:
        break;
    }
}

void fbcon_disable(void)
{
    fbcon.state = FBCON_OFF;
    fbcon.drawable = false;
    fbcon_early_len = 0;
}

static bool_t fbcon_setup_rgb(fbcon_info_t const *info)
{
    fbcon_info_t layout = *info;

    if (info->bpp != 8 && info->bpp != 15 && info->bpp != 16 && info->bpp != 24 && info->bpp != 32) {
        printf("fbcon: unsupported RGB framebuffer depth %u\n", (unsigned int)info->bpp);
        return false;
    }
    fbcon.bytespp = (info->bpp + 7) / 8;

    if (!layout.red_size && !layout.green_size && !layout.blue_size) {
        /* boot loader did not tell us, assume the usual layout */
        switch (info->bpp) {
        case 8:
            layout.red_pos = 5;
            layout.red_size = 3;
            layout.green_pos = 2;
            layout.green_size = 3;
            layout.blue_pos = 0;
            layout.blue_size = 2;
            break;
        case 15:
            layout.red_pos = 10;
            layout.red_size = 5;
            layout.green_pos = 5;
            layout.green_size = 5;
            layout.blue_pos = 0;
            layout.blue_size = 5;
            break;
        case 16:
            layout.red_pos = 11;
            layout.red_size = 5;
            layout.green_pos = 5;
            layout.green_size = 6;
            layout.blue_pos = 0;
            layout.blue_size = 5;
            break;
        default:
            layout.red_pos = 16;
            layout.red_size = 8;
            layout.green_pos = 8;
            layout.green_size = 8;
            layout.blue_pos = 0;
            layout.blue_size = 8;
            break;
        }
    }
    if (layout.red_pos + layout.red_size > fbcon.bytespp * 8 ||
        layout.green_pos + layout.green_size > fbcon.bytespp * 8 ||
        layout.blue_pos + layout.blue_size > fbcon.bytespp * 8) {
        printf("fbcon: invalid colour layout\n");
        return false;
    }
    if (info->width < FBCON_FONT_WIDTH || info->height < FBCON_FONT_HEIGHT ||
        info->pitch < (word_t)info->width * fbcon.bytespp) {
        printf("fbcon: invalid framebuffer geometry\n");
        return false;
    }

    for (int i = 0; i < ARRAY_SIZE(fbcon_rgb); i++) {
        fbcon.palette[i] = fbcon_rgb_to_pixel(fbcon_rgb[i], &layout);
    }

    /* keep text readable on high resolution screens */
    fbcon.scale = MAX(1, MIN(info->width / 1600, info->height / 900));
    fbcon.cols = MIN(info->width / (FBCON_FONT_WIDTH * fbcon.scale), FBCON_MAX_COLS);
    fbcon.rows = MIN(info->height / (FBCON_FONT_HEIGHT * fbcon.scale), FBCON_MAX_ROWS);
    return true;
}

static bool_t fbcon_setup_ega_text(fbcon_info_t const *info)
{
    if (info->width == 0 || info->height == 0 || info->pitch < (word_t)info->width * 2) {
        printf("fbcon: invalid text mode geometry\n");
        return false;
    }
    fbcon.bytespp = 2;
    fbcon.scale = 1;
    fbcon.cols = MIN(info->width, FBCON_MAX_COLS);
    fbcon.rows = MIN(info->height, FBCON_MAX_ROWS);
    return true;
}

/* Map the framebuffer at FBCON_VADDR in the boot page tables (see head.S),
 * which are still active at this point and map the first 4 GiB 1:1. */
static bool_t fbcon_map_boot(void)
{
    uint64_t cr3;
    uint64_t *pml4;
    uint64_t *pdpt;
    word_t pml4_index = GET_PML4_INDEX(FBCON_VADDR);
    word_t pdpt_index = GET_PDPT_INDEX(FBCON_VADDR);

    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    pml4 = (uint64_t *)(word_t)(cr3 & FBCON_PTE_ADDR_MASK);
    if (!(pml4[pml4_index] & FBCON_PTE_PRESENT)) {
        return false;
    }
    pdpt = (uint64_t *)(word_t)(pml4[pml4_index] & FBCON_PTE_ADDR_MASK);
    if (pdpt[pdpt_index] != 0) {
        /* the boot page tables use this slot after all, leave them alone */
        return false;
    }

    for (word_t i = 0; i < fbcon.map_pages; i++) {
        fbcon_boot_pd[FBCON_PD_FIRST + i] = (fbcon.map_base + i * BIT(seL4_LargePageBits)) |
                                            FBCON_PTE_PS | FBCON_PTE_PCD | FBCON_PTE_RW | FBCON_PTE_PRESENT;
    }
    pdpt[pdpt_index] = kpptr_to_paddr(fbcon_boot_pd) | FBCON_PTE_RW | FBCON_PTE_PRESENT;
    /* flush the TLB */
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    return true;
}

void fbcon_map_kernel_window(struct pde *kdev_pd)
{
    if (fbcon.state != FBCON_ON || fbcon.map_pages == 0) {
        return;
    }
    for (word_t i = 0; i < fbcon.map_pages; i++) {
        kdev_pd[FBCON_PD_FIRST + i] = pde_pde_large_new(
                                          0,  /* xd */
                                          fbcon.map_base + i * BIT(seL4_LargePageBits),
                                          0,  /* pat */
                                          0,  /* global */
                                          0,  /* dirty */
                                          0,  /* accessed */
                                          1,  /* cache_disabled, i.e. UC- */
                                          0,  /* write_through */
                                          0,  /* super_user */
                                          1,  /* read_write */
                                          1   /* present */
                                      );
    }
}

bool_t fbcon_init(fbcon_info_t const *info)
{
    bool_t ok;

    if (fbcon.state != FBCON_EARLY) {
        return fbcon.state == FBCON_ON;
    }

    switch (info->type) {
    case FBCON_TYPE_RGB:
        ok = fbcon_setup_rgb(info);
        break;
    case FBCON_TYPE_EGA_TEXT:
        ok = fbcon_setup_ega_text(info);
        break;
    default:
        printf("fbcon: framebuffer type %u is not supported\n", (unsigned int)info->type);
        ok = false;
        break;
    }

    fbcon.paddr = info->addr;
    fbcon.size = (word_t)info->pitch * info->height;
    if (ok && (fbcon.paddr == 0 || fbcon.paddr + fbcon.size < fbcon.paddr)) {
        printf("fbcon: invalid framebuffer address 0x%lx\n", fbcon.paddr);
        ok = false;
    }
    if (ok && info->type == FBCON_TYPE_EGA_TEXT) {
        /* accessed through the physical memory window */
        if (fbcon.paddr + fbcon.size > FBCON_BOOT_MAP_TOP) {
            printf("fbcon: text mode buffer at 0x%lx is not supported\n", fbcon.paddr);
            ok = false;
        }
        fbcon.map_pages = 0;
        fbcon.base = ptrFromPAddr(fbcon.paddr);
    } else if (ok) {
        fbcon.map_base = ROUND_DOWN(fbcon.paddr, seL4_LargePageBits);
        fbcon.map_pages = ROUND_UP(fbcon.paddr + fbcon.size, seL4_LargePageBits) - fbcon.map_base;
        fbcon.map_pages >>= seL4_LargePageBits;
        if (fbcon.map_pages > FBCON_PD_COUNT) {
            printf("fbcon: framebuffer is too big (%lu bytes)\n", fbcon.size);
            ok = false;
        }
        fbcon.base = (uint8_t *)(FBCON_VADDR + (fbcon.paddr - fbcon.map_base));
    }
    if (!ok) {
        fbcon_disable();
        return false;
    }

    fbcon.pitch = info->pitch;
    fbcon.width = info->width;
    fbcon.height = info->height;
    fbcon.type = info->type;
    /* if the boot page tables cannot take the mapping, output is shown once
     * the kernel window is up, see fbcon_kernel_window_ready() */
    fbcon.drawable = fbcon.map_pages == 0 || fbcon_map_boot();
    fbcon.attr = FBCON_DEFAULT_ATTR;
    fbcon.cx = 0;
    fbcon.cy = 0;
    fbcon.esc = FBCON_ESC_NONE;
    for (uint32_t r = 0; r < FBCON_MAX_ROWS; r++) {
        for (uint32_t c = 0; c < FBCON_MAX_COLS; c++) {
            fbcon_cells[r][c] = FBCON_BLANK;
        }
    }
    fbcon_clear_framebuffer();

    /* replay what was printed so far; if the buffer overflowed, start at the
     * first complete line that is still in it */
    word_t start = 0;
    if (fbcon_early_len > FBCON_EARLY_BUF_SIZE) {
        start = fbcon_early_len - FBCON_EARLY_BUF_SIZE;
        while (start < fbcon_early_len &&
               fbcon_early_buf[start % FBCON_EARLY_BUF_SIZE] != '\n') {
            start++;
        }
        start++;
    }
    for (word_t i = start; i < fbcon_early_len; i++) {
        fbcon_process(fbcon_early_buf[i % FBCON_EARLY_BUF_SIZE]);
    }
    fbcon_early_len = 0;
    fbcon.state = FBCON_ON;

    printf("fbcon: framebuffer at 0x%lx, %ux%u %s, %ux%u characters%s\n",
           fbcon.paddr, fbcon.width, fbcon.height,
           fbcon.type == FBCON_TYPE_EGA_TEXT ? "text mode" : "pixels",
           fbcon.cols, fbcon.rows,
           fbcon.drawable ? "" : " (not mapped yet, shown once the kernel window is up)");
    return true;
}

void fbcon_kernel_window_ready(void)
{
    if (fbcon.state != FBCON_ON || fbcon.drawable) {
        return;
    }
    /* fbcon_map_kernel_window() has put the mapping into the kernel window */
    fbcon.drawable = true;
    fbcon_redraw_all();
}

#endif /* CONFIG_PC99_FRAMEBUFFER_CONSOLE */
