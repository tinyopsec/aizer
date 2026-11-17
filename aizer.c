/* aizer is minimal Wayland app launcher: centered widget, type, Enter, done.
 * C23, deps: libc, wayland-client, xkbcommon, freetype, fontconfig. */
#define _GNU_SOURCE
#include "aizer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <ctype.h>
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
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

struct app {
	char name[96], gen[96], id[128];
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
static struct xkb_context *xkbc;
static struct xkb_keymap *xkbm;
static struct xkb_state *xkbs;
static int running = 1, configured, redraw, blink = 1, busy;
static uint32_t rep_key;
static uint64_t rep_at, rep_step = 25;
static int lockfd = -1;

static FT_Library ft;
static FT_Face face;
static int asc;

static void
rect(int x, int y, int w, int h, uint32_t c)
{
	uint32_t v = c << 24 | c >> 8;
	int i;

	while (h-- > 0) {
		for (i = 0; i < w; i++)
			px[y * WIDGET_W + x + i] = v;
		y++;
	}
}

/* blend glyph coverage over the pixel, keep buffer alpha */
static void
blend(int x, int y, uint32_t c, unsigned a)
{
	uint32_t d;
	unsigned r, g, b;

	if (!a || x < 0 || y < 0 || x >= WIDGET_W || y >= WIDGET_H)
		return;
	d = px[y * WIDGET_W + x];
	r = (c >> 24 & 255) * a + (d >> 16 & 255) * (255 - a);
	g = (c >> 16 & 255) * a + (d >> 8 & 255) * (255 - a);
	b = (c >> 8 & 255) * a + (d & 255) * (255 - a);
	px[y * WIDGET_W + x] = (d & 0xff000000u) |
	    r / 255 << 16 | g / 255 << 8 | b / 255;
}

static const char *
dec(const char *s, uint32_t *cp)
{
	unsigned char c = (unsigned char)*s;
	int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 :
	    (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;

	if (n < 0) {
		*cp = '?';
		return s + 1;
	}
	*cp = n ? c & (0xff >> (n + 2)) : c;
	while (n-- > 0 && ((unsigned char)s[1] & 0xc0) == 0x80)
		*cp = (*cp << 6) | ((unsigned char)*++s & 0x3f);
	return s + 1;
}

static int
glyph(int x, int ytop, uint32_t cp, uint32_t c)
{
	FT_Bitmap *b;
	int bx, by, row, col;
	unsigned a;

	if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
		return FONT_SIZE / 2;
	b = &face->glyph->bitmap;
	bx = x + face->glyph->bitmap_left;
	by = ytop + asc - face->glyph->bitmap_top;
	for (row = 0; row < (int)b->rows; row++)
		for (col = 0; col < (int)b->width; col++) {
			a = b->buffer[row * b->pitch + col];
			if (a)
				blend(bx + col, by + row, c, a);
		}
	return (int)(face->glyph->advance.x >> 6);
}

/* draw one line, clip at the right edge, return pen x */
static int
text(int x, int ytop, const char *s, uint32_t c)
{
	uint32_t cp;

	while (*s && x < WIDGET_W) {
		s = dec(s, &cp);
		x += glyph(x, ytop, cp, c);
	}
	return x;
}

static int
load_font(void)
{
	FcPattern *pat, *fcm;
	FcChar8 *file;
	FcResult res;
	int idx = 0, err;

	if (FT_Init_FreeType(&ft) || !FcInit())
		return -1;
	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)FONT_NAME);
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
	err = FT_New_Face(ft, (const char *)file, idx, &face);
	FcPatternDestroy(fcm);
	if (err || FT_Set_Pixel_Sizes(face, 0, FONT_SIZE))
		return -1;
	asc = face->size->metrics.ascender >> 6;
	return 0;
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

/* fuzzy subsequence: every needle char appears in order, case blind.
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

/* all space separated tokens must match name or GenericName.
 * *exact = 1 when every token is a plain substring. */
static int
app_match(const struct app *a, int *exact)
{
	char q[QUERY_MAX], *t;
	int sub = 1, fuzz = 1;

	if (!query[0]) {
		*exact = 1;
		return 1;
	}
	snprintf(q, sizeof q, "%s", query);
	for (t = strtok(q, " "); t; t = strtok(NULL, " ")) {
		if (!ci_has(a->name, t) && !(a->gen[0] && ci_has(a->gen, t)))
			sub = 0;
		if (!ci_sub(a->name, t) && !(a->gen[0] && ci_sub(a->gen, t)))
			fuzz = 0;
	}
	*exact = sub;
	return sub || fuzz;
}

/* exact substring matches first, then fuzzy */
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
		if (*r != '%') {
			*w++ = *r;
			continue;
		}
		if (r[1] == 0)
			break;
		if (r[1] == '%')
			*w++ = '%';
		r++;
	}
	*w = 0;
}

static int
have_bin(const char *bin)
{
	char full[1024];
	const char *path = getenv("PATH");
	char *copy, *p;

	if (bin[0] == '/')
		return access(bin, X_OK) == 0;
	if (!path || !(copy = strdup(path)))
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
add_app(const char *name, const char *gen, char *exec, const char *id)
{
	struct app *a;

	if (napps == capapps) {
		capapps = capapps ? 2 * capapps : 256;
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
	a->exec = exec;
}

static void
parse_desktop(const char *path, const char *id)
{
	FILE *f = fopen(path, "r");
	char line[512], name[96] = "", gen[96] = "", *exe = NULL, *eq, *v;
	int entry = 0, skip = 0, app = 1;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '[') {
			entry = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!entry || line[0] == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq = 0;
		v = eq + 1;
		if (!strcmp(line, "Type"))
			app = !strcmp(v, "Application");
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
	if (skip || !app || !name[0] || !exe) {
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

/* recursive scan of one applications dir, first id wins */
static void
scan_dir(const char *dir, const char *base)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[1024], id[128];
	struct stat st;
	size_t n;
	int i;

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) < 0)
			continue;
		if (base)
			snprintf(id, sizeof id, "%s-%s", base, e->d_name);
		else
			snprintf(id, sizeof id, "%s", e->d_name);
		if (S_ISDIR(st.st_mode)) {
			scan_dir(path, id);
			continue;
		}
		n = strlen(e->d_name);
		if (n < 9 || strcmp(e->d_name + n - 8, ".desktop"))
			continue;
		for (i = 0; i < napps; i++)
			if (!strcmp(apps[i].id, id))
				break;
		if (i == napps)
			parse_desktop(path, id);
	}
	closedir(d);
}

static void
scan_root(const char *share)
{
	char dir[600];

	snprintf(dir, sizeof dir, "%s/applications", share);
	scan_dir(dir, NULL);
}

static void
scan(void)
{
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char dir[512], *copy, *p;

	if (!xdg || !xdg[0]) {
		if (!home)
			return;
		snprintf(dir, sizeof dir, "%s/.local/share", home);
		xdg = dir;
	}
	scan_root(xdg);
	if ((xdg = getenv("XDG_DATA_DIRS")) && xdg[0]) {
		if ((copy = strdup(xdg))) {
			for (p = strtok(copy, ":"); p; p = strtok(NULL, ":"))
				scan_root(p);
			free(copy);
		}
	} else {
		scan_root("/usr/local/share");
		scan_root("/usr/share");
	}
	/* posix and BSD extras, id dedup makes repeats free */
	scan_root("/usr/pkg/share");
	scan_root("/opt/local/share");
}

#if LIST_PATH_EXEC
/* every executable in $PATH, dedup by name, PATH order kept */
static void
scan_path(void)
{
	const char *env = getenv("PATH");
	char *copy, *p, full[1024];
	struct dirent *e;
	struct stat st;
	int start = napps, i, dup;
	DIR *d;

	if (!env || !(copy = strdup(env)))
		return;
	for (p = strtok(copy, ":"); p; p = strtok(NULL, ":")) {
		if (!(d = opendir(p)))
			continue;
		while ((e = readdir(d))) {
			if (!isalnum((unsigned char)e->d_name[0]))
				continue;
			snprintf(full, sizeof full, "%s/%s", p, e->d_name);
			if (stat(full, &st) < 0 || !S_ISREG(st.st_mode) ||
			    !(st.st_mode & S_IXUSR))
				continue;
			dup = 0;
			for (i = start; i < napps; i++)
				if (!strcmp(apps[i].name, e->d_name))
					dup = 1;
			if (!dup)
				add_app(e->d_name, "", strdup(full), "");
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

/* split in place, fork, exec, close launcher */
static void
run(char *cmd)
{
	char *av[64], *t;
	int n = 0;
	pid_t p;

	for (t = strtok(cmd, " \t"); t && n < 63; t = strtok(NULL, " \t"))
		av[n++] = t;
	av[n] = NULL;
	if (n) {
		p = fork();
		if (p == 0) {
			setsid();
			execvp(av[0], av);
			perror("aizer");
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
	if ((e = strdup(apps[match[sel]].exec))) {
		clean_exec(e);
		run(e);
	}
}

static void
draw(void)
{
	int i, top = sel >= MAX_ROWS ? sel - MAX_ROWS + 1 : 0;
	int y, x, on;

	rect(0, 0, WIDGET_W, WIDGET_H,
	    (COLOR_BG & 0xffffff00u) | BG_ALPHA);
	x = text(PAD, SEARCH_BASE - asc, PROMPT, COLOR_ACCENT) + CURSOR_GAP;
	x = text(x, SEARCH_BASE - asc, query, COLOR_FG);
	if (x > WIDGET_W - CURSOR_W)
		x = WIDGET_W - CURSOR_W;
	if (blink)
		rect(x, CURSOR_TOP, CURSOR_W, CURSOR_H, COLOR_FG);

	for (i = 0; i < MAX_ROWS && top + i < nmatch; i++) {
		on = top + i == sel;
		y = LIST_TOP + i * ROW_H;
		if (on)
			rect(0, y, WIDGET_W, ROW_H, COLOR_SEL_BG);
		x = on ? text(PAD, y + ROW_BASE - asc, MARK, COLOR_FG)
		    : PAD;
		text(x, y + ROW_BASE - asc, apps[match[top + i]].name,
		    on ? COLOR_FG : COLOR_ROW);
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
	redraw = 0;
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
	struct wl_shm_pool *pool;
	struct wl_buffer *b;
	void *p;

	if (fd < 0 || ftruncate(fd, (off_t)sz) < 0)
		return NULL;
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
	xkb_state_unref(xkbs);
	xkb_keymap_unref(xkbm);
	xkbm = xkb_keymap_new_from_string(xkbc, s, XKB_KEYMAP_FORMAT_TEXT_V1,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	if (xkbm)
		xkbs = xkb_state_new(xkbm);
}

static void
kb_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)data;
	(void)k;
	(void)delay; /* fixed REPEAT_DELAY, compositor delay ignored */
	if (rate > 0) {
		uint64_t step = (uint64_t)(1000 / rate);
		rep_step = step < 25 ? step : 25;
	}
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void
key(uint32_t code)
{
	xkb_keysym_t sym;
	char t[QUERY_MAX], tmp[8], *e;
	size_t n;
	int dirty = 0, len;

	if (!xkbs)
		return;
	sym = xkb_state_key_get_one_sym(xkbs, code + 8);
	switch (sym) {
	case XKB_KEY_Escape:
		running = 0;
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		n = strcspn(query, " ");
		memcpy(t, query, n);
		t[n] = 0;
		if (query[n] && have_bin(t) && (e = strdup(query)))
			run(e);
		else
			launch();
		return;
	case XKB_KEY_BackSpace:
		n = strlen(query);
		while (n && ((unsigned char)query[n - 1] & 0xc0) == 0x80)
			n--;
		if (n) {
			query[n - 1] = 0;
			refilter();
			dirty = 1;
		}
		break;
	case XKB_KEY_Up:
		dirty = sel > 0;
		sel -= dirty;
		break;
	case XKB_KEY_Down:
		dirty = sel + 1 < nmatch;
		sel += dirty;
		break;
	case XKB_KEY_Home:
		dirty = nmatch && sel;
		if (nmatch)
			sel = 0;
		break;
	case XKB_KEY_End:
		dirty = nmatch && sel != nmatch - 1;
		if (nmatch)
			sel = nmatch - 1;
		break;
	default:
		len = xkb_state_key_get_utf8(xkbs, code + 8, tmp, sizeof tmp);
		if (len <= 0 || (unsigned char)tmp[0] < 32 || tmp[0] == 127)
			break;
		n = strlen(query);
		if (n + (size_t)len < sizeof query) {
			memcpy(query + n, tmp, (size_t)len);
			query[n + (size_t)len] = 0;
			refilter();
			dirty = 1;
		}
		break;
	}
	redraw |= dirty;
}

static void
kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
    uint32_t code, uint32_t state)
{
	(void)data; (void)k; (void)serial; (void)time;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		rep_key = code;
		rep_at = now_ms() + REPEAT_DELAY;
		key(code);
	} else if (code == rep_key)
		rep_key = 0;
}

static void
kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s, struct wl_array *keys)
{
	(void)data; (void)k; (void)serial; (void)s; (void)keys;
}

static void
kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *s)
{
	(void)data; (void)k; (void)serial; (void)s;
}

static void
kb_mods(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
    uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data; (void)k; (void)serial; (void)dep; (void)latched;
	(void)locked; (void)group;
}

static const struct wl_keyboard_listener kb_listener = {
	.enter = kb_enter,
	.leave = kb_leave,
	.modifiers = kb_mods,
	.keymap = kb_keymap,
	.key = kb_key,
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

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
};

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
reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
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
	redraw = 1;
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

static int
single_instance(void)
{
	char path[256];
	const char *dir = getenv("XDG_RUNTIME_DIR");

	if (dir && dir[0])
		snprintf(path, sizeof path, "%s/aizer.lock", dir);
	else
		snprintf(path, sizeof path, "/tmp/aizer-%d.lock", (int)getuid());
	lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0 && errno == EACCES) {
		unlink(path);
		lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	}
	if (lockfd < 0)
		return 0;
	if (flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		close(lockfd);
		return -1;
	}
	return 0;
}

int
main(void)
{
	struct pollfd pfd;
	uint64_t blink_at, now, wake;
	int r;

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
	if (!(match = calloc((size_t)(napps ? napps : 1), sizeof *match))) {
		perror("aizer: calloc");
		return 1;
	}
	refilter();

	xkbc = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkbc) {
		fprintf(stderr, "aizer: xkb context failed\n");
		return 1;
	}
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "aizer: cannot connect to wayland display\n");
		return 1;
	}
	fcntl(wl_display_get_fd(dpy), F_SETFD, FD_CLOEXEC);
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_listener,
	    NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !seat || !layer) {
		fprintf(stderr, "aizer: compositor lacks needed interfaces\n");
		return 1;
	}

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
	if (!(buf = create_buffer())) {
		fprintf(stderr, "aizer: shm buffer failed\n");
		return 1;
	}
	present();

	pfd.fd = wl_display_get_fd(dpy);
	pfd.events = POLLIN;
	blink_at = now_ms() + BLINK_MS;
	while (running) {
		if (redraw && !busy)
			present();
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		now = now_ms();
		wake = blink_at;
		if (rep_key && rep_at < wake)
			wake = rep_at;
		r = poll(&pfd, 1, wake > now ? (int)(wake - now) : 0);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			now = now_ms();
			if (rep_key && now >= rep_at) {
				key(rep_key);
				rep_at = now + rep_step;
			}
			if (now >= blink_at) {
				blink = !blink;
				blink_at = now + BLINK_MS;
				redraw = 1;
			}
		} else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		else if (wl_display_dispatch(dpy) < 0)
			break;
	}

	munmap(px, (size_t)WIDGET_W * WIDGET_H * 4);
	xkb_state_unref(xkbs);
	xkb_keymap_unref(xkbm);
	xkb_context_unref(xkbc);
	wl_display_disconnect(dpy);
	while (napps)
		free(apps[--napps].exec);
	free(apps);
	free(match);
	return 0;
}
