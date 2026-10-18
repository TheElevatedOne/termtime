#ifndef TERMTIME_RENDER_H
#define TERMTIME_RENDER_H

#include "config.h"

#include <stddef.h>
#include <time.h>

/**
 * Growable NUL-terminated byte buffer.
 */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} buffer;

void buf_append(buffer *b, const char *s, size_t n);
void buf_puts(buffer *b, const char *s);
void buf_printf(buffer *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void buf_free(buffer *b);

/**
 * Point in time the lines are rendered for.
 */
typedef struct {
  struct tm tm;
  long nsec;
} clock_time;

/**
 * Fills t with the current local time.
 */
void clock_now(clock_time *t);

/**
 * A run of expanded text drawn in one color.
 */
typedef struct {
  buffer text;
  int color;
} segment;

/**
 * Splits the line format into colored segments and expands the
 * date/time conversions in them. Returns the number of segments.
 * Every segment's text must be released with buf_free().
 */
int expand_line(const line_config *line, const clock_time *t, segment *segs,
                int max);

/**
 * A line rendered to blockfont rows.
 */
typedef struct {
  buffer signature; // expanded text and colors, to detect changes
  int rows;
  int width; // visible terminal columns
  buffer *text;
} rendered_line;

/**
 * How rendered rows are written for the terminal in use.
 */
typedef struct {
  blocks_mode blocks; // never BLOCKS_AUTO
  char block_char;    // for BLOCKS_ASCII
  int colors;         // 8, 16 or 256
  int bold_bright;    // bright colors as bold + normal color (Linux console)
} output_style;

/**
 * Nearest of the first `count` (8 or 16) ANSI colors to a 256 color.
 */
int color_to_basic(int color, int count);

/**
 * Rewrites a row rendered by blockfont for the output style:
 * blocks become U+2588, reverse video spaces or an ASCII character
 * and 256 color escapes are mapped down to 16 or 8 colors.
 */
void style_row(const char *row, const output_style *st, buffer *out);

/**
 * Number of glyphs the line expands to, 0 when disabled.
 */
int line_glyphs(const line_config *line, const clock_time *t);

/**
 * Lowers scales (as wanted, at least 1) so lines of the given glyph
 * counts fit into rows x cols: each line is shrunk to the width, then
 * the biggest lines are shrunk until the height fits.
 */
void fit_scales(const int *glyphs, int count, int spacing, int rows, int cols,
                int *scales);

/**
 * Renders a line at the given scale into r. Returns 1 when the result
 * differs from what r held before, 0 when nothing changed.
 * A disabled or empty line renders to 0 rows.
 */
int render_line(const line_config *line, const clock_time *t, int scale,
                const output_style *st, rendered_line *r);

void rendered_free(rendered_line *r);

/**
 * Terminal rows taken by all rendered lines and the gaps between them.
 */
int block_height(int spacing, const rendered_line *lines, int count);

#endif // !TERMTIME_RENDER_H
