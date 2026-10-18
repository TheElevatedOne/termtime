#include "config.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef TERMTIME_VERSION
#define TERMTIME_VERSION "dev"
#endif

static void term_size(int *rows, int *cols) {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 &&
      w.ws_col > 0) {
    *rows = w.ws_row;
    *cols = w.ws_col;
  } else {
    *rows = 24;
    *cols = 80;
  }
}

static void write_all(const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(STDOUT_FILENO, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    off += (size_t)n;
  }
}

/**
 * Draws both lines centered on the screen. Every terminal row is
 * cleared and redrawn in a single write, inside a synchronized update
 * for terminals that support it, so the clock does not flicker.
 */
static void draw_frame(const config *cfg, const rendered_line *lines) {
  int rows, cols;
  term_size(&rows, &cols);

  int top = (rows - block_height(cfg->spacing, lines, LINE_COUNT)) / 2;
  if (top < 0) {
    top = 0;
  }

  // Which line and row of it lands on each terminal row
  int *line_at = malloc((size_t)rows * sizeof(int));
  int *row_at = malloc((size_t)rows * sizeof(int));
  if (!line_at || !row_at) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  for (int y = 0; y < rows; y++) {
    line_at[y] = -1;
  }
  int y = top, shown = 0;
  for (int i = 0; i < LINE_COUNT; i++) {
    if (lines[i].rows == 0) {
      continue;
    }
    if (shown++) {
      y += cfg->spacing;
    }
    for (int row = 0; row < lines[i].rows; row++, y++) {
      if (y < rows) {
        line_at[y] = i;
        row_at[y] = row;
      }
    }
  }

  buffer frame = {0};
  buf_puts(&frame, "\033[?2026h");
  for (y = 0; y < rows; y++) {
    buf_printf(&frame, "\033[%d;1H\033[2K", y + 1);
    if (line_at[y] < 0) {
      continue;
    }
    const rendered_line *l = &lines[line_at[y]];
    int left = (cols - l->width) / 2;
    if (left > 0) {
      // Absolute positioning, which even a VT100 understands
      buf_printf(&frame, "\033[%d;%dH", y + 1, left + 1);
    }
    buf_append(&frame, l->text[row_at[y]].data, l->text[row_at[y]].len);
  }
  buf_puts(&frame, "\033[?2026l");

  write_all(frame.data, frame.len);
  buf_free(&frame);
  free(line_at);
  free(row_at);
}

/**
 * Prints the lines once to stdout, centered horizontally
 * when stdout is a terminal.
 */
static void print_once(const config *cfg, const rendered_line *lines) {
  int rows, cols = 0;
  if (isatty(STDOUT_FILENO)) {
    term_size(&rows, &cols);
  }
  int shown = 0;
  for (int i = 0; i < LINE_COUNT; i++) {
    const rendered_line *l = &lines[i];
    if (l->rows == 0) {
      continue;
    }
    if (shown++) {
      for (int s = 0; s < cfg->spacing; s++) {
        putchar('\n');
      }
    }
    int left = cols > l->width ? (cols - l->width) / 2 : 0;
    for (int row = 0; row < l->rows; row++) {
      printf("%*s%s\n", left, "", l->text[row].data);
    }
  }
}

static output_style style;

static int starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

/**
 * Colors the terminal supports, judged from TERM and COLORTERM.
 * Unknown terminals are assumed to be modern 256 color ones.
 */
static int detect_colors(const char *term) {
  const char *colorterm = getenv("COLORTERM");
  if (colorterm && colorterm[0] != '\0') {
    return 256;
  }
  if (strstr(term, "256color") || strstr(term, "direct")) {
    return 256;
  }
  if (strcmp(term, "linux") == 0) {
    return 16;
  }
  if (term[0] == '\0' || strcmp(term, "dumb") == 0 || starts_with(term, "vt") ||
      starts_with(term, "ansi") || starts_with(term, "cons") ||
      starts_with(term, "sun")) {
    return 8;
  }
  return 256;
}

static int utf8_locale(void) {
  setlocale(LC_CTYPE, "");
  const char *codeset = nl_langinfo(CODESET);
  int utf8 = codeset && (strcasecmp(codeset, "UTF-8") == 0 ||
                         strcasecmp(codeset, "utf8") == 0);
  setlocale(LC_CTYPE, "C");
  return utf8;
}

/**
 * Applies the locale and picks how to draw for this terminal.
 */
static void apply_environment(const config *cfg) {
  // "" uses the locale from the environment
  if (!setlocale(LC_TIME, cfg->locale)) {
    fprintf(stderr, "termtime: locale \"%s\" is not available, using C\n",
            cfg->locale);
    setlocale(LC_TIME, "C");
  }

  const char *term = getenv("TERM");
  if (!term) {
    term = "";
  }
  int linux_console = strcmp(term, "linux") == 0;

  style.blocks = cfg->blocks;
  style.block_char = cfg->block_char;
  if (style.blocks == BLOCKS_AUTO) {
    // Without UTF-8 the block glyph would be garbage, reverse
    // video spaces look the same and work with any font
    style.blocks = utf8_locale() ? BLOCKS_UNICODE : BLOCKS_REVERSE;
  }
  style.colors = cfg->colors == COLORS_AUTO ? detect_colors(term) : cfg->colors;
  // The Linux console shows bright colors for bold text
  style.bold_bright = linux_console;
}

/**
 * Renders both lines for the current time, shrunk to fit into
 * rows x cols when general.fit is on. Returns 1 when anything changed.
 */
static int render_all(const config *cfg, rendered_line *lines, int rows,
                      int cols) {
  clock_time now;
  clock_now(&now);

  int glyphs[LINE_COUNT], scales[LINE_COUNT];
  for (int i = 0; i < LINE_COUNT; i++) {
    glyphs[i] = line_glyphs(&cfg->lines[i], &now);
    scales[i] = cfg->lines[i].scale;
  }
  if (cfg->fit) {
    fit_scales(glyphs, LINE_COUNT, cfg->spacing, rows, cols, scales);
  }

  int changed = 0;
  for (int i = 0; i < LINE_COUNT; i++) {
    changed |= render_line(&cfg->lines[i], &now, scales[i], &style, &lines[i]);
  }
  return changed;
}

static void free_all(rendered_line *lines) {
  for (int i = 0; i < LINE_COUNT; i++) {
    rendered_free(&lines[i]);
  }
}

/* Terminal handling */

static struct termios saved_termios;
static int termios_saved = 0;
static int signal_pipe[2] = {-1, -1};
static volatile sig_atomic_t got_resize = 0;
static volatile sig_atomic_t got_quit = 0;
static volatile sig_atomic_t got_reload = 0;

// Only async-signal-safe calls, this also runs from the SIGTSTP handler
static void leave_terminal(void) {
  // Reset colors, clear (the Linux console has no alternate screen),
  // show cursor, enable line wrap, leave alternate screen
  static const char seq[] =
      "\033[0m\033[H\033[2J\033[?25h\033[?7h\033[?1049l";
  ssize_t rc = write(STDOUT_FILENO, seq, sizeof(seq) - 1);
  (void)rc;
  if (termios_saved) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
  }
}

static void enter_terminal(void) {
  if (termios_saved) {
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }
  // Enter alternate screen, hide cursor, disable line wrap
  static const char seq[] = "\033[?1049h\033[?25l\033[?7l";
  ssize_t rc = write(STDOUT_FILENO, seq, sizeof(seq) - 1);
  (void)rc;
}

static void wake_loop(void) {
  ssize_t rc = write(signal_pipe[1], "x", 1);
  (void)rc;
}

static void on_signal(int sig);

static void set_handler(int sig, void (*handler)(int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
}

static void on_signal(int sig) {
  int saved = errno;
  switch (sig) {
  case SIGWINCH:
    got_resize = 1;
    break;
  case SIGUSR1:
    got_reload = 1;
    break;
  case SIGTSTP: {
    // Give the terminal back and stop for real. Execution continues
    // here once resumed, or right away when the stop is discarded
    // (orphaned process group), so take the terminal again after.
    leave_terminal();
    set_handler(SIGTSTP, SIG_DFL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(SIGTSTP);
    sigprocmask(SIG_BLOCK, &set, NULL);
    set_handler(SIGTSTP, on_signal);
    enter_terminal();
    got_resize = 1;
    break;
  }
  case SIGCONT:
    // Resumed after SIGSTOP, redraw in case the screen was changed
    got_resize = 1;
    break;
  default:
    got_quit = 1;
    break;
  }
  wake_loop();
  errno = saved;
}

static void setup_signals(void) {
  if (pipe(signal_pipe) != 0) {
    perror("pipe");
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < 2; i++) {
    fcntl(signal_pipe[i], F_SETFL, O_NONBLOCK);
    fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
  }

  int sigs[] = {SIGINT,   SIGTERM, SIGHUP,  SIGQUIT,
                SIGWINCH, SIGUSR1, SIGTSTP, SIGCONT};
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    set_handler(sigs[i], on_signal);
  }
}

/**
 * Milliseconds until the next multiple of interval_ms
 * of wall clock time, so seconds tick on time.
 */
static int ms_until_tick(int interval_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  long long next = (now / interval_ms + 1) * interval_ms;
  return (int)(next - now) + 1;
}

static void reload_config(config *cfg, const char *path) {
  static config next;
  char err[512] = "";
  // On errors keep running with the previous config
  if (config_load(&next, path, err, sizeof(err)) == 0) {
    *cfg = next;
    apply_environment(cfg);
  }
}

/**
 * Handles keys read from the terminal. A lone Esc quits, while escape
 * sequences (arrow keys, the mouse wheel on the alternate screen,
 * Alt+key) are skipped.
 */
static void handle_keys(const char *keys, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = keys[i];
    if (c == '\033') {
      if (i + 1 == n) {
        got_quit = 1;
      } else if (keys[i + 1] == '[') {
        // CSI: parameters up to a final byte in 0x40-0x7e
        i += 2;
        while (i < n && !(keys[i] >= 0x40 && keys[i] <= 0x7e)) {
          i++;
        }
      } else if (keys[i + 1] == 'O') {
        i += 2; // SS3: one more byte
      } else {
        i++; // Alt+key
      }
    } else if (c == 'q' || c == 'Q') {
      got_quit = 1;
    } else if (c == 'r' || c == 'R') {
      got_reload = 1;
    }
  }
}

static void run(config *cfg, const char *path) {
  rendered_line lines[LINE_COUNT] = {0};

  if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
    termios_saved = 1;
  }
  int read_keys = termios_saved;

  setup_signals();
  atexit(leave_terminal);
  enter_terminal();

  int redraw = 1;
  while (!got_quit) {
    if (got_reload) {
      got_reload = 0;
      reload_config(cfg, path);
      free_all(lines);
    }
    if (got_resize) {
      got_resize = 0;
      redraw = 1;
    }
    int rows, cols;
    term_size(&rows, &cols);
    if (render_all(cfg, lines, rows, cols) || redraw) {
      draw_frame(cfg, lines);
      redraw = 0;
    }

    struct pollfd fds[2] = {
        {.fd = signal_pipe[0], .events = POLLIN},
        {.fd = STDIN_FILENO, .events = POLLIN},
    };
    int rc = poll(fds, read_keys ? 2 : 1, ms_until_tick(cfg->interval_ms));
    if (rc < 0 && errno != EINTR) {
      break;
    }
    if (rc <= 0) {
      continue;
    }

    if (fds[0].revents & POLLIN) {
      char drain[64];
      while (read(signal_pipe[0], drain, sizeof(drain)) > 0) {
      }
    }
    if (read_keys && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      char keys[64];
      ssize_t n = read(STDIN_FILENO, keys, sizeof(keys));
      if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        read_keys = 0; // stdin closed, stop polling it
      }
      handle_keys(keys, (size_t)(n > 0 ? n : 0));
    }
  }

  free_all(lines);
}

static void usage(FILE *out) {
  fprintf(out,
          "Usage: termtime [OPTIONS]\n"
          "\n"
          "Big block-letter clock for the terminal.\n"
          "\n"
          "Options:\n"
          "  -c, --config FILE   use FILE instead of the default config\n"
          "  -1, --once          print the lines once and exit\n"
          "  -p, --path          print the config file in use and exit\n"
          "  -h, --help          show this help\n"
          "  -v, --version       show the version\n"
          "\n"
          "Config lookup order:\n"
          "  $XDG_CONFIG_HOME/termtime/config.toml\n"
          "  $HOME/.config/termtime/config.toml\n"
          "  %s/termtime/config.toml\n"
          "\n"
          "Keys: q / Esc to quit, r to reload the config.\n",
          SYSCONFDIR);
}

int main(int argc, char **argv) {
  const char *path = NULL;
  int once = 0, show_path = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "termtime: %s needs a file\n", a);
        return EXIT_FAILURE;
      }
      path = argv[i];
    } else if (strncmp(a, "--config=", 9) == 0) {
      path = a + 9;
    } else if (strcmp(a, "-1") == 0 || strcmp(a, "--once") == 0) {
      once = 1;
    } else if (strcmp(a, "-p") == 0 || strcmp(a, "--path") == 0) {
      show_path = 1;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return EXIT_SUCCESS;
    } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
      printf("termtime %s\n", TERMTIME_VERSION);
      return EXIT_SUCCESS;
    } else {
      fprintf(stderr, "termtime: unknown option '%s'\n\n", a);
      usage(stderr);
      return EXIT_FAILURE;
    }
  }

  config cfg;
  char err[512] = "";
  if (config_load(&cfg, path, err, sizeof(err)) != 0) {
    fprintf(stderr, "termtime: %s\n", err);
    return EXIT_FAILURE;
  }

  if (show_path) {
    printf("%s\n", cfg.path[0] ? cfg.path : "(built-in defaults)");
    return EXIT_SUCCESS;
  }

  apply_environment(&cfg);

  if (once || !isatty(STDOUT_FILENO)) {
    rendered_line lines[LINE_COUNT] = {0};
    // Fit to the terminal width only, the output can scroll
    int rows = INT_MAX, cols = INT_MAX;
    if (isatty(STDOUT_FILENO)) {
      term_size(&rows, &cols);
      rows = INT_MAX;
    }
    render_all(&cfg, lines, rows, cols);
    print_once(&cfg, lines);
    free_all(lines);
    return EXIT_SUCCESS;
  }

  run(&cfg, path);
  return EXIT_SUCCESS;
}
