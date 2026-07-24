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
    TOK_FNUM,
    TOK_STR,
    TOK_IDENT,
    TOK_ELLIPSIS,
    TOK_KW_INT,
    TOK_KW_CHAR,
    TOK_KW_SHORT,
    TOK_KW_LONG,
    TOK_KW_FLOAT,
    TOK_KW_DOUBLE,
    TOK_KW_UNSIGNED,
    TOK_KW_SIGNED,
    TOK_KW_VOID,
    TOK_KW_SIZEOF,
    TOK_KW_RETURN,
    TOK_KW_STATIC,
    TOK_KW_EXTERN,
    TOK_KW_STRUCT,
    TOK_KW_UNION,
    TOK_KW_ENUM,
    TOK_KW_TYPEDEF,
    TOK_KW_CONST,
    TOK_KW_VOLATILE,
    TOK_KW_RESTRICT,
    TOK_KW_ASM,
    TOK_KW_INLINE,
    TOK_DOT,
    TOK_ARROW,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_DO,
    TOK_KW_SWITCH,
    TOK_KW_CASE,
    TOK_KW_DEFAULT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_SEMI,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_SHL,
    TOK_SHR,
    TOK_ASSIGN,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_ANDAND,
    TOK_OROR,
    TOK_BANG,
    /* compound assignment; parse desugars 'a op= b' to 'a = a op b' */
    TOK_PLUSEQ,
    TOK_MINUSEQ,
    TOK_STAREQ,
    TOK_SLASHEQ,
    TOK_PERCENTEQ,
    TOK_AMPEQ,
    TOK_PIPEEQ,
    TOK_CARETEQ,
    TOK_SHLEQ,
    TOK_SHREQ,
    TOK_PLUSPLUS,
    TOK_MINUSMINUS,
    TOK_QUESTION,
    TOK_COLON
};

struct token {
    enum tok_kind kind;
    int line;
    long num;      /* TOK_NUM; TOK_STR: byte length INCLUDING the NUL */
    int num_long;  /* TOK_NUM: type is long (L suffix or magnitude) */
    int num_uns;   /* TOK_NUM: type is unsigned (U suffix or hex range) */
    double fnum;   /* TOK_FNUM */
    int fnum_is_float; /* TOK_FNUM: an 'f' suffix -> float, else double */
    char *text;    /* TOK_IDENT; TOK_STR: the bytes (may contain NULs) */
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
