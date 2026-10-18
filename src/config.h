#ifndef TERMTIME_CONFIG_H
#define TERMTIME_CONFIG_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

#define LINE_COUNT 2
#define FORMAT_MAX 512
#define COLOR_KEY_MAX 64
#define COLOR_MAX 64
#define SCALE_LIMIT 50

/**
 * Overrides the line color for a part of the format.
 * key is matched literally against the format string,
 * e.g. "%H", ":" or "%H:%M".
 */
typedef struct {
  char key[COLOR_KEY_MAX];
  int color;
} color_rule;

typedef struct {
  int enabled;
  char format[FORMAT_MAX];
  int scale;
  int color;
  color_rule colors[COLOR_MAX];
  int ncolors;
} line_config;

// How blocks are drawn
typedef enum {
  BLOCKS_AUTO,    // unicode on UTF-8 locales, otherwise reverse
  BLOCKS_UNICODE, // U+2588 full block
  BLOCKS_REVERSE, // spaces in reverse video, works with any font/charset
  BLOCKS_ASCII,   // a configured ASCII character
} blocks_mode;

// Colors the terminal supports, 0 = detect from TERM
#define COLORS_AUTO 0

typedef struct {
  int interval_ms;
  int spacing;
  int fit; // shrink lines that do not fit the terminal
  char locale[64];
  blocks_mode blocks;
  char block_char; // for BLOCKS_ASCII
  int colors;      // COLORS_AUTO, 8, 16 or 256
  line_config lines[LINE_COUNT];
  // Path the config was loaded from, empty when using built-in defaults
  char path[PATH_MAX];
} config;

/**
 * Finds the config file to use, in order:
 * - $XDG_CONFIG_HOME/termtime/config.toml
 * - $HOME/.config/termtime/config.toml
 * - SYSCONFDIR/termtime/config.toml
 *
 * Returns 0 and fills out when found, -1 otherwise.
 */
int config_find(char *out, size_t size);

/**
 * Loads the config. When path is NULL the lookup order
 * of config_find() is used, falling back to built-in defaults.
 *
 * Returns 0 on success, -1 on failure (message in err).
 */
int config_load(config *cfg, const char *path, char *err, size_t errsize);

#endif // !TERMTIME_CONFIG_H
