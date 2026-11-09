<div align="center">
  <img src="https://capsule-render.vercel.app/api?type=waving&color=0A0A0A&height=80&section=header&text=aizer&fontSize=52&fontColor=E5E4E2&fontAlignY=72&animation=fadeIn" alt="aizer header">
  <br>
  <img src="https://readme-typing-svg.demolab.com?font=JetBrains+Mono&size=14&pause=1200&color=E5E4E2&center=true&vCenter=true&width=600&lines=Minimal+Wayland+application+launcher;Centered+rectangle.+No+runtime+config;Compile-time+configuration+only" alt="aizer typing">
  <br>
  <img src="https://raw.githubusercontent.com/tinyopsec/assets/main/aizer/aizer-logo.svg" style="width: 32%; height: auto;" alt="aizer logo">
</div>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-E5E4E2?style=flat&labelColor=0A0A0A" alt="license MIT">
  <img src="https://img.shields.io/badge/C23-E5E4E2?style=flat&label=lang&labelColor=0A0A0A&logo=c&logoColor=E5E4E2" alt="C23">
  <img src="https://img.shields.io/badge/Wayland-E5E4E2?style=flat&label=display&labelColor=0A0A0A" alt="Wayland">
  <img src="https://img.shields.io/badge/make-E5E4E2?style=flat&label=build&labelColor=0A0A0A&logo=gnu&logoColor=E5E4E2" alt="make">
  <img src="https://img.shields.io/badge/Linux-E5E4E2?style=flat&labelColor=0A0A0A&logo=linux&logoColor=E5E4E2" alt="Linux">
</p>

<div align="center">
  <img src="https://raw.githubusercontent.com/tinyopsec/assets/main/aizer/aizer-screenshot.webp" style="width: 80%; height: auto;" alt="aizer screenshot">
</div>

## Contents

- [Get started](#get-started)
- [Why small](#why-small)
- [Features](#features)
- [Installation](#installation)
- [Usage](#usage)
- [Configuration](#configuration)
- [How It Works](#how-it-works)
- [Contributing](#contributing)

---

## Get started

```sh
git clone --depth 1 https://github.com/tinyopsec/aizer
cd aizer
make
sudo make install
```

Bind a key in the compositor to run `aizer`. It reads `.desktop` files and executables from `$PATH`, shows a centered widget, filters as you type, and closes after a launch.

---

## Why small

I kept only what a launcher needs: a widget, a list, a filter, a launch. No bar, no icons, no history, no daemon.

| Area | aizer |
|---|---|
| Lines of code | about 1100 (one reading session) |
| Runtime dependencies | libc, wayland-client, xkbcommon, freetype, fontconfig |
| Idle RAM | a few MB |
| Configuration | single flat `aizer.h`, compile time only |
| Config files at runtime | none |
| IPC | none |
| Drawing | text only, no images or logos |

---

## Features

What it does, nothing extra:

| Category | Details |
|---|---|
| Widget | centered rectangle, ARGB transparency, fixed size from `aizer.h` |
| List | `.desktop` entries plus executables from `$PATH` |
| Filter | case-insensitive substring and subsequence match on name and generic name |
| Launch | `fork` + `execvp`, no shell; typed command lines with arguments run directly |
| Close | exits after a successful launch, or on Esc |
| Input | Up/Down move selection, Enter launch, Esc close, Backspace delete |
| Repeat | held arrow keys scroll after a short delay |
| Build | clean under `gcc` or `clang` with `-std=c23 -pedantic -Wall -Wextra` |

---

## Installation

### Requirements

A C compiler (`gcc` or `clang`), `make`, `pkg-config`, and the development headers for `wayland-client`, `xkbcommon`, `freetype2` and `fontconfig`. No runtime dependencies beyond those libraries.

### Build from source

```sh
git clone --depth 1 https://github.com/tinyopsec/aizer
cd aizer
make
sudo make install   # installs to /usr/local/bin/aizer
```

Change `PREFIX` in the `Makefile` to install elsewhere. To stage the install for packaging, use `make DESTDIR=/tmp/stage install`.

### Per system

The `Makefile` ships commented blocks for FreeBSD, DragonFly and NetBSD (pkgsrc). Uncomment the block for the target system and build normally. The Linux block is active by default.

### Uninstall

```sh
sudo make uninstall
```

---

## Usage

Run `aizer` from a key binding in the compositor. It opens a centered widget, reads the application list, and waits for input.

- type to filter the list
- Up and Down move the selection
- Enter launches the selected application, or a typed command line with arguments
- Esc closes without launching
- a typed command line such as `alacritty -e htop` runs directly and closes the launcher

The widget closes itself after a successful launch, so it does not stay in the background.

---

## Configuration

I configure aizer by editing `aizer.h` and recompiling:

```sh
$EDITOR aizer.h
make && sudo make install
```

There is no config file, no IPC and no reload mechanism.

### Options

| Option | Default | Description |
|---|---:|---|
| `WIDGET_W`, `WIDGET_H` | 419, 449 | widget size in pixels |
| `ROW_H`, `MAX_ROWS` | 26, 15 | row height and visible rows |
| `COLOR_*` | see file | colors in `0xRRGGBBAA` |
| `BG_ALPHA` | `0xff` | background alpha (0xff = opaque) |
| `FONT_NAME`, `FONT_SIZE`, `ROW_FONT_SIZE` | monospace, 16, 14 | font and sizes |
| `PROMPT` | `">"` | search line prompt |
| `QUERY_MAX` | 64 | query buffer size |
| `BLINK_MS` | 550 | cursor blink half period in ms |
| `REPEAT_DELAY` | 100 | key repeat delay in ms |
| `LIST_PATH_EXEC` | 1 | list executables from `$PATH` |
| `PAD`, `LIST_TOP`, `SEARCH_BASE` | 24, 51, 32 | layout offsets |

---

## How It Works

aizer connects to the Wayland compositor, creates a layer surface centered on screen, and allocates a shared memory buffer in ARGB format. It draws text with FreeType through fontconfig, filters the application list on every keystroke, and redraws only when the input or selection changes. On Enter it forks and calls `execvp` without a shell, then exits.

The application list comes from `.desktop` files under the XDG data directories plus executables found in `$PATH`. Entries are deduplicated by desktop id; the first copy wins.

## Limitations

- Wayland only: no X11 or XWayland support.
- No icons, images or logos in the widget: text only.
- No history or recent apps: the list resets on every launch.
- No runtime config: change `aizer.h` and recompile.
- Requires a compositor with `wlr-layer-shell` support.
- Silent failure on exec: if a typed command is not found, the launcher exits without an error message.
- `xkb_context_new` failure is not checked.

---

## Contributing

Found a bug or a rough edge. Open an issue with steps to reproduce and the setup.

<details>
<summary>Code requirements</summary>

- no external dependencies beyond the allowed list
- no over-abstraction or wrapper layers
- compiles clean: `gcc -std=c23 -pedantic -Wall -Wextra`
- optional features behind compile-time constants are considered; core event loop changes are reviewed carefully

</details>

---

MIT license, see `LICENSE`.