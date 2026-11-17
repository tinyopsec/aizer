/* aizer.h configuration only, data no logic. Change value, recompile.
 * Customize: colors, font, transparency (BG_ALPHA), widget size, layout,
 * prompt, mark, cursor blink, key repeat, PATH scan.
 * Geometry from ref/launcher.svg, colors from ref/PALETTE.md. */
#ifndef AIZER_H
#define AIZER_H

/* widget: centered rectangle, no frame */
#define WIDGET_W 392
#define WIDGET_H 210
#define ROW_H 19
#define MAX_ROWS 9

/* colors 0xRRGGBBAA, palette Permafrost Dusk, see ref/PALETTE.md */
#define COLOR_BG     0x1c2230ffu  /* widget background, Abyss */
#define COLOR_FG     0xd3dae6ffu  /* query, selected row, cursor, Rime */
#define COLOR_ROW    0xa99bd0ffu  /* other rows, Dusk */
#define COLOR_ACCENT 0x7dc4b8ffu  /* prompt, Thaw */
#define COLOR_SEL_BG 0x283042ffu  /* selection bar, Slate */

/* background alpha, 0xff = opaque */
#define BG_ALPHA 0xffu

/* key hold: ms before repeat starts, then compositor rate */
#define REPEAT_DELAY 150

/* list every executable from $PATH as an app */
#define LIST_PATH_EXEC 1

/* layout, widget px */
#define PAD 28            /* text left edge */
#define LIST_TOP 31       /* first row top */
#define SEARCH_BASE 20    /* prompt baseline */
#define ROW_BASE 14       /* row text baseline from row top */
#define CURSOR_W 2
#define CURSOR_H 15
#define CURSOR_TOP 8
#define CURSOR_GAP 8      /* gap between prompt and query */
#define MARK "\xc2\xbb "  /* selected row prefix, right guillemet and space */

/* font via fontconfig */
#define FONT_NAME "Hack"
#define FONT_SIZE 13

/* input */
#define PROMPT ">"      /* search line prompt text */
#define QUERY_MAX 64    /* query buffer, bytes */
#define BLINK_MS 550    /* cursor blink half period, ms, keep >= 1 */

#endif
