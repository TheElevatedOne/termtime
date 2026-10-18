#include "config.h"
#include "render.h"
#include "toml.h"

#include <blockfont.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                          \
  } while (0)

#define CHECK_STR(got, want)                                                   \
  do {                                                                         \
    checks++;                                                                  \
    const char *g_ = (got), *w_ = (want);                                      \
    if (!g_ || strcmp(g_, w_) != 0) {                                          \
      failures++;                                                              \
      fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
              g_ ? g_ : "(null)", w_);                                         \
    }                                                                          \
  } while (0)

static char tmpdir[256];

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(1);
  }
  fputs(content, f);
  fclose(f);
}

static void mkdirs(const char *path) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(buf, 0755);
      *p = '/';
    }
  }
  mkdir(buf, 0755);
}

/* TOML */

static void test_toml_values(void) {
  toml_doc doc;
  int rc = toml_parse_string(
      "# comment\n"
      "top = 1\n"
      "\n"
      "[general]   # trailing comment\n"
      "int = -1_000\n"
      "hex = 0xff\n"
      "float = 2.5\n"
      "yes = true\n"
      "no = false\n"
      "basic = \"a\\tb\\\"c\\u00e9\"\n"
      "literal = 'C:\\path'\n"
      "dotted.key = 3\n"
      "\"quoted key\" = 4\n"
      "arr = [1, 2,\n  3]\n"
      "[line1.colors]\n"
      "\"%H\" = 9\n"
      "':' = \"gray\"\n"
      "[line2]\n"
      "colors = { \"%M\" = 1, '.' = 2 }\n"
      "dup = 1\n"
      "dup = 2\n",
      &doc);
  CHECK(rc == 0);
  if (rc != 0) {
    fprintf(stderr, "  error: %s\n", doc.error);
    toml_free(&doc);
    return;
  }

  const toml_entry *e;
  CHECK((e = toml_get(&doc, "top")) && e->type == TOML_INT && e->ival == 1);
  CHECK((e = toml_get(&doc, "general.int")) && e->ival == -1000);
  CHECK((e = toml_get(&doc, "general.hex")) && e->ival == 255);
  CHECK((e = toml_get(&doc, "general.float")) && e->type == TOML_FLOAT &&
        e->fval == 2.5);
  CHECK((e = toml_get(&doc, "general.yes")) && e->type == TOML_BOOL &&
        e->ival == 1);
  CHECK((e = toml_get(&doc, "general.no")) && e->type == TOML_BOOL &&
        e->ival == 0);
  CHECK((e = toml_get(&doc, "general.basic")) && e->type == TOML_STRING);
  CHECK_STR(e ? e->str : NULL, "a\tb\"c\xc3\xa9");
  CHECK_STR((e = toml_get(&doc, "general.literal")) ? e->str : NULL,
            "C:\\path");
  CHECK((e = toml_get(&doc, "general.dotted.key")) && e->ival == 3);
  CHECK((e = toml_get(&doc, "general.quoted key")) && e->ival == 4);
  CHECK((e = toml_get(&doc, "general.arr")) && e->type == TOML_ARRAY &&
        e->ival == 3);
  CHECK((e = toml_get(&doc, "line1.colors.%H")) && e->ival == 9);
  CHECK_STR((e = toml_get(&doc, "line1.colors.:")) ? e->str : NULL, "gray");
  CHECK((e = toml_get(&doc, "line2.colors.%M")) && e->ival == 1);
  CHECK((e = toml_get(&doc, "line2.colors..")) && e->ival == 2);
  CHECK((e = toml_get(&doc, "line2.dup")) && e->ival == 2);
  CHECK(toml_get(&doc, "missing") == NULL);
  toml_free(&doc);
}

static void expect_toml_error(const char *src, const char *fragment) {
  toml_doc doc;
  int rc = toml_parse_string(src, &doc);
  CHECK(rc != 0);
  if (rc != 0 && !strstr(doc.error, fragment)) {
    failures++;
    fprintf(stderr, "error \"%s\" does not mention \"%s\"\n", doc.error,
            fragment);
  }
  toml_free(&doc);
}

static void test_toml_errors(void) {
  expect_toml_error("a = \"open\n", "line 1: unterminated string");
  expect_toml_error("\n\nkey 1\n", "line 3: expected '='");
  expect_toml_error("a = 1 2\n", "unexpected");
  expect_toml_error("a = \"\\q\"\n", "invalid escape");
  expect_toml_error("[table\n", "expected ']'");
  expect_toml_error("a = nope\n", "invalid value");
  expect_toml_error("a = \n", "expected a value");
  expect_toml_error("a = {b = 1\n", "inline table");
}

/* Config */

static int load(config *cfg, const char *content, char *err, size_t size) {
  char path[512];
  snprintf(path, sizeof(path), "%s/test.toml", tmpdir);
  write_file(path, content);
  return config_load(cfg, path, err, size);
}

static void expect_config_error(const char *content, const char *fragment) {
  static config cfg;
  char err[512] = "";
  int rc = load(&cfg, content, err, sizeof(err));
  CHECK(rc != 0);
  if (rc != 0 && !strstr(err, fragment)) {
    failures++;
    fprintf(stderr, "error \"%s\" does not mention \"%s\"\n", err, fragment);
  }
}

static void test_config_values(void) {
  static config cfg;
  char err[512] = "";
  int rc = load(&cfg,
                "[general]\n"
                "interval = 250\n"
                "spacing = 0\n"
                "fit = false\n"
                "blocks = \"#\"\n"
                "colors = 16\n"
                "locale = \"\"\n"
                "[line1]\n"
                "format = \"%I:%M\"\n"
                "scale = 3\n"
                "color = \"bright_red\"\n"
                "[line1.colors]\n"
                "\"%I\" = \"12\"\n"
                "\":\" = \"Gray\"\n"
                "\"%I\" = 200\n"
                "[line2]\n"
                "enabled = false\n",
                err, sizeof(err));
  CHECK(rc == 0);
  if (rc != 0) {
    fprintf(stderr, "  error: %s\n", err);
    return;
  }
  CHECK(cfg.interval_ms == 250);
  CHECK(cfg.fit == 0);
  CHECK(cfg.blocks == BLOCKS_ASCII && cfg.block_char == '#');
  CHECK(cfg.colors == 16);
  CHECK(cfg.spacing == 0);
  CHECK_STR(cfg.locale, "");
  CHECK_STR(cfg.lines[0].format, "%I:%M");
  CHECK(cfg.lines[0].scale == 3);
  CHECK(cfg.lines[0].color == 9);
  CHECK(cfg.lines[0].ncolors == 2);
  CHECK_STR(cfg.lines[0].colors[0].key, "%I");
  CHECK(cfg.lines[0].colors[0].color == 200);
  CHECK(cfg.lines[0].colors[1].color == 8);
  CHECK(cfg.lines[1].enabled == 0);
  // Untouched values keep their defaults
  CHECK_STR(cfg.lines[1].format, "%Y-%m-%d");
  CHECK(cfg.lines[1].scale == 1);
}

static void test_config_errors(void) {
  expect_config_error("[line1]\nscale = 0\n", "line1.scale");
  expect_config_error("[line1]\nscale = 51\n", "line1.scale");
  expect_config_error("[line1]\nscale = \"2\"\n", "line1.scale");
  expect_config_error("[line2]\ncolor = 256\n", "line2.color");
  expect_config_error("[line2]\ncolor = \"pink\"\n", "line2.color");
  expect_config_error("[line1]\nenabled = 1\n", "line1.enabled");
  expect_config_error("[line1]\nformat = 5\n", "line1.format");
  expect_config_error("[line1.colors]\n\"%H\" = -1\n", "line1.colors");
  expect_config_error("[line1]\ncolour = 3\n", "unknown option line1.colour");
  expect_config_error("[line3]\nformat = \"x\"\n", "unknown option line3");
  expect_config_error("[general]\ninterval = 5\n", "general.interval");
  expect_config_error("[general]\nspacing = -1\n", "general.spacing");
  expect_config_error("[general]\nfit = 1\n", "general.fit");
  expect_config_error("[general]\nblocks = \"big\"\n", "general.blocks");
  expect_config_error("[general]\nblocks = \" \"\n", "general.blocks");
  expect_config_error("[general]\nblocks = 1\n", "general.blocks");
  expect_config_error("[general]\ncolors = 88\n", "general.colors");
  expect_config_error("[general]\ncolors = \"many\"\n", "general.colors");
  expect_config_error("[general]\nfoo = 1\n", "unknown option general.foo");
  expect_config_error("[line1]\nformat = [1]\n", "unknown option");
  expect_config_error("a = \"\n", "test.toml: line 1");

  static config cfg;
  char err[512] = "";
  CHECK(config_load(&cfg, "/nonexistent/termtime.toml", err, sizeof(err)) !=
        0);
  CHECK(strstr(err, "No such file") != NULL);
}

static void test_config_lookup(void) {
  char xdg[512], home[512], xdg_file[600], home_file[600], found[4096];
  snprintf(xdg, sizeof(xdg), "%s/xdg", tmpdir);
  snprintf(home, sizeof(home), "%s/home", tmpdir);
  snprintf(xdg_file, sizeof(xdg_file), "%s/termtime/config.toml", xdg);
  snprintf(home_file, sizeof(home_file), "%s/.config/termtime/config.toml",
           home);

  char dir[600];
  snprintf(dir, sizeof(dir), "%s/termtime", xdg);
  mkdirs(dir);
  snprintf(dir, sizeof(dir), "%s/.config/termtime", home);
  mkdirs(dir);

  setenv("XDG_CONFIG_HOME", xdg, 1);
  setenv("HOME", home, 1);

  // HOME is used while XDG has no config
  write_file(home_file, "[line1]\nformat = \"home\"\n");
  CHECK(config_find(found, sizeof(found)) == 0);
  CHECK_STR(found, home_file);

  // XDG wins over HOME
  write_file(xdg_file, "[line1]\nformat = \"xdg\"\n");
  CHECK(config_find(found, sizeof(found)) == 0);
  CHECK_STR(found, xdg_file);

  static config cfg;
  char err[512] = "";
  CHECK(config_load(&cfg, NULL, err, sizeof(err)) == 0);
  CHECK_STR(cfg.path, xdg_file);
  CHECK_STR(cfg.lines[0].format, "xdg");
  CHECK(cfg.blocks == BLOCKS_AUTO && cfg.colors == COLORS_AUTO && cfg.fit == 1);

  // An empty XDG_CONFIG_HOME is ignored
  setenv("XDG_CONFIG_HOME", "", 1);
  CHECK(config_find(found, sizeof(found)) == 0);
  CHECK_STR(found, home_file);

  // Without user configs the system config or the defaults are used
  unsetenv("XDG_CONFIG_HOME");
  unlink(home_file);
  int rc = config_find(found, sizeof(found));
  if (rc == 0) {
    CHECK_STR(found, SYSCONFDIR "/termtime/config.toml");
  } else {
    CHECK(config_load(&cfg, NULL, err, sizeof(err)) == 0);
    CHECK_STR(cfg.path, "");
    CHECK_STR(cfg.lines[0].format, "%H:%M:%S");
  }
  unlink(xdg_file);
}

static void test_shipped_config(void) {
  static config cfg;
  char err[512] = "";
  int rc = config_load(&cfg, "config/config.toml", err, sizeof(err));
  CHECK(rc == 0);
  if (rc != 0) {
    fprintf(stderr, "  error: %s\n", err);
    return;
  }
  CHECK(cfg.lines[0].enabled && cfg.lines[1].enabled);
  CHECK(cfg.lines[0].ncolors > 0);
}

/* Rendering */

static clock_time fixed_time(void) {
  // Saturday 2026-03-07 09:05:03.123456789, UTC+05:30
  clock_time t;
  memset(&t, 0, sizeof(t));
  t.tm.tm_year = 2026 - 1900;
  t.tm.tm_mon = 2;
  t.tm.tm_mday = 7;
  t.tm.tm_hour = 9;
  t.tm.tm_min = 5;
  t.tm.tm_sec = 3;
  t.tm.tm_wday = 6;
  t.tm.tm_yday = 65;
  t.tm.tm_gmtoff = 5 * 3600 + 30 * 60;
  t.tm.tm_zone = "IST";
  t.nsec = 123456789;
  return t;
}

static line_config make_line(const char *format, int color) {
  line_config l;
  memset(&l, 0, sizeof(l));
  l.enabled = 1;
  l.scale = 1;
  l.color = color;
  snprintf(l.format, sizeof(l.format), "%s", format);
  return l;
}

static void add_color(line_config *l, const char *key, int color) {
  snprintf(l->colors[l->ncolors].key, COLOR_KEY_MAX, "%s", key);
  l->colors[l->ncolors].color = color;
  l->ncolors++;
}

/**
 * Expands a line into "[color]text[color]text..." for easy comparison.
 */
static const char *expand(const line_config *l) {
  static char out[2048];
  segment segs[FORMAT_MAX];
  clock_time t = fixed_time();
  int n = expand_line(l, &t, segs, FORMAT_MAX);
  out[0] = '\0';
  for (int i = 0; i < n; i++) {
    char part[512];
    snprintf(part, sizeof(part), "[%d]%s", segs[i].color,
             segs[i].text.data ? segs[i].text.data : "");
    strncat(out, part, sizeof(out) - strlen(out) - 1);
    buf_free(&segs[i].text);
  }
  return out;
}

static void test_expand(void) {
  line_config l = make_line("%H:%M:%S", 15);
  CHECK_STR(expand(&l), "[15]09:05:03");

  add_color(&l, "%H", 1);
  add_color(&l, "%M", 2);
  add_color(&l, "%S", 3);
  add_color(&l, ":", 8);
  CHECK_STR(expand(&l), "[1]09[8]:[2]05[8]:[3]03");

  // The longest key wins
  add_color(&l, "%M:%S", 4);
  CHECK_STR(expand(&l), "[1]09[8]:[4]05:03");

  // Flagged conversions fall back to the plain key
  l = make_line("%-d.%_m.", 7);
  add_color(&l, "%d", 1);
  add_color(&l, "%m", 2);
  CHECK_STR(expand(&l), "[1]7[7].[2] 3[7].");

  // An exact flagged key beats the fallback
  add_color(&l, "%_m", 5);
  CHECK_STR(expand(&l), "[1]7[7].[5] 3[7].");

  // Adjacent pieces with the same color share a segment
  l = make_line("%Y-%m-%d", 7);
  add_color(&l, "-", 7);
  CHECK_STR(expand(&l), "[7]2026-03-07");

  l = make_line("%a %A %b %B %p %I %j %u %F %T %R %D %y %e %k %l %P", 7);
  CHECK_STR(expand(&l), "[7]Sat Saturday Mar March AM 09 066 6 2026-03-07 "
                        "09:05:03 09:05 03/07/26 26  7  9  9 am");

  l = make_line("%^a %#Z %10Y|%-H|%z|%Z", 7);
  CHECK_STR(expand(&l), "[7]SAT ist 0000002026|9|+0530|IST");
}

static void test_expand_extensions(void) {
  line_config l = make_line("%N %3N %6N %1N %q", 7);
  CHECK_STR(expand(&l), "[7]123456789 123 123456 1 1");

  l = make_line("%:z %::z %:::z", 7);
  CHECK_STR(expand(&l), "[7]+05:30 +05:30:00 +05:30");

  l = make_line("%3N", 7);
  add_color(&l, "%N", 4);
  CHECK_STR(expand(&l), "[4]123");

  l = make_line("%::z", 7);
  add_color(&l, "%z", 4);
  CHECK_STR(expand(&l), "[4]+05:30:00");
}

static void test_expand_literals(void) {
  line_config l = make_line("100%% done", 7);
  CHECK_STR(expand(&l), "[7]100% done");

  l = make_line("50%", 7);
  CHECK_STR(expand(&l), "[7]50%");

  l = make_line("a%nb%tc", 7);
  CHECK_STR(expand(&l), "[7]a b c");

  // Each non-ASCII character becomes one unknown glyph
  l = make_line("\xc5\x99\xc3\xad" "x\xe2\x82\xac", 7);
  CHECK_STR(expand(&l), "[7]\x7f\x7fx\x7f");

  // Color keys may be non-ASCII
  add_color(&l, "\xe2\x82\xac", 3);
  CHECK_STR(expand(&l), "[7]\x7f\x7fx[3]\x7f");

  l = make_line("", 7);
  CHECK_STR(expand(&l), "");
}

/**
 * Visible terminal columns of a rendered row: escape
 * sequences are skipped and UTF-8 characters count once.
 */
static int visible_columns(const char *s) {
  int cols = 0;
  while (*s) {
    if (*s == '\033') {
      while (*s && *s != 'm') {
        s++;
      }
      if (*s) {
        s++;
      }
      continue;
    }
    if (((unsigned char)*s & 0xC0) != 0x80) {
      cols++;
    }
    s++;
  }
  return cols;
}

static const output_style unicode256 = {BLOCKS_UNICODE, 0, 256, 0};

static const char *styled(const char *row, output_style st) {
  static buffer out;
  buf_free(&out);
  buf_puts(&out, "");
  style_row(row, &st, &out);
  return out.data;
}

static void test_style(void) {
  // Basic colors stay, 256 colors map to the nearest one
  CHECK(color_to_basic(3, 16) == 3);
  CHECK(color_to_basic(12, 16) == 12);
  CHECK(color_to_basic(12, 8) == 4);
  CHECK(color_to_basic(8, 8) == 7);
  CHECK(color_to_basic(15, 8) == 7);
  CHECK(color_to_basic(196, 16) == 9);
  CHECK(color_to_basic(196, 8) == 1);
  CHECK(color_to_basic(46, 16) == 10);
  CHECK(color_to_basic(21, 16) == 12);
  CHECK(color_to_basic(232, 16) == 0);
  CHECK(color_to_basic(255, 16) == 15);
  CHECK(color_to_basic(244, 16) == 8);

  const char *sample = "\033[38;5;196m\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\033[0m";
  CHECK_STR(styled(sample, unicode256), sample);
  CHECK_STR(styled(sample, (output_style){BLOCKS_REVERSE, 0, 256, 0}),
            "\033[38;5;196m\033[7m  \033[27m \033[7m \033[27m\033[0m");
  CHECK_STR(styled(sample, (output_style){BLOCKS_ASCII, '#', 256, 0}),
            "\033[38;5;196m## #\033[0m");
  CHECK_STR(styled(sample, (output_style){BLOCKS_UNICODE, 0, 16, 0}),
            "\033[91m\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\033[0m");
  CHECK_STR(styled(sample, (output_style){BLOCKS_ASCII, '@', 16, 1}),
            "\033[1;31m@@ @\033[0m");
  CHECK_STR(styled(sample, (output_style){BLOCKS_ASCII, '@', 8, 1}),
            "\033[31m@@ @\033[0m");
  CHECK_STR(styled("\033[38;5;2m \033[0m", (output_style){BLOCKS_REVERSE, 0, 16, 0}),
            "\033[32m \033[0m");

  // Rendered rows keep their width in every style
  clock_time t = fixed_time();
  line_config l = make_line("%H:%M", 196);
  output_style styles[] = {
      {BLOCKS_UNICODE, 0, 256, 0},
      {BLOCKS_REVERSE, 0, 16, 1},
      {BLOCKS_ASCII, '#', 8, 0},
  };
  for (size_t i = 0; i < sizeof(styles) / sizeof(styles[0]); i++) {
    rendered_line r = {0};
    render_line(&l, &t, 2, &styles[i], &r);
    CHECK(r.width == 5 * (FONT_WIDTH + 1) * 2 - 2);
    for (int row = 0; row < r.rows; row++) {
      CHECK(visible_columns(r.text[row].data) == r.width + 2);
    }
    rendered_free(&r);
  }
}

static void test_render(void) {
  clock_time t = fixed_time();
  rendered_line r = {0};

  for (int scale = 1; scale <= 4; scale++) {
    line_config l = make_line("%H:%M:%S", 15);
    l.scale = scale;
    add_color(&l, ":", 8);
    add_color(&l, "%M", 10);

    CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 1);
    CHECK(r.rows == FONT_HEIGHT * scale);
    CHECK(r.width == 8 * (FONT_WIDTH + 1) * scale - scale);
    for (int row = 0; row < r.rows; row++) {
      CHECK(visible_columns(r.text[row].data) == r.width + scale);
    }
    CHECK(strstr(r.text[0].data, "\033[38;5;10m") != NULL);
    CHECK(strstr(r.text[0].data, "\033[38;5;8m") != NULL);

    // Same time again: nothing to redraw
    CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 0);
  }

  line_config l = make_line("%S", 15);
  CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 1);
  t.tm.tm_sec = 4;
  CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 1);
  // Only the color changing is a change too
  l.color = 1;
  CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 1);
  rendered_free(&r);

  // Non-ASCII renders one glyph wide
  l = make_line("\xc5\x99", 15);
  CHECK(render_line(&l, &t, l.scale, &unicode256, &r) == 1);
  CHECK(r.width == FONT_WIDTH);
  rendered_free(&r);

  // Disabled and empty lines have no rows
  l = make_line("%H", 15);
  l.enabled = 0;
  render_line(&l, &t, l.scale, &unicode256, &r);
  CHECK(r.rows == 0 && r.width == 0);
  rendered_free(&r);
  l = make_line("", 15);
  render_line(&l, &t, l.scale, &unicode256, &r);
  CHECK(r.rows == 0 && r.width == 0);
  rendered_free(&r);
}

static void test_fit(void) {
  clock_time t = fixed_time();
  line_config l = make_line("%H:%M:%S", 15);
  CHECK(line_glyphs(&l, &t) == 8);
  l.enabled = 0;
  CHECK(line_glyphs(&l, &t) == 0);

  // 8 glyphs are 55 columns at scale 1, 10 glyphs 69
  int glyphs[2] = {8, 10};
  int s[2];

  s[0] = 2, s[1] = 1;
  fit_scales(glyphs, 2, 1, 30, 120, s);
  CHECK(s[0] == 2 && s[1] == 1); // 110 columns fit into 120

  s[0] = 2, s[1] = 1;
  fit_scales(glyphs, 2, 1, 30, 100, s);
  CHECK(s[0] == 1 && s[1] == 1); // 110 does not fit into 100

  s[0] = 5, s[1] = 3;
  fit_scales(glyphs, 2, 1, 100, 300, s);
  CHECK(s[0] == 5 && s[1] == 3); // 275 and 207 columns fit

  s[0] = 9, s[1] = 9;
  fit_scales(glyphs, 2, 1, 100, 300, s);
  CHECK(s[0] == 5 && s[1] == 4); // shrunk to the width

  // Height: 5*4 + 2 + 5*4 = 42 rows into 30, the biggest shrinks first
  s[0] = 4, s[1] = 4;
  fit_scales(glyphs, 2, 2, 30, 1000, s);
  CHECK(5 * s[0] + 2 + 5 * s[1] <= 30);
  CHECK(s[0] == 3 && s[1] == 2);

  // Never below scale 1, even when it does not fit
  s[0] = 3, s[1] = 3;
  fit_scales(glyphs, 2, 1, 3, 10, s);
  CHECK(s[0] == 1 && s[1] == 1);

  // Disabled lines take no space
  int one[2] = {8, 0};
  s[0] = 6, s[1] = 6;
  fit_scales(one, 2, 5, 30, 1000, s);
  CHECK(s[0] == 6);
  fit_scales(one, 2, 5, 29, 1000, s);
  CHECK(s[0] == 5);
}

static void test_block_height(void) {
  rendered_line lines[2] = {{.rows = 10}, {.rows = 5}};
  CHECK(block_height(2, lines, 2) == 17);
  CHECK(block_height(0, lines, 2) == 15);
  lines[1].rows = 0;
  CHECK(block_height(2, lines, 2) == 10);
  lines[0].rows = 0;
  lines[1].rows = 5;
  CHECK(block_height(2, lines, 2) == 5);
}

int main(void) {
  const char *base = getenv("TMPDIR");
  snprintf(tmpdir, sizeof(tmpdir), "%s/termtime-test-XXXXXX",
           base && base[0] ? base : "/tmp");
  if (!mkdtemp(tmpdir)) {
    perror("mkdtemp");
    return 1;
  }

  test_toml_values();
  test_toml_errors();
  test_config_values();
  test_config_errors();
  test_config_lookup();
  test_shipped_config();
  test_expand();
  test_expand_extensions();
  test_expand_literals();
  test_render();
  test_style();
  test_fit();
  test_block_height();

  char cmd[600];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
  if (system(cmd) != 0) {
    fprintf(stderr, "could not remove %s\n", tmpdir);
  }

  printf("unit tests: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
