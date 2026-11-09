/* aizer is minimal Wayland app launcher, centered rectangle widget.
 * C23, deps: libc, wayland-client, xkbcommon, freetype, fontconfig. */
#define _GNU_SOURCE
#include "aizer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

struct app {
	char name[96];
	char gen[96];
	char id[128]; /* desktop file id, dedup key */
	char *exec;
};

static struct app *apps;
static int napps, capapps;
static int *match;
static int nmatch, sel;
static char query[QUERY_MAX];

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer;
static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *lfsurf;
static struct wl_keyboard *kb;
static struct wl_buffer *buf;
static uint32_t *px;
static struct xkb_context *xkbctx;
static struct xkb_keymap *xkbmap;
static struct xkb_state *xkbstate;
static int running = 1, configured, need_redraw, blink = 1, busy;
static uint32_t rep_key;   /* held keycode for repeat, 0 = none */
static uint64_t rep_at;    /* monotonic ms of next repeat */
static int rep_rate = 25;  /* chars/sec, from wl_keyboard repeat_info */

static const struct wl_seat_listener seat_listener;

static FT_Library ftlib;
static FT_Face face;
static int asc;      /* baseline from line top, px */
static int font_px;  /* current pixel size */

static void
pset(int x, int y, uint32_t c) /* c = 0xRRGGBBAA */
{
	uint32_t r, g, b, a;
	if (x < 0 || y < 0 || x >= WIDGET_W || y >= WIDGET_H)
		return;
	r = (c >> 24) & 0xff;
	g = (c >> 16) & 0xff;
	b = (c >> 8) & 0xff;
	a = c & 0xff;
	px[(size_t)y * WIDGET_W + x] = (a << 24) | (r << 16) | (g << 8) | b;
}

static void
rect(int x, int y, int w, int h, uint32_t c)
{
	int i, j;
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			pset(i, j, c);
}

/* blend glyph coverage over existing pixel, keep buffer alpha */
static void
blend(int x, int y, uint32_t c, unsigned char cov)
{
	uint32_t d;
	unsigned r, g, b, dr, dg, db;
	if (!cov || x < 0 || y < 0 || x >= WIDGET_W || y >= WIDGET_H)
		return;
	r = (c >> 24) & 0xff;
	g = (c >> 16) & 0xff;
	b = (c >> 8) & 0xff;
	d = px[(size_t)y * WIDGET_W + x];
	dr = (d >> 16) & 0xff;
	dg = (d >> 8) & 0xff;
	db = d & 0xff;
	dr = (r * cov + dr * (255 - cov)) / 255;
	dg = (g * cov + dg * (255 - cov)) / 255;
	db = (b * cov + db * (255 - cov)) / 255;
	px[(size_t)y * WIDGET_W + x] = (d & 0xff000000u) | (dr << 16) |
	    (dg << 8) | db;
}

static const char *
utf8_next(const char *s, uint32_t *cp)
{
	unsigned char c = (unsigned char)*s;
	if (c < 0x80) {
		*cp = c;
		return s + 1;
	}
	if ((c & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(c & 0x1f) << 6) | (s[1] & 0x3f);
		return s + 2;
	}
	if ((c & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) |
		    (s[2] & 0x3f);
		return s + 3;
	}
	if ((c & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
	    (s[3] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
		    ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
		return s + 4;
	}
	*cp = '?';
	return s + 1;
}

static int
draw_char(int x, int ytop, uint32_t cp, uint32_t c)
{
	FT_Bitmap *b;
	int bx, by, row, col;
	unsigned char *p;
	if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
		return font_px / 2;
	b = &face->glyph->bitmap;
	bx = x + face->glyph->bitmap_left;
	by = ytop + asc - face->glyph->bitmap_top;
	for (row = 0; row < (int)b->rows; row++) {
		p = b->buffer + row * b->pitch;
		for (col = 0; col < (int)b->width; col++)
			if (p[col])
				blend(bx + col, by + row, c, p[col]);
	}
	return (int)(face->glyph->advance.x >> 6);
}

/* draw utf8 string, stop at lim (px); returns pen x */
static int
draw_str(int x, int ytop, const char *s, uint32_t c, int lim)
{
	uint32_t cp;
	const char *n;
	while (*s && x < lim) {
		n = utf8_next(s, &cp);
		x += draw_char(x, ytop, cp, c);
		s = n;
	}
	return x;
}

static int
str_width(const char *s)
{
	int w = 0;
	uint32_t cp;
	const char *n;
	while (*s) {
		n = utf8_next(s, &cp);
		if (!FT_Load_Char(face, cp, FT_LOAD_RENDER))
			w += (int)(face->glyph->advance.x >> 6);
		else
			w += font_px / 2;
		s = n;
	}
	return w;
}

static int
load_font(void)
{
	FcPattern *pat, *fcm;
	FcChar8 *file = NULL;
	int idx = 0;
	FcResult res;
	if (FT_Init_FreeType(&ftlib))
		return -1;
	if (!FcInit())
		return -1;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)FONT_NAME);
	FcPatternAddDouble(pat, FC_SIZE, (double)FONT_SIZE);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	fcm = FcFontMatch(NULL, pat, &res);
	FcPatternDestroy(pat);
	if (!fcm)
		return -1;
	if (FcPatternGetString(fcm, FC_FILE, 0, &file) != FcResultMatch) {
		FcPatternDestroy(fcm);
		return -1;
	}
	FcPatternGetInteger(fcm, FC_INDEX, 0, &idx);
	if (FT_New_Face(ftlib, (const char *)file, idx, &face)) {
		FcPatternDestroy(fcm);
		return -1;
	}
	FcPatternDestroy(fcm);
	if (FT_Set_Pixel_Sizes(face, 0, FONT_SIZE))
		return -1;
	asc = face->size->metrics.ascender >> 6;
	font_px = FONT_SIZE;
	return 0;
}

/* switch active pixel size (prompt 16 vs rows 14) */
static void
set_font(int sz)
{
	font_px = sz;
	FT_Set_Pixel_Sizes(face, 0, sz);
	asc = face->size->metrics.ascender >> 6;
}

static char
low(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int
ci_has(const char *h, const char *n)
{
	for (; *h; h++) {
		const char *a = h, *b = n;
		while (*a && *b && low(*a) == low(*b)) {
			a++;
			b++;
		}
		if (!*b)
			return 1;
	}
	return 0;
}

/* fuzzy subsequence: every needle char appears in order (case-insensitive).
 * "ffx" finds Firefox */
static int
ci_sub(const char *h, const char *n)
{
	for (; *n; n++) {
		while (*h && low(*h) != low(*n))
			h++;
		if (!*h)
			return 0;
		h++;
	}
	return 1;
}

/* all space-separated tokens must match name or GenericName.
 * *exact = 1 when every token is a plain substring (better rank). */
static int
app_match(const struct app *a, int *exact)
{
	char q[sizeof query], *t;
	int sub = 1, fuzz = 1;
	if (!query[0]) {
		*exact = 1;
		return 1;
	}
	snprintf(q, sizeof q, "%s", query);
	for (t = strtok(q, " "); t; t = strtok(NULL, " ")) {
		int hs = ci_has(a->name, t) ||
		    (a->gen[0] && ci_has(a->gen, t));
		int hf = ci_sub(a->name, t) ||
		    (a->gen[0] && ci_sub(a->gen, t));
		if (!hs)
			sub = 0;
		if (!hf)
			fuzz = 0;
	}
	*exact = sub;
	return sub || fuzz;
}

/* exact substring matches first, then fuzzy; alphabetical inside each */
static void
refilter(void)
{
	int i, ex;
	nmatch = 0;
	sel = 0;
	for (i = 0; i < napps; i++)
		if (app_match(&apps[i], &ex) && ex)
			match[nmatch++] = i;
	for (i = 0; i < napps; i++)
		if (app_match(&apps[i], &ex) && !ex)
			match[nmatch++] = i;
}

static void
clean_exec(char *e)
{
	char *w = e, *r;
	for (r = e; *r; r++) {
		if (*r == '%') {
			if (r[1] == 0)
				break;
			if (r[1] == '%')
				*w++ = '%';
			r++;
			continue;
		}
		*w++ = *r;
	}
	*w = 0;
}

/* desktop id: base dir plus name foo.desktop gives dir-foo.desktop */
static void
make_id(char *dst, size_t cap, const char *base, const char *name)
{
	if (base)
		snprintf(dst, cap, "%s-%s", base, name);
	else
		snprintf(dst, cap, "%s", name);
}

/* absolute path checked directly, else each PATH dir */
static int
have_bin(const char *bin)
{
	char *copy, *p, full[1024];
	const char *path;
	if (bin[0] == '/')
		return access(bin, X_OK) == 0;
	path = getenv("PATH");
	if (!path)
		return 0;
	copy = strdup(path);
	if (!copy)
		return 0;
	for (p = strtok(copy, ":"); p; p = strtok(NULL, ":")) {
		snprintf(full, sizeof full, "%s/%s", p, bin);
		if (access(full, X_OK) == 0) {
			free(copy);
			return 1;
		}
	}
	free(copy);
	return 0;
}

static void
add_app(const char *name, const char *gen, char *exe, const char *id)
{
	struct app *a;
	if (napps == capapps) {
		capapps = capapps ? 2 * capapps : 128;
		apps = realloc(apps, (size_t)capapps * sizeof *apps);
		if (!apps) {
			perror("aizer: realloc");
			exit(1);
		}
	}
	a = &apps[napps++];
	snprintf(a->name, sizeof a->name, "%s", name);
	snprintf(a->gen, sizeof a->gen, "%s", gen);
	snprintf(a->id, sizeof a->id, "%s", id);
	a->exec = exe;
}

static void
parse_desktop(const char *path, const char *id)
{
	FILE *f = fopen(path, "r");
	char line[512], name[96] = "", gen[96] = "";
	char *exe = NULL, *eq, *v;
	int group = 0, skip = 0, type_ok = 1;
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '[') {
			group = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!group || line[0] == '#')
			continue;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		v = eq + 1;
		if (!strcmp(line, "Type"))
			type_ok = !strcmp(v, "Application");
		else if (!strcmp(line, "Name") && !name[0])
			snprintf(name, sizeof name, "%s", v);
		else if (!strcmp(line, "GenericName") && !gen[0])
			snprintf(gen, sizeof gen, "%s", v);
		else if (!strcmp(line, "Exec") && !exe)
			exe = strdup(v);
		else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) {
			if (!strcmp(v, "true"))
				skip = 1;
		} else if (!strcmp(line, "TryExec") && !have_bin(v))
			skip = 1;
	}
	fclose(f);
	if (skip || !type_ok || !name[0] || !exe) {
		free(exe);
		return;
	}
	clean_exec(exe);
	if (!exe[0]) {
		free(exe);
		return;
	}
	add_app(name, gen, exe, id);
}

/* recursive scan of one applications dir; id dedup first-wins across the
 * whole run (user data_home scanned first) */
static void
scan_dir(const char *dir, const char *base)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		char path[1024], id[128];
		struct stat st;
		int i, dup = 0;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) < 0)
			continue;
		make_id(id, sizeof id, base, e->d_name);
		if (S_ISDIR(st.st_mode)) {
			scan_dir(path, id);
			continue;
		}
		if (n < 9 || strcmp(e->d_name + n - 8, ".desktop"))
			continue;
		for (i = 0; i < napps; i++)
			if (!strcmp(apps[i].id, id)) {
				dup = 1;
				break;
			}
		if (!dup)
			parse_desktop(path, id);
	}
	closedir(d);
}

static void
scan_root(const char *share) /* <share>/applications */
{
	char dir[600];
	snprintf(dir, sizeof dir, "%s/applications", share);
	scan_dir(dir, NULL);
}

static void
scan(void)
{
	const char *home = getenv("HOME");
	const char *xdg;
	char *copy, *p;

	xdg = getenv("XDG_DATA_HOME");
	if (xdg && xdg[0])
		scan_root(xdg);
	else if (home) {
		char dir[512];
		snprintf(dir, sizeof dir, "%s/.local/share", home);
		scan_root(dir);
	}
	xdg = getenv("XDG_DATA_DIRS");
	if (xdg && xdg[0]) {
		copy = strdup(xdg);
		if (copy) {
			for (p = strtok(copy, ":"); p; p = strtok(NULL, ":"))
				scan_root(p);
			free(copy);
		}
	} else {
		scan_root("/usr/local/share");
		scan_root("/usr/share");
	}
	/* posix/BSD extras; id dedup makes repeats free */
	scan_root("/usr/pkg/share");  /* NetBSD pkgsrc */
	scan_root("/opt/local/share"); /* MacPorts */
}

#if LIST_PATH_EXEC
/* every executable in $PATH. Dedup by exact name among PATH entries
 * only, PATH dirs in order, appended after desktops then all sorted. */
static void
scan_path(void)
{
	const char *env = getenv("PATH");
	char *copy, *p;
	int start = napps, i;
	struct dirent *e;
	if (!env)
		return;
	copy = strdup(env);
	if (!copy)
		return;
	for (p = strtok(copy, ":"); p; p = strtok(NULL, ":")) {
		DIR *d = opendir(p);
		if (!d)
			continue;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, "[") || !strcmp(e->d_name, "]"))
				continue;
			char full[1024];
			char *ex;
			struct stat st;
			int dup = 0;
			snprintf(full, sizeof full, "%s/%s", p, e->d_name);
			if (stat(full, &st) < 0 || !S_ISREG(st.st_mode) ||
			    !(st.st_mode & S_IXUSR))
				continue;
			for (i = start; i < napps; i++)
				if (!strcmp(apps[i].name, e->d_name)) {
					dup = 1;
					break;
				}
			if (dup)
				continue;
			ex = strdup(full);
			if (ex)
				add_app(e->d_name, "", ex, "");
		}
		closedir(d);
	}
	free(copy);
}
#endif

static int
cmp_app(const void *a, const void *b)
{
	return strcasecmp(((const struct app *)a)->name,
	    ((const struct app *)b)->name);
}

static void
run_cmd(char *cmd) /* tokenize in place, fork+execvp, close launcher, free */
{
	char *argv[64], *t;
	int argc = 0;
	pid_t p;
	for (t = strtok(cmd, " \t"); t && argc < 63; t = strtok(NULL, " \t"))
		argv[argc++] = t;
	argv[argc] = NULL;
	if (argc > 0) {
		p = fork();
		if (p == 0) {
			setsid();
			execvp(argv[0], argv);
			_exit(127);
		}
		if (p > 0)
			running = 0;
	}
	free(cmd);
}

static void
launch(void)
{
	char *e;
	if (sel < 0 || sel >= nmatch)
		return;
	e = strdup(apps[match[sel]].exec);
	if (!e)
		return;
	clean_exec(e);
	run_cmd(e);
}

static int
u32_utf8(char *d, uint32_t cp)
{
	if (cp < 0x80) {
		d[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		d[0] = (char)(0xc0 | (cp >> 6));
		d[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		d[0] = (char)(0xe0 | (cp >> 12));
		d[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		d[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	d[0] = (char)(0xf0 | (cp >> 18));
	d[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	d[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	d[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static void
draw(void)
{
	const char *qs;
	int qx, cx, maxq, ql, ry0, top, i, idx, ry, on, ty;
	uint32_t bg;

	/* 1px border, then translucent fill (reference SVG) */
	rect(0, 0, WIDGET_W, WIDGET_H, COLOR_BORDER);
	bg = (COLOR_BG & 0xffffff00u) | BG_ALPHA;
	rect(1, 1, WIDGET_W - 2, WIDGET_H - 2, bg);

	/* search line: prompt, query tail, blinking cursor */
	set_font(FONT_SIZE);
	qx = draw_str(PAD, SEARCH_BASE - asc, PROMPT, COLOR_ACCENT, WIDGET_W) + 10;
	maxq = (WIDGET_W - PAD - CURSOR_W - qx) / (FONT_SIZE / 2);
	qs = query;
	ql = (int)strlen(query);
	if (ql > maxq && maxq > 0)
		qs = query + ql - maxq;
	if (query[0])
		draw_str(qx, SEARCH_BASE - asc, qs, COLOR_FG,
		    WIDGET_W - PAD - CURSOR_W);
	cx = qx + str_width(qs);
	if (blink)
		rect(cx, SEARCH_BASE - 15, CURSOR_W, CURSOR_H, COLOR_ACCENT);

	/* list: full-width selection bar, rows, GenericName never shown */
	set_font(ROW_FONT_SIZE);
	ry0 = LIST_TOP;
	top = sel >= MAX_ROWS ? sel - MAX_ROWS + 1 : 0;
	for (i = 0; i < MAX_ROWS && top + i < nmatch; i++) {
		idx = match[top + i];
		ry = ry0 + i * ROW_H;
		on = top + i == sel;
		if (on)
			rect(1, ry, WIDGET_W - 2, ROW_H, COLOR_SEL_BG);
		ty = ry + ROW_BASE - asc;
		draw_str(PAD, ty, apps[idx].name,
		    on ? COLOR_FG : COLOR_ROW, WIDGET_W - PAD);
	}
}

static void
present(void)
{
	draw();
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage(surf, 0, 0, WIDGET_W, WIDGET_H);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
	need_redraw = 0;
	busy = 1;
}

static void
buf_release(void *data, struct wl_buffer *b)
{
	(void)data;
	(void)b;
	busy = 0;
}

static const struct wl_buffer_listener buf_listener = {
	.release = buf_release,
};

static struct wl_buffer *
create_buffer(void)
{
	int fd = memfd_create("aizer", MFD_CLOEXEC);
	size_t sz = (size_t)WIDGET_W * WIDGET_H * 4;
	void *p;
	struct wl_shm_pool *pool;
	struct wl_buffer *b;
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)sz) < 0) {
		close(fd);
		return NULL;
	}
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	px = p;
	pool = wl_shm_create_pool(shm, fd, (int)sz);
	b = wl_shm_pool_create_buffer(pool, 0, WIDGET_W, WIDGET_H,
	    WIDGET_W * 4, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(b, &buf_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void
reg_global(void *data, struct wl_registry *r, uint32_t name,
    const char *iface, uint32_t ver)
{
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer = wl_registry_bind(r, name,
		    &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
}

static void
reg_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

static void
kb_keymap(void *data, struct wl_keyboard *k, uint32_t fmt, int32_t fd,
    uint32_t size)
{
	char *s;
	(void)data;
	(void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	if (xkbstate)
		xkb_state_unref(xkbstate);
	if (xkbmap)
		xkb_keymap_unref(xkbmap);
	xkbmap = xkb_keymap_new_from_string(xkbctx, s, XKB_KEYMAP_FORMAT_TEXT_V1,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	if (xkbmap)
		xkbstate = xkb_state_new(xkbmap);
}

static void
kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s, struct wl_array *keys)
{
	(void)data;
	(void)k;
	(void)serial;
	(void)s;
	(void)keys;
}

static void
kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s)
{
	(void)data;
	(void)k;
	(void)serial;
	(void)s;
}

static void
kb_mods(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
    uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data;
	(void)k;
	(void)serial;
	(void)dep;
	(void)latched;
	(void)locked;
	(void)group;
}

static void
kb_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)data;
	(void)k;
	(void)delay; /* fixed REPEAT_DELAY, compositor delay ignored */
	if (rate > 0)
		rep_rate = rate;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* one key press action, shared by press events and the repeat timer */
static void
handle_key(uint32_t key)
{
	xkb_keysym_t sym;
	uint32_t ch;
	size_t ql;
	int dirty = 0;
	sym = xkb_state_key_get_one_sym(xkbstate, key + 8);
	switch (sym) {
	case XKB_KEY_Escape:
		running = 0;
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter: {
		char tok[QUERY_MAX], *e;
		size_t i = 0;
		while (query[i] && query[i] != ' ' && i + 1 < sizeof tok) {
			tok[i] = query[i];
			i++;
		}
		tok[i] = 0;
		/* typed command with arguments (alacritty -e htop): run it */
		if (i && strchr(query, ' ') && have_bin(tok) && (e = strdup(query)))
			run_cmd(e);
		else
			launch();
		break;
	}
	case XKB_KEY_BackSpace:
		ql = strlen(query);
		while (ql > 0 && ((unsigned char)query[ql - 1] & 0xc0) == 0x80)
			ql--;
		if (ql > 0)
			ql--;
		if (query[ql] != 0 || strlen(query) > 0) {
			query[ql] = 0;
			refilter();
			dirty = 1;
		}
		break;
	case XKB_KEY_Up:
		if (sel > 0) {
			sel--;
			dirty = 1;
		}
		break;
	case XKB_KEY_Down:
		if (sel + 1 < nmatch) {
			sel++;
			dirty = 1;
		}
		break;
	case XKB_KEY_Home:
		if (nmatch) {
			sel = 0;
			dirty = 1;
		}
		break;
	case XKB_KEY_End:
		if (nmatch) {
			sel = nmatch - 1;
			dirty = 1;
		}
		break;
	default:
		ch = xkb_state_key_get_utf32(xkbstate, key + 8);
		ql = strlen(query);
		if (ch >= 32 && ch != 0x7f) {
			char tmp[4];
			int n = u32_utf8(tmp, ch);
			if (ql + (size_t)n < sizeof query) {
				memcpy(query + ql, tmp, (size_t)n);
				query[ql + (size_t)n] = 0;
				refilter();
				dirty = 1;
			}
		}
		break;
	}
	if (dirty)
		need_redraw = 1;
}

static void
kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
    uint32_t key, uint32_t state)
{
	(void)data;
	(void)k;
	(void)serial;
	(void)time;
	if (!xkbstate)
		return;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		rep_key = key;
		rep_at = now_ms() + REPEAT_DELAY;
		handle_key(key);
	} else if (state == WL_KEYBOARD_KEY_STATE_RELEASED && key == rep_key)
		rep_key = 0;
}

static const struct wl_keyboard_listener kb_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_mods,
	.repeat_info = kb_repeat,
};

static void
seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kb) {
		kb = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(kb, &kb_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && kb) {
		wl_keyboard_destroy(kb);
		kb = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *s, uint32_t serial,
    uint32_t w, uint32_t h)
{
	(void)data;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(s, serial);
	configured = 1;
	need_redraw = 1;
}

static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *s)
{
	(void)data;
	(void)s;
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener lfsurf_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static int lockfd = -1;

static int
single_instance(void)
{
	char path[256];
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir && dir[0])
		snprintf(path, sizeof path, "%s/aizer.lock", dir);
	else
		snprintf(path, sizeof path, "/tmp/aizer.lock");
	lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0)
		return 0;
	if (flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		close(lockfd);
		lockfd = -1;
		return -1;
	}
	return 0;
}

int
main(void)
{
	struct pollfd pfd;
	int r;
	uint64_t blink_at;

	if (single_instance() < 0)
		return 0;

	if (load_font()) {
		fprintf(stderr, "aizer: cannot load font %s\n", FONT_NAME);
		return 1;
	}
	scan();
#if LIST_PATH_EXEC
	scan_path();
#endif
	fprintf(stderr, "aizer: %d apps\n", napps);
	qsort(apps, (size_t)napps, sizeof *apps, cmp_app);
	match = calloc((size_t)(napps ? napps : 1), sizeof *match);
	if (!match) {
		perror("aizer: calloc");
		return 1;
	}
	refilter();

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "aizer: cannot connect to wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_listener,
	    NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !seat || !layer) {
		fprintf(stderr, "aizer: compositor lacks needed interfaces\n");
		return 1;
	}
	xkbctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	surf = wl_compositor_create_surface(comp);
	lfsurf = zwlr_layer_shell_v1_get_layer_surface(layer, surf, NULL,
	    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "aizer");
	zwlr_layer_surface_v1_set_size(lfsurf, WIDGET_W, WIDGET_H);
	zwlr_layer_surface_v1_set_anchor(lfsurf, 0); /* no edge = centered */
	zwlr_layer_surface_v1_set_keyboard_interactivity(lfsurf,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(lfsurf, &lfsurf_listener, NULL);
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);
	if (!configured) {
		fprintf(stderr, "aizer: layer surface not configured\n");
		return 1;
	}

	buf = create_buffer();
	if (!buf) {
		fprintf(stderr, "aizer: shm buffer failed\n");
		return 1;
	}
	present();

	pfd.fd = wl_display_get_fd(dpy);
	pfd.events = POLLIN;
	blink_at = now_ms() + BLINK_MS;
	while (running) {
		uint64_t now;
		if (need_redraw && !busy)
			present();
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		now = now_ms();
		r = BLINK_MS;
		if (rep_key && rep_at <= now)
			r = 0;
		else if (rep_key && rep_at - now < (uint64_t)r)
			r = (int)(rep_at - now);
		if (blink_at <= now)
			r = 0;
		else if (blink_at - now < (uint64_t)r)
			r = (int)(blink_at - now);
		r = poll(&pfd, 1, r);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			now = now_ms();
			if (rep_key && now >= rep_at) {
				handle_key(rep_key);
				rep_at = now + 1000 / rep_rate;
			}
			if (now >= blink_at) {
				blink = !blink;
				blink_at = now + BLINK_MS;
				need_redraw = 1;
			}
			continue;
		}
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (wl_display_dispatch(dpy) < 0)
			break;
	}

	wl_display_disconnect(dpy);
	free(apps);
	free(match);
	return 0;
}
