#include "lex.h"
#include <ctype.h>
#include <string.h>

static bool is_id_start(int c) { return isalpha(c) || c == '_'; }
static bool is_id_char(int c) { return isalnum(c) || c == '_'; }

static Status push(Arena *a, Tokens *t, Tok k, const char *s, size_t n, const char *file, long line, Err *e) {
  Token *x = ARR_SLOT(a, *t, e);
  if (x == NULL) return STATUS_ERR;
  x->kind = k;
  x->s = s;
  x->n = n;
  x->mag = 0;
  x->file = file;
  x->line = line;
  return STATUS_OK;
}

/* a `#` line must be a line marker: `#line N "file"` or `# N "file"`; the next line has number N */
static Status marker(Arena *a, const char *p, const char *end, const char **file, long *line, Err *e) {
  const char *q = p + 1;
  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (end - q >= 4 && memcmp(q, "line", 4) == 0) {
    q += 4;
    if (q >= end || (*q != ' ' && *q != '\t')) return err_set(e, CODE_PARSE, *file, *line, "a # line that is not a line marker");
  }
  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (q >= end || !isdigit((unsigned char)*q)) return err_set(e, CODE_PARSE, *file, *line, "a # line that is not a line marker");
  long n = 0;
  while (q < end && isdigit((unsigned char)*q)) {
    n = n * 10 + (*q - '0');
    q++;
  }
  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (q < end && *q == '"') {
    const char *f = ++q;
    while (q < end && *q != '"') q++;
    if (q >= end) return err_set(e, CODE_PARSE, *file, *line, "unterminated file name in a line marker");
    if (strlen(*file) != (size_t)(q - f) || memcmp(*file, f, (size_t)(q - f)) != 0) {
      char *copy = arena_strndup(a, f, (size_t)(q - f));
      if (copy == NULL) return err_oom(e);
      *file = copy;
    }
  }
  *line = n;
  return STATUS_OK;
}

Status lex_program(Arena *a, const char *text, size_t len, const char *file0, Tokens *out, Err *e) {
  const char *p = text, *end = text + len;
  const char *file = file0;
  long line = 1;
  bool bol = true; /* only whitespace since the last newline */
  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
      line++;
      p++;
      bol = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      p++;
      continue;
    }
    if (c == '#' && bol) {
      if (marker(a, p, end, &file, &line, e) != STATUS_OK) return STATUS_ERR;
      while (p < end && *p != '\n') p++;
      if (p < end) p++; /* the newline ends the marker line; the next line is `line` */
      bol = true;
      continue;
    }
    bol = false;
    Status st = STATUS_OK;
    if (is_id_start(c)) {
      const char *s = p;
      while (p < end && is_id_char((unsigned char)*p)) p++;
      st = push(a, out, T_IDENT, s, (size_t)(p - s), file, line, e);
    } else if (isdigit(c)) {
      const char *s = p;
      U128 mag = 0;
      while (p < end && isdigit((unsigned char)*p)) {
        unsigned d = (unsigned)(*p - '0');
        if (mag > ((((U128)1 << 127)) - d) / 10) return err_set(e, CODE_PARSE, file, line, "number literal out of range");
        mag = mag * 10 + d;
        p++;
      }
      if (p < end && is_id_char((unsigned char)*p))
        return err_set(e, CODE_PARSE, file, line, "bad number literal (hex, binary and float are not supported)");
      if (p < end && *p == '.' && p + 1 < end && isdigit((unsigned char)p[1]))
        return err_set(e, CODE_PARSE, file, line, "float literals are not supported");
      st = push(a, out, T_NUMBER, s, (size_t)(p - s), file, line, e);
      if (st == STATUS_OK) out->p[out->n - 1].mag = mag;
    } else if (c == '"') {
      const char *s = ++p;
      while (p < end && *p != '"') {
        if (*p == '\\') return err_set(e, CODE_PARSE, file, line, "escape in a string literal");
        if (*p == '\n') return err_set(e, CODE_PARSE, file, line, "unterminated string literal");
        p++;
      }
      if (p >= end) return err_set(e, CODE_PARSE, file, line, "unterminated string literal");
      st = push(a, out, T_STRING, s, (size_t)(p - s), file, line, e);
      p++;
    } else if (c == '.' && p + 1 < end && isalpha((unsigned char)p[1])) {
      const char *s = ++p;
      while (p < end && is_id_char((unsigned char)*p)) p++;
      size_t n = (size_t)(p - s);
      Tok k;
      if (n == 4 && memcmp(s, "decl", 4) == 0) k = T_DECL;
      else if (n == 4 && memcmp(s, "type", 4) == 0) k = T_TYPE;
      else if (n == 5 && memcmp(s, "input", 5) == 0) k = T_INPUT;
      else if (n == 6 && memcmp(s, "output", 6) == 0) k = T_OUTPUT;
      else return err_set(e, CODE_PARSE, file, line, "directive .%.*s is not supported", (int)n, s);
      st = push(a, out, k, s, n, file, line, e);
    } else {
      Tok k;
      size_t n = 1;
      switch (c) {
      case '(': k = T_LPAREN; break;
      case ')': k = T_RPAREN; break;
      case '{': k = T_LBRACE; break;
      case '}': k = T_RBRACE; break;
      case ',': k = T_COMMA; break;
      case '.': k = T_DOT; break;
      case ';': k = T_SEMI; break;
      case '!':
        if (p + 1 < end && p[1] == '=') { k = T_NE; n = 2; } else k = T_BANG;
        break;
      case '=': k = T_EQ; break;
      case '<':
        if (p + 1 < end && p[1] == '=') { k = T_LE; n = 2; }
        else if (p + 1 < end && p[1] == ':') { k = T_SUBTYPE; n = 2; }
        else k = T_LT;
        break;
      case '>':
        if (p + 1 < end && p[1] == '=') { k = T_GE; n = 2; } else k = T_GT;
        break;
      case ':':
        if (p + 1 < end && p[1] == '-') { k = T_IMPLIES; n = 2; } else k = T_COLON;
        break;
      case '+': k = T_PLUS; break;
      case '-': k = T_MINUS; break;
      case '*': k = T_STAR; break;
      case '/': k = T_SLASH; break;
      case '%': k = T_PERCENT; break;
      case '|': k = T_PIPE; break;
      default: return err_set(e, CODE_PARSE, file, line, "unexpected character 0x%02x", c);
      }
      st = push(a, out, k, p, n, file, line, e);
      p += n;
    }
    if (st != STATUS_OK) return STATUS_ERR;
  }
  return push(a, out, T_EOF, p, 0, file, line, e);
}
