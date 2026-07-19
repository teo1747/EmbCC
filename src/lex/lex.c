#include "lex.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

void lex_init(struct lexer *lx, const char *file, const char *src)
{
    lx->file = file;
    lx->src = src;
    lx->p = src;
    lx->line = 1;
    lex_next(lx);
}

static void skip_space_and_comments(struct lexer *lx)
{
    for (;;) {
        while (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
               *lx->p == '\n') {
            if (*lx->p == '\n')
                lx->line++;
            lx->p++;
        }
        if (lx->p[0] == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n')
                lx->p++;
            continue;
        }
        if (lx->p[0] == '/' && lx->p[1] == '*') {
            int start = lx->line;
            lx->p += 2;
            while (!(lx->p[0] == '*' && lx->p[1] == '/')) {
                if (!*lx->p)
                    diag_fatal(lx->file, start, "unterminated comment");
                if (*lx->p == '\n')
                    lx->line++;
                lx->p++;
            }
            lx->p += 2;
            continue;
        }
        return;
    }
}

static const struct {
    const char *word;
    enum tok_kind kind;
} keywords[] = {
    { "int", TOK_KW_INT },
    { "void", TOK_KW_VOID },
    { "return", TOK_KW_RETURN },
    { "static", TOK_KW_STATIC },
};

void lex_next(struct lexer *lx)
{
    struct token *t = &lx->tok;

    skip_space_and_comments(lx);
    t->line = lx->line;
    t->text = NULL;
    t->num = 0;

    if (!*lx->p) {
        t->kind = TOK_EOF;
        return;
    }

    if (isdigit((unsigned char)*lx->p)) {
        char *end;
        long v = strtol(lx->p, &end, 0);
        if (isalpha((unsigned char)*end) || *end == '_' || *end == '.')
            diag_fatal(lx->file, lx->line,
                       "only plain integer constants are supported "
                       "(M1 subset)");
        if (v < INT_MIN || v > INT_MAX)
            diag_fatal(lx->file, lx->line,
                       "integer constant out of range for int");
        t->kind = TOK_NUM;
        t->num = v;
        lx->p = end;
        return;
    }

    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') {
        const char *start = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '_')
            lx->p++;
        size_t n = (size_t)(lx->p - start);
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
            if (strlen(keywords[i].word) == n &&
                memcmp(keywords[i].word, start, n) == 0) {
                t->kind = keywords[i].kind;
                return;
            }
        }
        t->kind = TOK_IDENT;
        t->text = xstrndup(start, n);
        return;
    }

    switch (*lx->p) {
    case '(': t->kind = TOK_LPAREN; break;
    case ')': t->kind = TOK_RPAREN; break;
    case '{': t->kind = TOK_LBRACE; break;
    case '}': t->kind = TOK_RBRACE; break;
    case ',': t->kind = TOK_COMMA; break;
    case ';': t->kind = TOK_SEMI; break;
    case '+': t->kind = TOK_PLUS; break;
    case '-': t->kind = TOK_MINUS; break;
    case '*': t->kind = TOK_STAR; break;
    case '=':
        if (lx->p[1] == '=')
            diag_fatal(lx->file, lx->line,
                       "'==' is not supported (M1 subset has no comparisons)");
        t->kind = TOK_ASSIGN;
        break;
    case '#':
        diag_fatal(lx->file, lx->line,
                   "preprocessor directives are not supported yet "
                   "(the preprocessor is M2 — see docs/ROADMAP.md)");
        break;
    case '"':
    case '\'':
        diag_fatal(lx->file, lx->line,
                   "string and character literals are not supported "
                   "(M1 subset)");
        break;
    default:
        diag_fatal(lx->file, lx->line,
                   "character '%c' is not part of the M1 subset", *lx->p);
    }
    lx->p++;
}

const char *tok_describe(const struct token *t)
{
    static char buf[64];
    switch (t->kind) {
    case TOK_EOF: return "end of file";
    case TOK_NUM:
        snprintf(buf, sizeof buf, "number %ld", t->num);
        return buf;
    case TOK_IDENT:
        snprintf(buf, sizeof buf, "'%s'", t->text);
        return buf;
    case TOK_KW_INT: return "'int'";
    case TOK_KW_VOID: return "'void'";
    case TOK_KW_RETURN: return "'return'";
    case TOK_KW_STATIC: return "'static'";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_LBRACE: return "'{'";
    case TOK_RBRACE: return "'}'";
    case TOK_COMMA: return "','";
    case TOK_SEMI: return "';'";
    case TOK_PLUS: return "'+'";
    case TOK_MINUS: return "'-'";
    case TOK_STAR: return "'*'";
    case TOK_ASSIGN: return "'='";
    }
    return "?";
}
