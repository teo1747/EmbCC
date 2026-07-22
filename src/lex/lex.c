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
    { "char", TOK_KW_CHAR },
    { "short", TOK_KW_SHORT },
    { "long", TOK_KW_LONG },
    { "unsigned", TOK_KW_UNSIGNED },
    { "signed", TOK_KW_SIGNED },
    { "void", TOK_KW_VOID },
    { "sizeof", TOK_KW_SIZEOF },
    { "return", TOK_KW_RETURN },
    { "static", TOK_KW_STATIC },
    { "extern", TOK_KW_EXTERN },
    { "struct", TOK_KW_STRUCT },
    { "union", TOK_KW_UNION },
    { "enum", TOK_KW_ENUM },
    { "typedef", TOK_KW_TYPEDEF },
    { "const", TOK_KW_CONST },
    { "volatile", TOK_KW_VOLATILE },
    { "restrict", TOK_KW_RESTRICT },
    { "if", TOK_KW_IF },
    { "else", TOK_KW_ELSE },
    { "while", TOK_KW_WHILE },
    { "for", TOK_KW_FOR },
    { "break", TOK_KW_BREAK },
    { "continue", TOK_KW_CONTINUE },
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
        int hex = lx->p[0] == '0' &&
                  (lx->p[1] == 'x' || lx->p[1] == 'X');
        unsigned long v = strtoul(lx->p, &end, 0);
        int has_u = 0, has_l = 0;
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') {
            if (*end == 'u' || *end == 'U')
                has_u = 1;
            else
                has_l = 1;
            end++;
        }
        if (isalnum((unsigned char)*end) || *end == '_' || *end == '.')
            diag_fatal(lx->file, lx->line,
                       "malformed integer constant");
        t->kind = TOK_NUM;
        t->num = (long)v;
        /* C99 typing: decimal grows int -> long; hex additionally
         * passes through the unsigned types. Suffixes force it. */
        t->num_long = has_l || v > (unsigned long)INT_MAX;
        t->num_uns = has_u;
        if (hex && !has_u) {
            if (v > (unsigned long)INT_MAX && v <= 0xffffffffUL) {
                t->num_uns = 1;
                t->num_long = has_l;
            } else if (v > (unsigned long)LONG_MAX) {
                t->num_uns = 1;
            }
        }
        if (!hex && !has_u && !has_l && v > (unsigned long)LONG_MAX)
            diag_fatal(lx->file, lx->line,
                       "integer constant out of range for long");
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

    /* Operators that pair with '=' (or double themselves) share one
     * shape: base, base=, and for some basebase / basebase=. */
    switch (*lx->p) {
    case '(': t->kind = TOK_LPAREN; break;
    case ')': t->kind = TOK_RPAREN; break;
    case '{': t->kind = TOK_LBRACE; break;
    case '}': t->kind = TOK_RBRACE; break;
    case '[': t->kind = TOK_LBRACKET; break;
    case ']': t->kind = TOK_RBRACKET; break;
    case ',': t->kind = TOK_COMMA; break;
    case ';': t->kind = TOK_SEMI; break;
    case '~': t->kind = TOK_TILDE; break;
    case '+':
        if (lx->p[1] == '+') { t->kind = TOK_PLUSPLUS; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_PLUSEQ; lx->p++; }
        else t->kind = TOK_PLUS;
        break;
    case '-':
        if (lx->p[1] == '-') { t->kind = TOK_MINUSMINUS; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_MINUSEQ; lx->p++; }
        else if (lx->p[1] == '>') { t->kind = TOK_ARROW; lx->p++; }
        else t->kind = TOK_MINUS;
        break;
    case '*':
        if (lx->p[1] == '=') { t->kind = TOK_STAREQ; lx->p++; }
        else t->kind = TOK_STAR;
        break;
    case '/':
        if (lx->p[1] == '=') { t->kind = TOK_SLASHEQ; lx->p++; }
        else t->kind = TOK_SLASH;
        break;
    case '%':
        if (lx->p[1] == '=') { t->kind = TOK_PERCENTEQ; lx->p++; }
        else t->kind = TOK_PERCENT;
        break;
    case '^':
        if (lx->p[1] == '=') { t->kind = TOK_CARETEQ; lx->p++; }
        else t->kind = TOK_CARET;
        break;
    case '=':
        if (lx->p[1] == '=') { t->kind = TOK_EQEQ; lx->p++; }
        else t->kind = TOK_ASSIGN;
        break;
    case '!':
        if (lx->p[1] == '=') { t->kind = TOK_NEQ; lx->p++; }
        else t->kind = TOK_BANG;
        break;
    case '&':
        if (lx->p[1] == '&') { t->kind = TOK_ANDAND; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_AMPEQ; lx->p++; }
        else t->kind = TOK_AMP;
        break;
    case '|':
        if (lx->p[1] == '|') { t->kind = TOK_OROR; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_PIPEEQ; lx->p++; }
        else t->kind = TOK_PIPE;
        break;
    case '<':
        if (lx->p[1] == '<' && lx->p[2] == '=') {
            t->kind = TOK_SHLEQ;
            lx->p += 2;
        } else if (lx->p[1] == '<') {
            t->kind = TOK_SHL;
            lx->p++;
        } else if (lx->p[1] == '=') {
            t->kind = TOK_LE;
            lx->p++;
        } else {
            t->kind = TOK_LT;
        }
        break;
    case '>':
        if (lx->p[1] == '>' && lx->p[2] == '=') {
            t->kind = TOK_SHREQ;
            lx->p += 2;
        } else if (lx->p[1] == '>') {
            t->kind = TOK_SHR;
            lx->p++;
        } else if (lx->p[1] == '=') {
            t->kind = TOK_GE;
            lx->p++;
        } else {
            t->kind = TOK_GT;
        }
        break;
    case '#': {
        /* a line marker from the preprocessor: # LINE "FILE" */
        const char *p = lx->p + 1;
        while (*p == ' ')
            p++;
        if (!isdigit((unsigned char)*p))
            diag_fatal(lx->file, lx->line,
                       "stray '#' (preprocessor directives are handled "
                       "before the lexer)");
        char *end;
        long ln = strtol(p, &end, 10);
        p = end;
        while (*p == ' ')
            p++;
        if (*p != '"')
            diag_fatal(lx->file, lx->line, "malformed line marker");
        const char *fstart = ++p;
        while (*p && *p != '"')
            p++;
        lx->file = xstrndup(fstart, (size_t)(p - fstart));
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
        lx->p = p;
        lx->line = (int)ln;
        lex_next(lx); /* the marker produced no token; go again */
        return;
    }
    case '\'': {
        lx->p++;
        long v;
        if (*lx->p == '\\') {
            lx->p++;
            switch (*lx->p) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case '0': v = 0; break;
            case '\\': v = '\\'; break;
            case '\'': v = '\''; break;
            case '"': v = '"'; break;
            default:
                diag_fatal(lx->file, lx->line,
                           "unknown escape '\\%c' in character constant",
                           *lx->p);
                return;
            }
            lx->p++;
        } else if (*lx->p && *lx->p != '\'' && *lx->p != '\n') {
            v = (unsigned char)*lx->p;
            lx->p++;
        } else {
            diag_fatal(lx->file, lx->line, "empty character constant");
            return;
        }
        if (*lx->p != '\'')
            diag_fatal(lx->file, lx->line,
                       "unterminated character constant");
        t->kind = TOK_NUM; /* a char constant has type int in C */
        t->num = v;
        t->num_long = 0;
        t->num_uns = 0;
        break;
    }
    case '"': {
        lx->p++;
        /* Worst case the literal shrinks (escapes), never grows. */
        size_t cap = 0;
        const char *scan = lx->p;
        while (*scan && *scan != '"') {
            if (*scan == '\\' && scan[1])
                scan++;
            scan++;
            cap++;
        }
        char *bytes = xmalloc(cap + 1);
        size_t n = 0;
        while (*lx->p && *lx->p != '"' && *lx->p != '\n') {
            char ch;
            if (*lx->p == '\\') {
                lx->p++;
                switch (*lx->p) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = 0; break;
                case '\\': ch = '\\'; break;
                case '\'': ch = '\''; break;
                case '"': ch = '"'; break;
                default:
                    diag_fatal(lx->file, lx->line,
                               "unknown escape '\\%c' in string literal",
                               *lx->p);
                    return;
                }
                lx->p++;
            } else {
                ch = *lx->p++;
            }
            bytes[n++] = ch;
        }
        if (*lx->p != '"')
            diag_fatal(lx->file, lx->line, "unterminated string literal");
        bytes[n] = 0;
        t->kind = TOK_STR;
        t->text = bytes;
        t->num = (long)n + 1; /* the NUL is part of the object */
        break;
    }
    case '.':
        if (lx->p[1] == '.' && lx->p[2] == '.') {
            t->kind = TOK_ELLIPSIS;
            lx->p += 2;
        } else {
            t->kind = TOK_DOT;
        }
        break;
    default:
        diag_fatal(lx->file, lx->line,
                   "character '%c' is not supported yet", *lx->p);
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
    case TOK_STR: return "a string literal";
    case TOK_ELLIPSIS: return "'...'";
    case TOK_IDENT:
        snprintf(buf, sizeof buf, "'%s'", t->text);
        return buf;
    case TOK_KW_INT: return "'int'";
    case TOK_KW_CHAR: return "'char'";
    case TOK_KW_SHORT: return "'short'";
    case TOK_KW_LONG: return "'long'";
    case TOK_KW_UNSIGNED: return "'unsigned'";
    case TOK_KW_SIGNED: return "'signed'";
    case TOK_KW_SIZEOF: return "'sizeof'";
    case TOK_KW_VOID: return "'void'";
    case TOK_KW_RETURN: return "'return'";
    case TOK_KW_STATIC: return "'static'";
    case TOK_KW_EXTERN: return "'extern'";
    case TOK_KW_STRUCT: return "'struct'";
    case TOK_KW_UNION: return "'union'";
    case TOK_KW_ENUM: return "'enum'";
    case TOK_KW_TYPEDEF: return "'typedef'";
    case TOK_KW_CONST: return "'const'";
    case TOK_KW_VOLATILE: return "'volatile'";
    case TOK_KW_RESTRICT: return "'restrict'";
    case TOK_DOT: return "'.'";
    case TOK_ARROW: return "'->'";
    case TOK_KW_IF: return "'if'";
    case TOK_KW_ELSE: return "'else'";
    case TOK_KW_WHILE: return "'while'";
    case TOK_KW_FOR: return "'for'";
    case TOK_KW_BREAK: return "'break'";
    case TOK_KW_CONTINUE: return "'continue'";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_LBRACE: return "'{'";
    case TOK_RBRACE: return "'}'";
    case TOK_LBRACKET: return "'['";
    case TOK_RBRACKET: return "']'";
    case TOK_COMMA: return "','";
    case TOK_SEMI: return "';'";
    case TOK_PLUS: return "'+'";
    case TOK_MINUS: return "'-'";
    case TOK_STAR: return "'*'";
    case TOK_SLASH: return "'/'";
    case TOK_PERCENT: return "'%'";
    case TOK_AMP: return "'&'";
    case TOK_PIPE: return "'|'";
    case TOK_CARET: return "'^'";
    case TOK_TILDE: return "'~'";
    case TOK_SHL: return "'<<'";
    case TOK_SHR: return "'>>'";
    case TOK_ASSIGN: return "'='";
    case TOK_EQEQ: return "'=='";
    case TOK_NEQ: return "'!='";
    case TOK_LT: return "'<'";
    case TOK_GT: return "'>'";
    case TOK_LE: return "'<='";
    case TOK_GE: return "'>='";
    case TOK_ANDAND: return "'&&'";
    case TOK_OROR: return "'||'";
    case TOK_BANG: return "'!'";
    case TOK_PLUSEQ: return "'+='";
    case TOK_MINUSEQ: return "'-='";
    case TOK_STAREQ: return "'*='";
    case TOK_SLASHEQ: return "'/='";
    case TOK_PERCENTEQ: return "'%='";
    case TOK_AMPEQ: return "'&='";
    case TOK_PIPEEQ: return "'|='";
    case TOK_CARETEQ: return "'^='";
    case TOK_SHLEQ: return "'<<='";
    case TOK_SHREQ: return "'>>='";
    case TOK_PLUSPLUS: return "'++'";
    case TOK_MINUSMINUS: return "'--'";
    }
    return "?";
}
