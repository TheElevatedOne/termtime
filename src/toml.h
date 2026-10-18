#ifndef TERMTIME_TOML_H
#define TERMTIME_TOML_H

/**
 * Minimal TOML reader.
 *
 * Supports tables ([a.b]), dotted/quoted keys, inline tables,
 * basic and literal strings, integers, floats, booleans and arrays
 * (arrays are parsed but their values are not stored).
 * Every value is stored flat under its full dotted key path,
 * e.g. [line1.colors] "%H" = 9 -> key "line1.colors.%H".
 */

#define TOML_KEY_MAX 256
#define TOML_STR_MAX 1024

typedef enum {
  TOML_STRING,
  TOML_INT,
  TOML_FLOAT,
  TOML_BOOL,
  TOML_ARRAY,
} toml_type;

typedef struct {
  char key[TOML_KEY_MAX];
  toml_type type;
  char str[TOML_STR_MAX];
  long long ival;
  double fval;
} toml_entry;

typedef struct {
  toml_entry *entries;
  int count;
  int cap;
  char error[256];
} toml_doc;

/**
 * Parses a TOML file into doc.
 * Returns 0 on success, -1 on failure (doc->error holds the message).
 */
int toml_parse_file(const char *path, toml_doc *doc);

/**
 * Parses a NUL-terminated TOML string into doc.
 */
int toml_parse_string(const char *src, toml_doc *doc);

void toml_free(toml_doc *doc);

/**
 * Returns the entry for the full dotted key, or NULL.
 */
const toml_entry *toml_get(const toml_doc *doc, const char *key);

#endif // !TERMTIME_TOML_H
