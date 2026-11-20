#ifndef TUPU_LEX_H
#define TUPU_LEX_H
#include "common.h"

typedef enum {
  T_EOF, T_IDENT, T_NUMBER, T_STRING, T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_COMMA, T_DOT,
  T_DECL, T_TYPE, T_INPUT, T_OUTPUT, T_IMPLIES, T_SEMI, T_COLON, T_BANG, T_EQ, T_NE, T_LT, T_LE,
  T_GT, T_GE, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_SUBTYPE, T_PIPE
} Tok;

typedef struct {
  Tok kind;
  const char *s; /* identifier text, string contents (no quotes) */
  size_t n;
  U128 mag; /* T_NUMBER: the magnitude, at most 2^127 */
  const char *file;
  long line;
} Token;

typedef struct {
  Token *p;
  size_t n, cap;
} Tokens;

Status lex_program(Arena *a, const char *text, size_t len, const char *file0, Tokens *out, Err *e);

#endif
