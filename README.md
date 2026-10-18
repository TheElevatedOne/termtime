# termtime

Terminal clock that draws two lines of big block letters, centered on the
screen, using [blockfont](https://github.com/TheElevatedOne/blockfont.h).

## Build & install

Requires `libblockfont` (AUR: `blockfont-git`), or build it in with
`BLOCKFONT=static` (cloned from GitHub at a pinned tag).

```shell
make                         # build/termtime
make test                    # unit, command line and terminal tests
sudo make install            # /usr/local/bin/termtime, man page, /etc/termtime/config.toml
sudo make PREFIX=/usr install
make BLOCKFONT=static        # no libblockfont needed at runtime
make package                 # dist/termtime-VERSION-ARCH.tar.gz + .sha256
```

`make install` does not overwrite an existing `/etc/termtime/config.toml`
(unless `DESTDIR` is set, for packaging).

`make package` always builds blockfont in, so the archive runs without
libblockfont. It contains `usr/bin/termtime`, the man page, the docs and
`etc/termtime/config.toml`; extract it with `sudo tar -xzf FILE
--strip-components=1 -C /`. Cross builds work with `CC` and `ARCH`, e.g.
`make package CC=arm-linux-gnueabihf-gcc ARCH=armv7`.

### Layout

```
src/        sources
config/     default config.toml
tests/      unit, command line and terminal tests
build/      objects and binaries (make clean)
dist/       release archives (make distclean)
```

### Releases

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds and
tests x86_64, aarch64, i686 and armv7 on every push. Pushing a tag
(`git tag 0.1.0 && git push --tags`), or running the workflow by hand,
publishes the archives with their `.sha256` files and a `SHA256SUMS` to the
release of that tag (the latest tag for manual runs).

## Usage

```
termtime [-c FILE] [-1] [-p] [-h] [-v]
  -c, --config FILE   use FILE instead of the default config
  -1, --once          print the lines once and exit
  -p, --path          print the config file in use and exit
```

Keys: `q` / `Esc` quit, `r` reloads the config (or `kill -USR1`).
A config with errors is ignored on reload. Ctrl-Z / `fg` work as usual.

Lines that do not fit the terminal are shrunk (`fit = true`). On the Linux
console (`TERM=linux`) and other limited terminals colors are mapped to 16
or 8, and without a UTF-8 locale the blocks are drawn as reverse video
spaces; see `blocks` and `colors` in the config to force a style.

## Configuration

The first config found is used:

1. `$XDG_CONFIG_HOME/termtime/config.toml`
2. `$HOME/.config/termtime/config.toml`
3. `/etc/termtime/config.toml`

```toml
[general]
interval = 1000   # redraw interval in ms, aligned to the clock
spacing = 2       # empty rows between the lines
fit = true        # shrink lines that do not fit the terminal
blocks = "auto"   # "unicode", "reverse" or an ASCII character like "#"
colors = "auto"   # 256, 16 or 8
# locale = ""     # "" = environment locale for %A, %B... (default "C")

[line1]
format = "%I:%M:%S %p"   # date(1)/strftime(3) conversions
scale = 2                # 1-50
color = 15               # 0-255 or a name like "red", "bright_blue", "gray"

[line1.colors]           # color parts of the format
"%I" = "bright_red"      # hours
":"  = 8                 # every colon
"%M" = "bright_green"    # minutes
"%S" = "bright_blue"     # seconds
"%p" = 244

[line2]
enabled = true
format = "%A %d.%m.%Y"
scale = 1
color = 7
```

Formats support everything `strftime(3)` does (including GNU flags like
`%-d`, `%_H`, `%^a`) plus the `date(1)` extras `%N` / `%3N`
(nanoseconds / milliseconds), `%q` (quarter) and `%:z`, `%::z`, `%:::z`.
The font is ASCII only; other characters are drawn as one placeholder glyph.

Color keys are matched literally against the format, the longest match wins
(so `"%H:%M"` can color a whole group), and `"%d"` also covers flagged forms
such as `%-d`. Everything else uses the line's `color`.

See [`config/config.toml`](config/config.toml) for the full commented default.
