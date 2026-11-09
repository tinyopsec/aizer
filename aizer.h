/* aizer.h configuration only, data no logic. Change value, recompile.
 * Customize: colors, font, transparency (BG_ALPHA), widget size, layout,
 * prompt, cursor blink, key repeat, PATH scan (see blocks below).
 * Values from ref/launcher_improved.svg. */
#ifndef AIZER_H
#define AIZER_H

/* widget: centered rectangle (see PHILOSOPHY-RULES.md 5.1) */
#define WIDGET_W 419
#define WIDGET_H 449
#define ROW_H 26
#define MAX_ROWS 15

/* colors 0xRRGGBBAA, Obsidian Chrome Palette (ref/obsidian-chrome-palette.css):
 * onyx bg, blue slate accent/frame, alabaster selected/query text */
#define COLOR_BORDER 0x536878ffu  /* blue slate, 1px frame */
#define COLOR_BG     0x0a0a0affu  /* onyx */
#define COLOR_FG     0xe5e4e2ffu  /* alabaster grey: query + selected row */
#define COLOR_ROW    0x536878ffu  /* blue slate: unselected rows */
#define COLOR_ACCENT 0x536878ffu  /* blue slate: prompt + cursor */
#define COLOR_SEL_BG 0x53687859u  /* blue slate ~35%: selection bar */

/* background alpha, transparency of widget surface */
#define BG_ALPHA 0xf0u

/* key hold: ms before repeat starts, then compositor rate */
#define REPEAT_DELAY 150

/* list every executable from $PATH as an app */
#define LIST_PATH_EXEC 1

/* layout (widget-local px, from the reference SVG) */
#define PAD 24          /* text left edge */
#define LIST_TOP 51     /* first row rect top */
#define SEARCH_BASE 32  /* prompt baseline */
#define CURSOR_W 9
#define CURSOR_H 19
#define ROW_BASE 18     /* row text baseline from row top */

/* font (via fontconfig) */
#define FONT_NAME "monospace"
#define FONT_SIZE 16    /* prompt */
#define ROW_FONT_SIZE 14 /* list rows */

/* input */
#define PROMPT ">"      /* search line prompt text */
#define QUERY_MAX 64    /* query buffer, bytes */
#define BLINK_MS 550    /* cursor blink half period, ms, keep >= 1 */

#endif
