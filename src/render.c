#include "render.h"

#include <blockfont.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPAND_MAX 1024

// Byte the library has no glyph for, drawn as the "unknown" glyph
#define UNKNOWN_GLYPH '\x7f'

void buf_append(buffer *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (b->len + n + 1 > cap) {
      cap *= 2;
    }
    char *d = realloc(b->data, cap);
    if (!d) {
      perror("realloc");
      exit(EXIT_FAILURE);
    }
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

void buf_puts(buffer *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_printf(buffer *b, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n > 0) {
    buf_append(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
  }
}

void buf_free(buffer *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}

void clock_now(clock_time *t) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &t->tm);
  t->nsec = ts.tv_nsec;
}

/**
 * Parsed conversion specification, e.g. "%-3N" or "%::z".
 */
typedef struct {
  size_t len;  // bytes in the format, 1 when incomplete
  int width;   // -1 when not given
  int colons;  // number of ':' before the conversion
  char conv;
} spec;

static spec parse_spec(const char *p) {
  spec s = {1, -1, 0, '\0'};
  size_t i = 1;
  while (p[i] && strchr("-_0^#+", p[i])) {
    i++;
  }
  if (isdigit((unsigned char)p[i])) {
    s.width = 0;
    while (isdigit((unsigned char)p[i])) {
      if (s.width < 1000) {
        s.width = s.width * 10 + (p[i] - '0');
      }
      i++;
    }
  }
  while (p[i] == ':') {
    s.colons++;
    i++;
  }
  if (p[i] == 'E' || p[i] == 'O') {
    i++;
  }
  if (p[i] == '\0') {
    return s;
  }
  s.conv = p[i];
  s.len = i + 1;
  return s;
}

/**
 * Conversions date(1) has but strftime(3) does not:
 * %N nanoseconds (%3N milliseconds...), %q quarter,
 * %:z +hh:mm, %::z +hh:mm:ss, %:::z shortest offset.
 * Returns 1 when handled.
 */
static int expand_date_extension(buffer *out, const spec *s,
                                 const clock_time *t) {
  if (s->conv == 'N' && s->colons == 0) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%09ld", t->nsec);
    int width = (s->width < 0 || s->width > 9) ? 9 : s->width;
    buf_append(out, digits, (size_t)(width ? width : 9));
    return 1;
  }
  if (s->conv == 'q' && s->colons == 0) {
    buf_printf(out, "%d", t->tm.tm_mon / 3 + 1);
    return 1;
  }
  if (s->conv == 'z' && s->colons >= 1 && s->colons <= 3) {
    long off = t->tm.tm_gmtoff;
    char sign = off < 0 ? '-' : '+';
    if (off < 0) {
      off = -off;
    }
    long h = off / 3600, m = off / 60 % 60, sec = off % 60;
    if (s->colons == 1 || (s->colons == 3 && sec == 0 && m != 0)) {
      buf_printf(out, "%c%02ld:%02ld", sign, h, m);
    } else if (s->colons == 2 || (s->colons == 3 && sec != 0)) {
      buf_printf(out, "%c%02ld:%02ld:%02ld", sign, h, m, sec);
    } else {
      buf_printf(out, "%c%02ld", sign, h);
    }
    return 1;
  }
  return 0;
}

/**
 * Appends text, replacing each non-ASCII UTF-8 character with one
 * unknown glyph (the font only has ASCII) and tabs/newlines with spaces.
 */
static void append_glyphs(buffer *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n' || c == '\t') {
      buf_append(out, " ", 1);
    } else if (c >= 0x80) {
      // Skip the continuation bytes of this character
      while (i + 1 < n && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
        i++;
      }
      char u = UNKNOWN_GLYPH;
      buf_append(out, &u, 1);
    } else {
      buf_append(out, s + i, 1);
    }
  }
}

/**
 * Expands a piece of the format, which may hold literal text
 * and any number of conversions.
 */
static void expand_piece(buffer *out, const char *p, size_t len,
                         const clock_time *t) {
  const char *end = p + len;
  while (p < end) {
    if (*p != '%') {
      const char *lit = p;
      while (p < end && *p != '%') {
        p++;
      }
      append_glyphs(out, lit, (size_t)(p - lit));
      continue;
    }

    spec s = parse_spec(p);
    if (s.len == 1 || p + s.len > end) {
      // Incomplete specification, print as is
      append_glyphs(out, p, (size_t)(end - p));
      return;
    }

    buffer ext = {0};
    if (expand_date_extension(&ext, &s, t)) {
      append_glyphs(out, ext.data, ext.len);
    } else {
      char fmt[64], res[EXPAND_MAX];
      size_t n = 0;
      if (s.len < sizeof(fmt)) {
        memcpy(fmt, p, s.len);
        fmt[s.len] = '\0';
        n = strftime(res, sizeof(res), fmt, &t->tm);
      }
      append_glyphs(out, res, n);
    }
    buf_free(&ext);
    p += s.len;
  }
}

static const color_rule *find_rule(const line_config *line, const char *key) {
  for (int i = 0; i < line->ncolors; i++) {
    if (strcmp(line->colors[i].key, key) == 0) {
      return &line->colors[i];
    }
  }
  return NULL;
}

/**
 * Finds the longest color key that matches the format at p.
 */
static const color_rule *longest_rule(const line_config *line, const char *p) {
  const color_rule *best = NULL;
  size_t best_len = 0;
  for (int i = 0; i < line->ncolors; i++) {
    size_t len = strlen(line->colors[i].key);
    if (len > best_len && strncmp(p, line->colors[i].key, len) == 0) {
      best = &line->colors[i];
      best_len = len;
    }
  }
  return best;
}

int expand_line(const line_config *line, const clock_time *t, segment *segs,
                int max) {
  int count = 0;
  const char *p = line->format;

  while (*p) {
    size_t len;
    int color = line->color;
    const color_rule *rule = longest_rule(line, p);

    if (rule) {
      len = strlen(rule->key);
      color = rule->color;
    } else if (*p == '%') {
      spec s = parse_spec(p);
      len = s.len;
      // "%-d" falls back to the color of "%d", "%::z" to "%z"
      if (len > 2) {
        char base[3] = {'%', s.conv, '\0'};
        rule = find_rule(line, base);
        if (rule) {
          color = rule->color;
        }
      }
    } else {
      // Keep a UTF-8 character in one piece
      len = 1;
      while (((unsigned char)p[len] & 0xC0) == 0x80) {
        len++;
      }
    }

    if (count == 0 || segs[count - 1].color != color) {
      if (count == max) {
        break;
      }
      memset(&segs[count], 0, sizeof(segs[count]));
      segs[count].color = color;
      count++;
    }
    expand_piece(&segs[count - 1].text, p, len, t);
    p += len;
  }
  return count;
}

void rendered_free(rendered_line *r) {
  for (int i = 0; i < r->rows; i++) {
    buf_free(&r->text[i]);
  }
  free(r->text);
  buf_free(&r->signature);
  r->text = NULL;
  r->rows = 0;
  r->width = 0;
}

// RGB of a color from the 6x6x6 cube (16-231) or the gray ramp (232-255)
static void color_rgb(int c, int rgb[3]) {
  static const int levels[6] = {0, 95, 135, 175, 215, 255};
  if (c < 232) {
    c -= 16;
    rgb[0] = levels[c / 36];
    rgb[1] = levels[c / 6 % 6];
    rgb[2] = levels[c % 6];
  } else {
    rgb[0] = rgb[1] = rgb[2] = 8 + 10 * (c - 232);
  }
}

int color_to_basic(int color, int count) {
  int c;
  if (color < 16) {
    c = color;
  } else {
    int rgb[3];
    color_rgb(color, rgb);
    int max = rgb[0], min = rgb[0];
    for (int k = 1; k < 3; k++) {
      max = rgb[k] > max ? rgb[k] : max;
      min = rgb[k] < min ? rgb[k] : min;
    }
    if (max - min < 0x20) {
      // Gray: black, gray, white or bright white
      c = max < 0x40 ? 0 : max < 0xa0 ? 8 : max < 0xd8 ? 7 : 15;
    } else {
      // Channels close to the strongest one make the hue
      // (red 1, green 2, blue 4), bright ones are 8 higher
      c = 0;
      for (int k = 0; k < 3; k++) {
        if (rgb[k] * 2 >= max + min) {
          c |= 1 << k;
        }
      }
      if (max >= 0xd7) {
        c += 8;
      }
    }
  }
  if (count < 16 && c >= 8) {
    // Without bright colors gray becomes white, not black
    c = c == 8 ? 7 : c - 8;
  }
  return c;
}

static void style_color(int color, const output_style *st, buffer *out) {
  if (st->colors >= 256) {
    buf_printf(out, "\033[38;5;%dm", color);
    return;
  }
  int c = color_to_basic(color, st->colors >= 16 ? 16 : 8);
  if (c < 8) {
    buf_printf(out, "\033[3%dm", c);
  } else if (st->bold_bright) {
    buf_printf(out, "\033[1;3%dm", c - 8);
  } else {
    buf_printf(out, "\033[9%dm", c - 8);
  }
}

void style_row(const char *row, const output_style *st, buffer *out) {
  static const char block[] = "\xe2\x96\x88"; // U+2588
  const size_t block_len = sizeof(block) - 1;
  const char *p = row;

  while (*p) {
    if (p[0] == '\033' && p[1] == '[') {
      const char *end = p + 2;
      while (*end && *end != 'm') {
        end++;
      }
      int color;
      if (*end == 'm' && sscanf(p + 2, "38;5;%d", &color) == 1 && color >= 0 &&
          color <= 255) {
        style_color(color, st, out);
      } else {
        buf_append(out, p, (size_t)(end - p) + (*end ? 1 : 0));
      }
      p = *end ? end + 1 : end;
    } else if (strncmp(p, block, block_len) == 0) {
      size_t run = 0;
      while (strncmp(p, block, block_len) == 0) {
        run++;
        p += block_len;
      }
      if (st->blocks == BLOCKS_REVERSE) {
        buf_puts(out, "\033[7m");
      }
      for (size_t i = 0; i < run; i++) {
        if (st->blocks == BLOCKS_UNICODE) {
          buf_append(out, block, block_len);
        } else {
          buf_append(out, st->blocks == BLOCKS_ASCII ? &st->block_char : " ", 1);
        }
      }
      if (st->blocks == BLOCKS_REVERSE) {
        buf_puts(out, "\033[27m");
      }
    } else {
      buf_append(out, p, 1);
      p++;
    }
  }
}

int line_glyphs(const line_config *line, const clock_time *t) {
  if (!line->enabled) {
    return 0;
  }
  segment segs[FORMAT_MAX];
  int count = expand_line(line, t, segs, FORMAT_MAX);
  size_t glyphs = 0;
  for (int i = 0; i < count; i++) {
    glyphs += segs[i].text.len;
    buf_free(&segs[i].text);
  }
  return (int)glyphs;
}

void fit_scales(const int *glyphs, int count, int spacing, int rows, int cols,
                int *scales) {
  int height = 0, shown = 0;
  for (int i = 0; i < count; i++) {
    if (glyphs[i] == 0) {
      continue;
    }
    // A line is (FONT_WIDTH + 1) * glyphs - 1 columns wide at scale 1
    int unit = (FONT_WIDTH + 1) * glyphs[i] - 1;
    if (scales[i] > cols / unit) {
      scales[i] = cols / unit > 1 ? cols / unit : 1;
    }
    height += FONT_HEIGHT * scales[i] + (shown++ ? spacing : 0);
  }

  // Shrink the biggest line until everything fits vertically,
  // on ties the later line goes first so line 1 stays big
  while (height > rows) {
    int big = -1;
    for (int i = 0; i < count; i++) {
      if (glyphs[i] > 0 && scales[i] > 1 &&
          (big < 0 || scales[i] >= scales[big])) {
        big = i;
      }
    }
    if (big < 0) {
      break;
    }
    scales[big]--;
    height -= FONT_HEIGHT;
  }
}

int render_line(const line_config *line, const clock_time *t, int scale,
                const output_style *st, rendered_line *r) {
  segment segs[FORMAT_MAX];
  int count = line->enabled ? expand_line(line, t, segs, FORMAT_MAX) : 0;

  buffer sig = {0};
  buf_printf(&sig, "%d|", scale);
  for (int i = 0; i < count; i++) {
    buf_printf(&sig, "%d:", segs[i].color);
    if (segs[i].text.data) {
      buf_puts(&sig, segs[i].text.data);
    }
    buf_puts(&sig, "\x1f");
  }

  if (r->signature.data && strcmp(r->signature.data, sig.data) == 0) {
    buf_free(&sig);
    for (int i = 0; i < count; i++) {
      buf_free(&segs[i].text);
    }
    return 0;
  }

  rendered_free(r);
  r->signature = sig;

  size_t chars = 0;
  for (int i = 0; i < count; i++) {
    if (segs[i].text.len == 0) {
      continue;
    }
    fontized f = blockfont_color(segs[i].text.data, scale, segs[i].color);
    if (f.value == 0) {
      if (!r->text) {
        r->rows = f.rows;
        r->text = calloc((size_t)f.rows, sizeof(buffer));
        if (!r->text) {
          perror("calloc");
          exit(EXIT_FAILURE);
        }
      }
      for (int row = 0; row < f.rows && row < r->rows; row++) {
        style_row(f.text[row], st, &r->text[row]);
      }
      chars += segs[i].text.len;
    }
    free_blockfont(&f);
  }
  for (int i = 0; i < count; i++) {
    buf_free(&segs[i].text);
  }

  // Every glyph is followed by `scale` columns of spacing,
  // the trailing spacing of the last glyph is not counted
  if (chars > 0) {
    r->width = (int)chars * (FONT_WIDTH + 1) * scale - scale;
  }
  return 1;
}

int block_height(int spacing, const rendered_line *lines, int count) {
  int height = 0, shown = 0;
  for (int i = 0; i < count; i++) {
    if (lines[i].rows > 0) {
      height += lines[i].rows + (shown ? spacing : 0);
      shown++;
    }
  }
  return height;
}
