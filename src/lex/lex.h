/* Tokens for the M1 subset (ARCHITECTURE §2).
 *
 * Anything C has that this enum lacks is rejected in lex.c with a
 * diagnostic naming the construct — THE RULE: unsupported must fail
 * loudly, never pass through as something else. Keywords outside the
 * subset (if, while, char, ...) deliberately lex as identifiers so the
 * parser can reject them with better context than the lexer has.
 */
#ifndef EMBCC_LEX_LEX_H
#define EMBCC_LEX_LEX_H

enum tok_kind {
    TOK_EOF,
    TOK_NUM,
    TOK_IDENT,
    TOK_KW_INT,
    TOK_KW_VOID,
    TOK_KW_RETURN,
    TOK_KW_STATIC,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_SEMI,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_ASSIGN
};

struct token {
    enum tok_kind kind;
    int line;
    long num;   /* TOK_NUM */
    char *text; /* TOK_IDENT */
};

struct lexer {
    const char *file;
    const char *src;
    const char *p;
    int line;
    struct token tok; /* current token */
};

void lex_init(struct lexer *lx, const char *file, const char *src);
void lex_next(struct lexer *lx);

/* Human-readable name of a token, for diagnostics. */
const char *tok_describe(const struct token *t);

#endif
