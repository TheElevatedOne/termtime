#include "config.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const struct {
  const char *name;
  int color;
} color_names[] = {
    {"black", 0},          {"red", 1},
    {"green", 2},          {"yellow", 3},
    {"blue", 4},           {"magenta", 5},
    {"cyan", 6},           {"white", 7},
    {"gray", 8},           {"grey", 8},
    {"bright_black", 8},   {"bright_red", 9},
    {"bright_green", 10},  {"bright_yellow", 11},
    {"bright_blue", 12},   {"bright_magenta", 13},
    {"bright_cyan", 14},   {"bright_white", 15},
};

static void set_defaults(config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->interval_ms = 1000;
  cfg->spacing = 1;
  cfg->fit = 1;
  // The font only has ASCII glyphs, so localized names are opt-in
  snprintf(cfg->locale, sizeof(cfg->locale), "C");

  line_config *l1 = &cfg->lines[0];
  l1->enabled = 1;
  snprintf(l1->format, sizeof(l1->format), "%%H:%%M:%%S");
  l1->scale = 2;
  l1->color = 15;

  line_config *l2 = &cfg->lines[1];
  l2->enabled = 1;
  snprintf(l2->format, sizeof(l2->format), "%%Y-%%m-%%d");
  l2->scale = 1;
  l2->color = 7;
}

static int file_exists(const char *path) { return access(path, R_OK) == 0; }

int config_find(char *out, size_t size) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0') {
    snprintf(out, size, "%s/termtime/config.toml", xdg);
    if (file_exists(out)) {
      return 0;
    }
  }

  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    snprintf(out, size, "%s/.config/termtime/config.toml", home);
    if (file_exists(out)) {
      return 0;
    }
  }

  snprintf(out, size, "%s/termtime/config.toml", SYSCONFDIR);
  if (file_exists(out)) {
    return 0;
  }

  out[0] = '\0';
  return -1;
}

/**
 * Accepts an integer 0-255, a numeric string or a color name.
 */
static int parse_color(const toml_entry *e, int *out) {
  if (e->type == TOML_INT) {
    if (e->ival < 0 || e->ival > 255) {
      return -1;
    }
    *out = (int)e->ival;
    return 0;
  }
  if (e->type != TOML_STRING) {
    return -1;
  }
  for (size_t i = 0; i < sizeof(color_names) / sizeof(color_names[0]); i++) {
    if (strcasecmp(e->str, color_names[i].name) == 0) {
      *out = color_names[i].color;
      return 0;
    }
  }
  char *end;
  long v = strtol(e->str, &end, 10);
  if (end != e->str && *end == '\0' && v >= 0 && v <= 255) {
    *out = (int)v;
    return 0;
  }
  return -1;
}

static int load_line(line_config *line, const char *name, const toml_entry *e,
                     const char *field, char *err, size_t errsize) {
  if (strcmp(field, "enabled") == 0) {
    if (e->type != TOML_BOOL) {
      snprintf(err, errsize, "%s.enabled must be true or false", name);
      return -1;
    }
    line->enabled = (int)e->ival;
  } else if (strcmp(field, "format") == 0) {
    if (e->type != TOML_STRING || strlen(e->str) >= sizeof(line->format)) {
      snprintf(err, errsize, "%s.format must be a string of at most %d bytes",
               name, FORMAT_MAX - 1);
      return -1;
    }
    memcpy(line->format, e->str, strlen(e->str) + 1);
  } else if (strcmp(field, "scale") == 0) {
    if (e->type != TOML_INT || e->ival < 1 || e->ival > SCALE_LIMIT) {
      snprintf(err, errsize, "%s.scale must be an integer from 1 to %d", name,
               SCALE_LIMIT);
      return -1;
    }
    line->scale = (int)e->ival;
  } else if (strcmp(field, "color") == 0) {
    if (parse_color(e, &line->color) != 0) {
      snprintf(err, errsize,
               "%s.color must be 0-255 or a color name (e.g. \"red\")", name);
      return -1;
    }
  } else if (strncmp(field, "colors.", 7) == 0) {
    const char *key = field + 7;
    color_rule *rule = NULL;
    // Later definitions of the same key replace earlier ones
    for (int i = 0; i < line->ncolors; i++) {
      if (strcmp(line->colors[i].key, key) == 0) {
        rule = &line->colors[i];
      }
    }
    if (!rule) {
      if (line->ncolors == COLOR_MAX) {
        snprintf(err, errsize, "%s.colors: too many entries (max %d)", name,
                 COLOR_MAX);
        return -1;
      }
      if (key[0] == '\0' || strlen(key) >= COLOR_KEY_MAX) {
        snprintf(err, errsize, "%s.colors: invalid key \"%s\"", name, key);
        return -1;
      }
      rule = &line->colors[line->ncolors++];
      snprintf(rule->key, sizeof(rule->key), "%s", key);
    }
    if (parse_color(e, &rule->color) != 0) {
      snprintf(err, errsize, "%s.colors.\"%s\" must be 0-255 or a color name",
               name, key);
      return -1;
    }
  } else {
    snprintf(err, errsize, "unknown option %s.%s", name, field);
    return -1;
  }
  return 0;
}

static const char blocks_error[] =
    "general.blocks must be \"auto\", \"unicode\", \"reverse\" or a single "
    "ASCII character";

static int load_general(config *cfg, const toml_entry *e, const char *field,
                        char *err, size_t errsize) {
  if (strcmp(field, "interval") == 0) {
    if (e->type != TOML_INT || e->ival < 10 || e->ival > 3600000) {
      snprintf(err, errsize,
               "general.interval must be milliseconds from 10 to 3600000");
      return -1;
    }
    cfg->interval_ms = (int)e->ival;
  } else if (strcmp(field, "spacing") == 0) {
    if (e->type != TOML_INT || e->ival < 0 || e->ival > 100) {
      snprintf(err, errsize, "general.spacing must be an integer from 0 to 100");
      return -1;
    }
    cfg->spacing = (int)e->ival;
  } else if (strcmp(field, "fit") == 0) {
    if (e->type != TOML_BOOL) {
      snprintf(err, errsize, "general.fit must be true or false");
      return -1;
    }
    cfg->fit = (int)e->ival;
  } else if (strcmp(field, "blocks") == 0) {
    if (e->type != TOML_STRING) {
      snprintf(err, errsize, "%s", blocks_error);
      return -1;
    }
    if (strcmp(e->str, "auto") == 0) {
      cfg->blocks = BLOCKS_AUTO;
    } else if (strcmp(e->str, "unicode") == 0) {
      cfg->blocks = BLOCKS_UNICODE;
    } else if (strcmp(e->str, "reverse") == 0) {
      cfg->blocks = BLOCKS_REVERSE;
    } else if (strlen(e->str) == 1 && e->str[0] > ' ' && e->str[0] < 0x7f) {
      cfg->blocks = BLOCKS_ASCII;
      cfg->block_char = e->str[0];
    } else {
      snprintf(err, errsize, "%s", blocks_error);
      return -1;
    }
  } else if (strcmp(field, "colors") == 0) {
    long long n = -1;
    if (e->type == TOML_INT) {
      n = e->ival;
    } else if (e->type == TOML_STRING && strcmp(e->str, "auto") == 0) {
      n = COLORS_AUTO;
    } else if (e->type == TOML_STRING) {
      char *end;
      n = strtoll(e->str, &end, 10);
      if (end == e->str || *end != '\0') {
        n = -1;
      }
    }
    if (n != COLORS_AUTO && n != 8 && n != 16 && n != 256) {
      snprintf(err, errsize,
               "general.colors must be \"auto\", 8, 16 or 256");
      return -1;
    }
    cfg->colors = (int)n;
  } else if (strcmp(field, "locale") == 0) {
    if (e->type != TOML_STRING || strlen(e->str) >= sizeof(cfg->locale)) {
      snprintf(err, errsize, "general.locale must be a locale name");
      return -1;
    }
    memcpy(cfg->locale, e->str, strlen(e->str) + 1);
  } else {
    snprintf(err, errsize, "unknown option general.%s", field);
    return -1;
  }
  return 0;
}

int config_load(config *cfg, const char *path, char *err, size_t errsize) {
  set_defaults(cfg);

  if (path) {
    snprintf(cfg->path, sizeof(cfg->path), "%s", path);
  } else if (config_find(cfg->path, sizeof(cfg->path)) != 0) {
    return 0; // No config file, built-in defaults
  }

  toml_doc doc;
  if (toml_parse_file(cfg->path, &doc) != 0) {
    snprintf(err, errsize, "%s: %s", cfg->path, doc.error);
    toml_free(&doc);
    return -1;
  }

  char msg[256] = "";
  int rc = 0;
  for (int i = 0; i < doc.count && rc == 0; i++) {
    const toml_entry *e = &doc.entries[i];
    if (strncmp(e->key, "general.", 8) == 0) {
      rc = load_general(cfg, e, e->key + 8, msg, sizeof(msg));
    } else if (strncmp(e->key, "line1.", 6) == 0) {
      rc = load_line(&cfg->lines[0], "line1", e, e->key + 6, msg, sizeof(msg));
    } else if (strncmp(e->key, "line2.", 6) == 0) {
      rc = load_line(&cfg->lines[1], "line2", e, e->key + 6, msg, sizeof(msg));
    } else {
      snprintf(msg, sizeof(msg), "unknown option %.200s", e->key);
      rc = -1;
    }
  }
  toml_free(&doc);

  if (rc != 0) {
    snprintf(err, errsize, "%s: %s", cfg->path, msg);
    return -1;
  }
  return 0;
}
