#include "toml.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *p;
  int line;
  toml_doc *doc;
} parser;

static int fail(parser *ps, const char *fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(ps->doc->error, sizeof(ps->doc->error), "line %d: %s", ps->line,
           msg);
  return -1;
}

// Skips spaces and tabs (not newlines)
static void skip_ws(parser *ps) {
  while (*ps->p == ' ' || *ps->p == '\t') {
    ps->p++;
  }
}

static void skip_comment(parser *ps) {
  if (*ps->p == '#') {
    while (*ps->p && *ps->p != '\n') {
      ps->p++;
    }
  }
}

// Skips whitespace, newlines and comments
static void skip_all(parser *ps) {
  for (;;) {
    skip_ws(ps);
    skip_comment(ps);
    if (*ps->p == '\r') {
      ps->p++;
    } else if (*ps->p == '\n') {
      ps->p++;
      ps->line++;
    } else {
      return;
    }
  }
}

// Expects only whitespace and an optional comment until end of line
static int expect_eol(parser *ps) {
  skip_ws(ps);
  skip_comment(ps);
  if (*ps->p == '\r') {
    ps->p++;
  }
  if (*ps->p == '\n') {
    ps->p++;
    ps->line++;
    return 0;
  }
  if (*ps->p == '\0') {
    return 0;
  }
  return fail(ps, "unexpected '%c' after value", *ps->p);
}

static int append_utf8(char *out, size_t *len, size_t max, unsigned long cp) {
  char buf[4];
  size_t n;
  if (cp < 0x80) {
    buf[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  if (*len + n >= max) {
    return -1;
  }
  memcpy(out + *len, buf, n);
  *len += n;
  return 0;
}

/**
 * Parses a basic ("...") or literal ('...') string.
 * ps->p must point at the opening quote.
 */
static int parse_string(parser *ps, char *out, size_t max) {
  char quote = *ps->p++;
  size_t len = 0;

  while (*ps->p != quote) {
    char c = *ps->p;
    if (c == '\0' || c == '\n') {
      return fail(ps, "unterminated string");
    }
    if (quote == '"' && c == '\\') {
      ps->p++;
      unsigned long cp;
      switch (*ps->p) {
      case 'n':
        c = '\n';
        break;
      case 't':
        c = '\t';
        break;
      case 'r':
        c = '\r';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'e':
        c = '\033';
        break;
      case '"':
      case '\\':
        c = *ps->p;
        break;
      case 'u':
      case 'U': {
        int digits = (*ps->p == 'u') ? 4 : 8;
        char hex[9] = {0};
        for (int i = 0; i < digits; i++) {
          if (!isxdigit((unsigned char)ps->p[1 + i])) {
            return fail(ps, "invalid unicode escape");
          }
          hex[i] = ps->p[1 + i];
        }
        cp = strtoul(hex, NULL, 16);
        if (append_utf8(out, &len, max, cp) != 0) {
          return fail(ps, "string too long");
        }
        ps->p += digits + 1;
        continue;
      }
      default:
        return fail(ps, "invalid escape '\\%c'", *ps->p);
      }
    }
    if (len + 1 >= max) {
      return fail(ps, "string too long");
    }
    out[len++] = c;
    ps->p++;
  }
  ps->p++;
  out[len] = '\0';
  return 0;
}

static int is_bare_key_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/**
 * Parses a (possibly dotted) key and appends it to path.
 * Stops before '=' or ']'.
 */
static int parse_key(parser *ps, char *path, size_t max) {
  for (;;) {
    char part[TOML_KEY_MAX];
    skip_ws(ps);
    if (*ps->p == '"' || *ps->p == '\'') {
      if (parse_string(ps, part, sizeof(part)) != 0) {
        return -1;
      }
    } else {
      size_t n = 0;
      while (is_bare_key_char(*ps->p)) {
        if (n + 1 >= sizeof(part)) {
          return fail(ps, "key too long");
        }
        part[n++] = *ps->p++;
      }
      if (n == 0) {
        return fail(ps, "expected a key");
      }
      part[n] = '\0';
    }

    if (path[0] != '\0') {
      if (strlen(path) + 1 >= max) {
        return fail(ps, "key too long");
      }
      strcat(path, ".");
    }
    if (strlen(path) + strlen(part) >= max) {
      return fail(ps, "key too long");
    }
    strcat(path, part);

    skip_ws(ps);
    if (*ps->p != '.') {
      return 0;
    }
    ps->p++;
  }
}

static toml_entry *add_entry(parser *ps, const char *key) {
  toml_doc *doc = ps->doc;
  if (doc->count == doc->cap) {
    int cap = doc->cap ? doc->cap * 2 : 32;
    toml_entry *e = realloc(doc->entries, (size_t)cap * sizeof(*e));
    if (!e) {
      fail(ps, "out of memory");
      return NULL;
    }
    doc->entries = e;
    doc->cap = cap;
  }
  toml_entry *e = &doc->entries[doc->count++];
  memset(e, 0, sizeof(*e));
  snprintf(e->key, sizeof(e->key), "%s", key);
  return e;
}

static int parse_value(parser *ps, const char *key);

static int parse_inline_table(parser *ps, const char *prefix) {
  ps->p++; // {
  skip_ws(ps);
  if (*ps->p == '}') {
    ps->p++;
    return 0;
  }
  for (;;) {
    char key[TOML_KEY_MAX];
    snprintf(key, sizeof(key), "%s", prefix);
    if (parse_key(ps, key, sizeof(key)) != 0) {
      return -1;
    }
    if (*ps->p != '=') {
      return fail(ps, "expected '=' in inline table");
    }
    ps->p++;
    skip_ws(ps);
    if (parse_value(ps, key) != 0) {
      return -1;
    }
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == '}') {
      ps->p++;
      return 0;
    }
    return fail(ps, "expected ',' or '}' in inline table");
  }
}

static int parse_array(parser *ps, const char *key) {
  ps->p++; // [
  int count = 0;
  for (;;) {
    skip_all(ps);
    if (*ps->p == ']') {
      ps->p++;
      break;
    }
    char elem[TOML_KEY_MAX];
    snprintf(elem, sizeof(elem), "%s.%d", key, count++);
    if (parse_value(ps, elem) != 0) {
      return -1;
    }
    skip_all(ps);
    if (*ps->p == ',') {
      ps->p++;
    } else if (*ps->p != ']') {
      return fail(ps, "expected ',' or ']' in array");
    }
  }
  toml_entry *e = add_entry(ps, key);
  if (!e) {
    return -1;
  }
  e->type = TOML_ARRAY;
  e->ival = count;
  return 0;
}

static int parse_value(parser *ps, const char *key) {
  char c = *ps->p;

  if (c == '{') {
    return parse_inline_table(ps, key);
  }
  if (c == '[') {
    return parse_array(ps, key);
  }

  toml_entry *e = add_entry(ps, key);
  if (!e) {
    return -1;
  }

  if (c == '"' || c == '\'') {
    e->type = TOML_STRING;
    return parse_string(ps, e->str, sizeof(e->str));
  }
  if (strncmp(ps->p, "true", 4) == 0 && !is_bare_key_char(ps->p[4])) {
    e->type = TOML_BOOL;
    e->ival = 1;
    ps->p += 4;
    return 0;
  }
  if (strncmp(ps->p, "false", 5) == 0 && !is_bare_key_char(ps->p[5])) {
    e->type = TOML_BOOL;
    e->ival = 0;
    ps->p += 5;
    return 0;
  }

  // Number: copy without '_' separators
  char num[64];
  size_t n = 0;
  while (*ps->p && (isalnum((unsigned char)*ps->p) || *ps->p == '_' ||
                    *ps->p == '+' || *ps->p == '-' || *ps->p == '.')) {
    if (*ps->p != '_') {
      if (n + 1 >= sizeof(num)) {
        return fail(ps, "number too long");
      }
      num[n++] = *ps->p;
    }
    ps->p++;
  }
  num[n] = '\0';
  if (n == 0) {
    return fail(ps, "expected a value for '%s'", key);
  }

  char *end;
  errno = 0;
  int base = 10;
  const char *digits = num;
  int neg = 0;
  if (*digits == '+' || *digits == '-') {
    neg = (*digits == '-');
    digits++;
  }
  if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' ||
                           digits[1] == 'b')) {
    base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
    digits += 2;
  }
  long long v = strtoll(digits, &end, base);
  if (*end == '\0' && errno == 0 && end != digits) {
    e->type = TOML_INT;
    e->ival = neg ? -v : v;
    e->fval = (double)e->ival;
    return 0;
  }
  if (base == 10) {
    double d = strtod(num, &end);
    if (*end == '\0') {
      e->type = TOML_FLOAT;
      e->fval = d;
      e->ival = (long long)d;
      return 0;
    }
  }
  return fail(ps, "invalid value '%s' for '%s'", num, key);
}

int toml_parse_string(const char *src, toml_doc *doc) {
  parser ps = {src, 1, doc};
  char table[TOML_KEY_MAX] = "";

  memset(doc, 0, sizeof(*doc));

  for (;;) {
    skip_all(&ps);
    if (*ps.p == '\0') {
      return 0;
    }

    if (*ps.p == '[') {
      int array_table = (ps.p[1] == '[');
      ps.p += array_table ? 2 : 1;
      table[0] = '\0';
      if (parse_key(&ps, table, sizeof(table)) != 0) {
        return -1;
      }
      if (*ps.p != ']' || (array_table && ps.p[1] != ']')) {
        return fail(&ps, "expected ']' after table name");
      }
      ps.p += array_table ? 2 : 1;
      if (expect_eol(&ps) != 0) {
        return -1;
      }
      continue;
    }

    char key[TOML_KEY_MAX];
    snprintf(key, sizeof(key), "%s", table);
    if (parse_key(&ps, key, sizeof(key)) != 0) {
      return -1;
    }
    if (*ps.p != '=') {
      return fail(&ps, "expected '=' after key '%s'", key);
    }
    ps.p++;
    skip_ws(&ps);
    if (parse_value(&ps, key) != 0) {
      return -1;
    }
    if (expect_eol(&ps) != 0) {
      return -1;
    }
  }
}

int toml_parse_file(const char *path, toml_doc *doc) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    memset(doc, 0, sizeof(*doc));
    snprintf(doc->error, sizeof(doc->error), "%s", strerror(errno));
    return -1;
  }

  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += n;
    if (cap - len - 1 == 0) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        fclose(f);
        return -1;
      }
      buf = nb;
    }
  }
  buf[len] = '\0';
  fclose(f);

  int rc = toml_parse_string(buf, doc);
  free(buf);
  return rc;
}

void toml_free(toml_doc *doc) {
  free(doc->entries);
  doc->entries = NULL;
  doc->count = doc->cap = 0;
}

const toml_entry *toml_get(const toml_doc *doc, const char *key) {
  // Last definition wins
  for (int i = doc->count - 1; i >= 0; i--) {
    if (strcmp(doc->entries[i].key, key) == 0) {
      return &doc->entries[i];
    }
  }
  return NULL;
}
