/* EmbAS — NASM/Intel-syntax assembler for the EmbLinkOS kernel corpus (A1).
 * See as.h. The kernel's hand-written `.asm` are NASM syntax, which EmbCC's
 * inline-asm encoder (AT&T, operand-resolved) cannot read; rather than port
 * nasm, EmbCC grows its own front-end so the toolchain owns the whole build.
 *
 * Flow: preprocess (expand %macro, strip comments, mangle `.local` labels) ->
 * parse each line into an ITEM in its section (fixed bytes, a relaxable local
 * jump, or a label def) -> relax jumps to a fixpoint so short ones use rel8 like
 * nasm -> place labels -> emit, patching in-file references and turning
 * external/absolute ones into relocations. ELF output uses the shared writer
 * (src/elf/write.c); `-f bin` drops symbols/relocs and resolves labels to
 * org-relative absolute addresses. Byte-identical to nasm on the kernel corpus.
 */
#include "as.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../driver/util.h"
#include "../elf/write.h"
#include "../elf/elf.h"

static char *xstrdup(const char *s){ return xstrndup(s, strlen(s)); }

/* c99-clean tokenizer (strtok_r is POSIX): return the next token up to `d`,
 * advancing *sp past the delimiter; NULL when exhausted. Modifies in place. */
static char *split(char **sp, char d){
    char *s=*sp; if(!s) return NULL;
    if(!*s){ *sp=NULL; return NULL; }
    char *start=s; while(*s && *s!=d) s++;
    if(*s){ *s=0; *sp=s+1; } else *sp=s;   /* *sp -> "" at end, then NULL next call */
    return start;
}

enum sec { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS, SEC_N };
static const char *sec_name[SEC_N] = { ".text", ".rodata", ".data", ".bss" };

struct sym { char *name; int sec; long off; int global, ext, defined; int seq; };

/* One assembled unit. A BYTES item is finished machine code or data; a JMP item
 * is a local jump whose rel8/rel32 width relaxation still chooses. A fixup patch
 * inside a BYTES item is resolved in-file (local target) or emitted as a
 * relocation (external / absolute). */
enum { IT_BYTES, IT_JMP };
struct item {
    int sec, kind;
    unsigned char b[32]; int n;          /* IT_BYTES */
    int fix_at; char fix_sym[128]; int fix_kind; long fix_add; int has_fix;
    /* IT_JMP */
    char jsym[128];
    unsigned op8, op32a, op32b;          /* rel8 opcode; rel32 opcode (1-2 bytes) */
    int wide;                            /* chosen: 0 = rel8, 1 = rel32 */
    long off;                            /* assigned within-section offset */
};
enum { FIX_REL32, FIX_ABS32, FIX_ABS64 };

struct as {
    struct item *it; int nit, capit;
    struct sym *sy; int nsy, capsy;
    long secsize[SEC_N];
    const char *file;
    int cur, bits, err;
    char last_global[128];               /* for `.local` label scoping */
    const char *incbin_dir;
    int secorder[SEC_N], nsecorder;      /* sections in first-appearance order (nasm's) */
    int seqctr;                          /* monotonic symbol-definition sequence (nasm's symtab order) */
};

/* Record a section the first time content lands in it — nasm orders the ELF
 * section header table (and its section symbols) by source appearance. */
static void sec_touch(struct as *a, int s) {
    for (int i = 0; i < a->nsecorder; i++) if (a->secorder[i]==s) return;
    a->secorder[a->nsecorder++] = s;
}

static void mangle(struct as *a, const char *in, char *out, int cap);

static void aerr(struct as *a, int ln, const char *m, const char *arg) {
    fprintf(stderr, "%s:%d: assembler error: %s%s%s\n", a->file, ln, m,
            arg ? " " : "", arg ? arg : "");
    a->err = 1;
}
static struct sym *sym_get(struct as *a, const char *nm) {
    for (int i = 0; i < a->nsy; i++)
        if (!strcmp(a->sy[i].name, nm)) return &a->sy[i];
    if (a->nsy == a->capsy) { a->capsy = a->capsy ? a->capsy*2 : 32;
        a->sy = xrealloc(a->sy, (size_t)a->capsy * sizeof *a->sy); }
    struct sym *s = &a->sy[a->nsy++]; memset(s, 0, sizeof *s);
    s->name = xstrdup(nm); s->sec = -1; s->seq = -1; return s;
}
static struct item *new_item(struct as *a) {
    if (a->nit == a->capit) { a->capit = a->capit ? a->capit*2 : 128;
        a->it = xrealloc(a->it, (size_t)a->capit * sizeof *a->it); }
    struct item *it = &a->it[a->nit++]; memset(it, 0, sizeof *it);
    it->sec = a->cur; it->kind = IT_BYTES; sec_touch(a, a->cur); return it;
}

/* ---- registers / operands ---- */
static int regnum(const char *s, int len, int *w) {
    static const struct { const char *n; int r, w; } R[] = {
        {"rax",0,8},{"rcx",1,8},{"rdx",2,8},{"rbx",3,8},{"rsp",4,8},{"rbp",5,8},
        {"rsi",6,8},{"rdi",7,8},{"r8",8,8},{"r9",9,8},{"r10",10,8},{"r11",11,8},
        {"r12",12,8},{"r13",13,8},{"r14",14,8},{"r15",15,8},
        {"eax",0,4},{"ecx",1,4},{"edx",2,4},{"ebx",3,4},{"esp",4,4},{"ebp",5,4},
        {"esi",6,4},{"edi",7,4},{"r8d",8,4},{"r9d",9,4},{"r10d",10,4},
        {"r11d",11,4},{"r12d",12,4},{"r13d",13,4},{"r14d",14,4},{"r15d",15,4},
    };
    for (unsigned i = 0; i < sizeof R/sizeof R[0]; i++)
        if ((int)strlen(R[i].n)==len && !strncmp(R[i].n,s,(size_t)len))
            { if (w) *w = R[i].w; return R[i].r; }
    return -1;
}
static long parse_int(const char *s) {
    while (*s==' ') s++;
    int neg = 0; if (*s=='-'){neg=1;s++;} else if (*s=='+') s++;
    long v = strtol(s, NULL, 0); return neg ? -v : v;
}

enum { OP_REG, OP_IMM, OP_MEM, OP_LABEL };
struct oper { int kind, reg, w, base, memlabel; long imm; char label[128]; };

static int parse_oper(struct as *a, int ln, char *t, struct oper *o) {
    memset(o, 0, sizeof *o);
    while (*t==' ') t++;
    /* skip a size override: `push qword 0`, `mov byte [x], 1`, etc. */
    static const char *SZ[] = {"byte","word","dword","qword","oword",
                               "tword","yword"};
    for (unsigned i=0;i<sizeof SZ/sizeof SZ[0];i++){
        size_t l=strlen(SZ[i]);
        if (!strncmp(t,SZ[i],l) && (t[l]==' '||t[l]=='\t')){ t+=l; while(*t==' '||*t=='\t') t++; break; }
    }
    if (t[0]=='[') {
        o->kind = OP_MEM;
        char in[128]; strncpy(in, t+1, sizeof in-1); in[sizeof in-1]=0;
        char *e = strchr(in, ']'); if (e) *e=0;
        char *p = in; while (*p==' ') p++;
        char reg[32]={0}; int i=0;
        while (*p && *p!='+' && *p!='-' && *p!=' ' && i<31) reg[i++]=*p++;
        int w; o->base = regnum(reg,(int)strlen(reg),&w);
        if (o->base<0) { aerr(a,ln,"bad memory base",reg); return 1; }
        while (*p==' ') p++;
        if (*p=='+'||*p=='-'){ int ng=*p=='-'; p++; o->imm=parse_int(p); if(ng)o->imm=-o->imm; }
        return 0;
    }
    int w, r = regnum(t,(int)strlen(t),&w);
    if (r>=0){ o->kind=OP_REG; o->reg=r; o->w=w; return 0; }
    if (t[0]=='-'||t[0]=='+'||isdigit((unsigned char)t[0])){ o->kind=OP_IMM; o->imm=parse_int(t); return 0; }
    o->kind=OP_LABEL; strncpy(o->label,t,sizeof o->label-1); return 0;
}

/* ---- encoding into an item's byte buffer ---- */
static void eb(struct item *it, unsigned v){ it->b[it->n++] = (unsigned char)v; }
static void rex(struct item *it,int w,int r,int x,int base){
    int v=0x40|(w?8:0)|((r&8)?4:0)|((x&8)?2:0)|((base&8)?1:0);
    if (v!=0x40) eb(it,v);
}
static void modrr(struct item *it,int reg,int rm){ eb(it,0xc0|((reg&7)<<3)|(rm&7)); }
static void modmem(struct item *it,int reg,int base,long d){
    int bb=base&7, sib=(bb==4), force=(bb==5);
    int mod=(d==0&&!force)?0:(d>=-128&&d<=127)?1:2;
    eb(it,(mod<<6)|((reg&7)<<3)|(sib?4:bb));
    if (sib) eb(it,(4<<3)|bb);
    if (mod==1) eb(it,(unsigned)(d&0xff));
    else if (mod==2) for(int i=0;i<4;i++) eb(it,(unsigned)((d>>(8*i))&0xff));
}
static void imm4(struct item *it,long v){ for(int i=0;i<4;i++) eb(it,(unsigned)((v>>(8*i))&0xff)); }
static void imm8(struct item *it,long v){ eb(it,(unsigned)(v&0xff)); }

/* Record an in-item fixup at the current write position (rel32/abs), then leave
 * `bytes` placeholder zero bytes for the field. */
static void fixup(struct item *it,const char *sym,int kind,long add,int bytes){
    it->has_fix=1; it->fix_at=it->n; strncpy(it->fix_sym,sym,sizeof it->fix_sym-1);
    it->fix_kind=kind; it->fix_add=add;
    for (int i=0;i<bytes;i++) eb(it,0);
}

/* Encode one instruction into a fresh item. ops[] are already parsed. */
static void encode(struct as *a,int ln,const char *m,struct oper *ops,int no){
    struct item *it = new_item(a);
    /* zero-operand */
    struct { const char *m; unsigned char b[3]; int n; } Z[] = {
        {"cli",{0xfa},1},{"hlt",{0xf4},1},{"ret",{0xc3},1},{"nop",{0x90},1},
        {"sti",{0xfb},1},{"leave",{0xc9},1},{"iretd",{0xcf},1},
        /* NASM in BITS 64: bare `iret` is IRETQ (48 cf); only `iretd` is cf */
        {"iret",{0x48,0xcf},2},{"iretq",{0x48,0xcf},2},
        {"pushfq",{0x9c},1},{"popfq",{0x9d},1},
        {"rdmsr",{0x0f,0x32},2},{"wrmsr",{0x0f,0x30},2},{"syscall",{0x0f,0x05},2},
        {"cpuid",{0x0f,0xa2},2},{"swapgs",{0x0f,0x01,0xf8},3},
    };
    for (unsigned i=0;i<sizeof Z/sizeof Z[0];i++)
        if (!strcmp(m,Z[i].m)) { for(int k=0;k<Z[i].n;k++) eb(it,Z[i].b[k]); return; }

    if (!strcmp(m,"push")) {
        if (no==1 && ops[0].kind==OP_REG){ rex(it,0,0,0,ops[0].reg); eb(it,0x50|(ops[0].reg&7)); }
        else if (no==1 && ops[0].kind==OP_IMM){
            if (ops[0].imm>=-128 && ops[0].imm<=127){ eb(it,0x6a); imm8(it,ops[0].imm); }
            else { eb(it,0x68); imm4(it,ops[0].imm); }
        } else aerr(a,ln,"bad push",NULL);
        return;
    }
    if (!strcmp(m,"pop") && no==1 && ops[0].kind==OP_REG){
        rex(it,0,0,0,ops[0].reg); eb(it,0x58|(ops[0].reg&7)); return;
    }
    if (!strcmp(m,"mov") && no==2) {
        struct oper *d=&ops[0], *s=&ops[1];
        if (d->kind==OP_REG && s->kind==OP_REG){
            rex(it,d->w==8,s->reg,0,d->reg); eb(it,0x89); modrr(it,s->reg,d->reg);
        } else if (d->kind==OP_REG && s->kind==OP_MEM){
            rex(it,d->w==8,d->reg,0,s->base); eb(it,0x8b); modmem(it,d->reg,s->base,s->imm);
        } else if (d->kind==OP_MEM && s->kind==OP_REG){
            rex(it,s->w==8,s->reg,0,d->base); eb(it,0x89); modmem(it,s->reg,d->base,d->imm);
        } else if (d->kind==OP_REG && s->kind==OP_IMM){
            long v=s->imm;
            if (d->w==4){                         /* mov r32, imm32 */
                rex(it,0,0,0,d->reg); eb(it,0xb8|(d->reg&7)); imm4(it,v);
            } else if (v>=0 && v<=0xffffffffL){    /* nasm: zero-extends via r32 */
                rex(it,0,0,0,d->reg); eb(it,0xb8|(d->reg&7)); imm4(it,v);
            } else if (v>=-0x80000000L && v<=0x7fffffffL){  /* sign-extended imm32 */
                rex(it,1,0,0,d->reg); eb(it,0xc7); modrr(it,0,d->reg); imm4(it,v);
            } else {                               /* movabs imm64 */
                rex(it,1,0,0,d->reg); eb(it,0xb8|(d->reg&7));
                for(int i=0;i<8;i++) eb(it,(unsigned)((v>>(8*i))&0xff));
            }
        } else if (d->kind==OP_REG && s->kind==OP_LABEL){
            /* mov reg, symbol -> movabs r64, imm64=&symbol + R_X86_64_64 reloc */
            char mg[128]; mangle(a,s->label,mg,sizeof mg);
            rex(it,1,0,0,d->reg); eb(it,0xb8|(d->reg&7));
            fixup(it,mg,FIX_ABS64,0,8);
        } else aerr(a,ln,"bad mov",NULL);
        return;
    }
    if (!strcmp(m,"lea") && no==2 && ops[0].kind==OP_REG && ops[1].kind==OP_MEM){
        rex(it,1,ops[0].reg,0,ops[1].base); eb(it,0x8d); modmem(it,ops[0].reg,ops[1].base,ops[1].imm); return;
    }
    /* ALU reg,reg / reg,imm */
    struct { const char *m; unsigned rr, ext, acc; } ALU[] = {
        {"add",0x01,0,0x05},{"or",0x09,1,0x0d},{"and",0x21,4,0x25},
        {"sub",0x29,5,0x2d},{"xor",0x31,6,0x35},{"cmp",0x39,7,0x3d},
    };
    for (unsigned i=0;i<sizeof ALU/sizeof ALU[0];i++) if(!strcmp(m,ALU[i].m)){
        struct oper *d=&ops[0], *s=&ops[1];
        if (d->kind==OP_REG && s->kind==OP_REG){
            rex(it,d->w==8,s->reg,0,d->reg); eb(it,ALU[i].rr); modrr(it,s->reg,d->reg);
        } else if (d->kind==OP_REG && s->kind==OP_IMM){
            if (s->imm>=-128 && s->imm<=127){ rex(it,d->w==8,0,0,d->reg); eb(it,0x83); modrr(it,ALU[i].ext,d->reg); imm8(it,s->imm); }
            else if (d->reg==0){ /* al/eax/rax accumulator form (nasm's choice) */
                rex(it,d->w==8,0,0,0); eb(it,ALU[i].acc); imm4(it,s->imm); }
            else { rex(it,d->w==8,0,0,d->reg); eb(it,0x81); modrr(it,ALU[i].ext,d->reg); imm4(it,s->imm); }
        } else aerr(a,ln,"bad ALU operand",m);
        return;
    }
    if (!strcmp(m,"test") && no==2 && ops[0].kind==OP_REG && ops[1].kind==OP_REG){
        rex(it,ops[0].w==8,ops[1].reg,0,ops[0].reg); eb(it,0x85); modrr(it,ops[1].reg,ops[0].reg); return;
    }
    if (!strcmp(m,"call")){
        if (no==1 && ops[0].kind==OP_REG){ rex(it,0,0,0,ops[0].reg); eb(it,0xff); modrr(it,2,ops[0].reg); }
        else if (no==1 && ops[0].kind==OP_LABEL){ eb(it,0xe8); fixup(it,ops[0].label,FIX_REL32,-4,4); }
        else aerr(a,ln,"bad call",NULL);
        return;
    }
    if (!strcmp(m,"jmp")){
        if (no==1 && ops[0].kind==OP_REG){ rex(it,0,0,0,ops[0].reg); eb(it,0xff); modrr(it,4,ops[0].reg); }
        else if (no==1 && ops[0].kind==OP_LABEL){
            it->kind=IT_JMP; strncpy(it->jsym,ops[0].label,sizeof it->jsym-1);
            it->op8=0xeb; it->op32a=0xe9; it->op32b=0;
        } else aerr(a,ln,"bad jmp",NULL);
        return;
    }
    /* conditional jumps (local, relaxable) */
    struct { const char *m; unsigned s, n; } JCC[] = {
        {"jo",0x70,0x80},{"jno",0x71,0x81},{"jb",0x72,0x82},{"jae",0x73,0x83},
        {"je",0x74,0x84},{"jz",0x74,0x84},{"jne",0x75,0x85},{"jnz",0x75,0x85},
        {"jbe",0x76,0x86},{"ja",0x77,0x87},{"js",0x78,0x88},{"jns",0x79,0x89},
        {"jl",0x7c,0x8c},{"jge",0x7d,0x8d},{"jle",0x7e,0x8e},{"jg",0x7f,0x8f},
    };
    for (unsigned i=0;i<sizeof JCC/sizeof JCC[0];i++) if(!strcmp(m,JCC[i].m)){
        if (no==1 && ops[0].kind==OP_LABEL){
            it->kind=IT_JMP; strncpy(it->jsym,ops[0].label,sizeof it->jsym-1);
            it->op8=JCC[i].s; it->op32a=0x0f; it->op32b=JCC[i].n;
        } else aerr(a,ln,"bad jcc",m);
        return;
    }
    if ((!strcmp(m,"fxsave")||!strcmp(m,"fxrstor")) && no==1 && ops[0].kind==OP_MEM){
        rex(it,0,0,0,ops[0].base); eb(it,0x0f); eb(it,0xae);
        modmem(it, !strcmp(m,"fxsave")?0:1, ops[0].base, ops[0].imm); return;
    }
    if ((!strcmp(m,"lgdt")||!strcmp(m,"lidt")) && no==1 && ops[0].kind==OP_MEM){
        eb(it,0x0f); eb(it,0x01); modmem(it, !strcmp(m,"lgdt")?2:3, ops[0].base, ops[0].imm); return;
    }
    aerr(a, ln, "unsupported instruction", m);
}

enum { IT_LABEL = 2 };   /* extends the {IT_BYTES, IT_JMP} kinds */

static struct sym *sym_find(struct as *a, const char *nm){
    for (int i=0;i<a->nsy;i++) if(!strcmp(a->sy[i].name,nm)) return &a->sy[i];
    return NULL;
}

/* Mangle a `.local` label against the last global (nasm's local-label scope). */
static void mangle(struct as *a, const char *in, char *out, int cap){ /* fwd-declared */
    if (in[0]=='.' && a->last_global[0]) snprintf(out,cap,"%s%s",a->last_global,in);
    else { strncpy(out,in,cap-1); out[cap-1]=0; }
}

/* A data directive: db/dw/dd/dq with constants or a label (label -> fixup). */
static void data_dir(struct as *a,int width,char *args){
    struct item *it=new_item(a);
    char *p=args, *tok;
    while ((tok=split(&p, ','))){
        while (*tok==' ') tok++;
        char *e=tok+strlen(tok); while(e>tok && (e[-1]==' '||e[-1]=='\t')) *--e=0;
        if (tok[0]=='"' || tok[0]=='\''){          /* string bytes (db) */
            char q=tok[0]; for(char*s=tok+1; *s && *s!=q; s++) eb(it,(unsigned char)*s);
        } else if (isdigit((unsigned char)tok[0]) || tok[0]=='-' || tok[0]=='+'){
            long v=parse_int(tok);
            for(int i=0;i<width;i++) eb(it,(unsigned)((v>>(8*i))&0xff));
        } else {                                    /* a label reference */
            char mg[128]; mangle(a,tok,mg,sizeof mg);
            fixup(it, mg, width==8?FIX_ABS64:FIX_ABS32, 0, width);
        }
        if (it->n > 24) { /* one item per element keeps items small */ }
    }
}

/* Handle one preprocessed source line (comments already stripped). */
static void process_line(struct as *a, int ln, char *line){
    while (*line==' '||*line=='\t') line++;
    if (!*line) return;
    /* [BITS 64] / [bits 64] */
    if (line[0]=='['){ if (strstr(line,"64")) a->bits=64; return; }

    /* leading label(s): NAME:  possibly followed by more on the line */
    for (;;){
        char *c=line; while (*c && (isalnum((unsigned char)*c)||*c=='_'||*c=='.'||*c=='$')) c++;
        char *sk=c; while (*sk==' '||*sk=='\t') sk++;
        if (c>line && *sk==':'){
            char name[128]; int len=(int)(c-line); if(len>127)len=127;
            memcpy(name,line,len); name[len]=0;
            char mg[128]; mangle(a,name,mg,sizeof mg);
            if (name[0]!='.') { strncpy(a->last_global,name,sizeof a->last_global-1); }
            struct item *it=new_item(a); it->kind=IT_LABEL;
            strncpy(it->jsym,mg,sizeof it->jsym-1);
            struct sym*s=sym_get(a,mg);            /* defined at placement */
            if (s->seq<0) s->seq=a->seqctr++;      /* symtab order = definition point */
            line=sk+1; while(*line==' '||*line=='\t') line++;
            if (!*line) return;
            continue;
        }
        break;
    }

    /* directive or instruction: split mnemonic + rest */
    char mn[32]; int i=0;
    while (line[i] && line[i]!=' ' && line[i]!='\t' && i<31){ mn[i]=(char)tolower((unsigned char)line[i]); i++; }
    mn[i]=0;
    char *rest=line+i; while(*rest==' '||*rest=='\t') rest++;

    if (!strcmp(mn,"global")||!strcmp(mn,"globl")){ struct sym*s=sym_get(a,rest); s->global=1; return; }
    if (!strcmp(mn,"extern")){ struct sym*s=sym_get(a,rest); s->ext=1; if(s->seq<0) s->seq=a->seqctr++; return; }
    if (!strcmp(mn,"section")||!strcmp(mn,"segment")){
        char nm[32]; sscanf(rest,"%31s",nm);
        for (int k=0;k<SEC_N;k++) if(!strcmp(nm,sec_name[k])){ a->cur=k; return; }
        aerr(a,ln,"unknown section",nm); return;
    }
    if (!strcmp(mn,"bits")||!strcmp(mn,"default")||!strcmp(mn,"cpu")) return;
    if (!strcmp(mn,"align")){ /* handled at placement via a padding item */
        struct item *it=new_item(a); it->kind=IT_BYTES; it->n=0;
        it->has_fix=0; it->fix_add=parse_int(rest); it->fix_kind=-2; /* -2 = align marker */
        strcpy(it->fix_sym,""); return;
    }
    if (!strcmp(mn,"db")){ data_dir(a,1,rest); return; }
    if (!strcmp(mn,"dw")){ data_dir(a,2,rest); return; }
    if (!strcmp(mn,"dd")){ data_dir(a,4,rest); return; }
    if (!strcmp(mn,"dq")){ data_dir(a,8,rest); return; }
    if (!strcmp(mn,"resb")||!strcmp(mn,"resw")||!strcmp(mn,"resd")||!strcmp(mn,"resq")){
        int unit = mn[3]=='b'?1:mn[3]=='w'?2:mn[3]=='d'?4:8;
        struct item *it=new_item(a); it->kind=IT_BYTES; it->fix_kind=-3; /* -3 = reserve */
        it->fix_add = parse_int(rest)*unit; return;   /* .bss reserve */
    }
    if (!strcmp(mn,"incbin")){
        /* incbin "path" — embed a raw file verbatim into the current section
         * (the AP trampoline blob). Chunked into item-sized runs. */
        char *q=strchr(rest,'"'); char *q2=q?strchr(q+1,'"'):NULL;
        if (!q||!q2){ aerr(a,ln,"incbin needs a quoted path",NULL); return; }
        char path[256]; int pl=(int)(q2-q-1); if(pl>255)pl=255;
        memcpy(path,q+1,(size_t)pl); path[pl]=0;
        FILE *bf=fopen(path,"rb");
        if (!bf && a->incbin_dir){ char full[512];
            snprintf(full,sizeof full,"%s/%s",a->incbin_dir,path); bf=fopen(full,"rb"); }
        if (!bf){ aerr(a,ln,"cannot open incbin file",path); return; }
        unsigned char buf[32]; size_t r;
        while ((r=fread(buf,1,sizeof buf,bf))>0){
            struct item *it=new_item(a); it->kind=IT_BYTES; it->n=(int)r;
            memcpy(it->b,buf,r);
        }
        fclose(bf); return;
    }

    /* an instruction: parse up to 2 comma-separated operands */
    struct oper ops[2]; int no=0;
    char *save=rest, *tok;
    while ((tok=split(&save, ',')) && no<2){
        while(*tok==' '||*tok=='\t') tok++;
        char *e=tok+strlen(tok); while(e>tok&&(e[-1]==' '||e[-1]=='\t'))*--e=0;
        /* mangle a bare `.local` jump/call target */
        if (tok[0]=='.'){ char mg[128]; mangle(a,tok,mg,sizeof mg); if(parse_oper(a,ln,mg,&ops[no])) return; }
        else if (parse_oper(a,ln,tok,&ops[no])) return;
        no++;
    }
    encode(a, ln, mn, ops, no);
}

/* ---------- preprocessor: strip comments, expand %macro ---------- */

struct macro { char *name; int nargs; char **body; int nbody; };

/* Replace %1..%9 in `src` with args[]; write to out (cap). */
static void subst(const char *src, char **args, int nargs, char *out, int cap){
    int o=0;
    for (const char *p=src; *p && o<cap-1; ){
        if (*p=='%' && p[1]>='1' && p[1]<='9'){
            int idx=p[1]-'1'; p+=2;
            if (idx<nargs){ const char*a=args[idx]; while(*a&&o<cap-1) out[o++]=*a++; }
        } else out[o++]=*p++;
    }
    out[o]=0;
}

static void strip_comment(char *s){
    int inq=0; char q=0;
    for (char *p=s; *p; p++){
        if (inq){ if(*p==q) inq=0; }
        else if (*p=='"'||*p=='\''){ inq=1; q=*p; }
        else if (*p==';'){ *p=0; break; }
    }
    int n=(int)strlen(s); while(n>0 && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
}

/* Expand the source file into a flat line list (macros inlined). */
static char **preprocess(struct as *a, const char *text, int *nout){
    char **lines=NULL; int nl=0, cap=0;
    struct macro *macs=NULL; int nmac=0, cmac=0;
    struct macro *defining=NULL;
    char *copy=xstrdup(text), *save=copy, *ln;
    while ((ln=split(&save,'\n'))){
        char buf[512]; strncpy(buf,ln,sizeof buf-1); buf[sizeof buf-1]=0;
        strip_comment(buf);
        char *t=buf; while(*t==' '||*t=='\t') t++;
        if (!*t) continue;
        if (!strncmp(t,"%macro",6)){
            char nm[64]; int na=0; sscanf(t+6," %63s %d",nm,&na);
            if (nmac==cmac){ cmac=cmac?cmac*2:8; macs=xrealloc(macs,(size_t)cmac*sizeof*macs); }
            defining=&macs[nmac++]; defining->name=xstrdup(nm); defining->nargs=na;
            defining->body=NULL; defining->nbody=0; continue;
        }
        if (!strncmp(t,"%endmacro",9)){ defining=NULL; continue; }
        if (defining){
            defining->body=xrealloc(defining->body,(size_t)(defining->nbody+1)*sizeof(char*));
            defining->body[defining->nbody++]=xstrdup(t); continue;
        }
        /* macro invocation? first token matches a macro name */
        char first[64]; int fi=0; while(t[fi]&&t[fi]!=' '&&t[fi]!='\t'&&fi<63){first[fi]=t[fi];fi++;} first[fi]=0;
        struct macro *mm=NULL;
        for (int i=0;i<nmac;i++) if(!strcmp(macs[i].name,first)) mm=&macs[i];
        if (mm){
            char *args[9]={0}; int na=0;
            char *asave=t+fi, *at;
            while ((at=split(&asave,',')) && na<9){
                while(*at==' '||*at=='\t') at++;
                char*e=at+strlen(at); while(e>at&&(e[-1]==' '||e[-1]=='\t'))*--e=0;
                args[na++]=xstrdup(at);
            }
            for (int b=0;b<mm->nbody;b++){
                char ex[512]; subst(mm->body[b],args,na,ex,sizeof ex);
                if (nl==cap){ cap=cap?cap*2:256; lines=xrealloc(lines,(size_t)cap*sizeof(char*)); }
                lines[nl++]=xstrdup(ex);
            }
            for (int i=0;i<na;i++) free(args[i]);
            continue;
        }
        if (nl==cap){ cap=cap?cap*2:256; lines=xrealloc(lines,(size_t)cap*sizeof(char*)); }
        lines[nl++]=xstrdup(t);
    }
    free(copy);
    *nout=nl; (void)a; return lines;
}

/* ---------- placement (with jump relaxation) ---------- */
static long jmp_size(struct item *it){ return it->wide ? (it->op32b?6:5) : 2; }

static void place(struct as *a){
    int changed=1, guard=0;
    while (changed && guard++<200){
        changed=0; long off[SEC_N]={0,0,0,0};
        for (int i=0;i<a->nit;i++){
            struct item *it=&a->it[i]; int s=it->sec;
            if (it->kind==IT_LABEL){ struct sym*sy=sym_get(a,it->jsym); sy->sec=s; sy->off=off[s]; sy->defined=1; continue; }
            it->off=off[s];
            if (it->kind==IT_JMP) off[s]+=jmp_size(it);
            else if (it->fix_kind==-2){ long al=it->fix_add; if(al>1) off[s]+= (al-(off[s]%al))%al; }
            else if (it->fix_kind==-3) off[s]+=it->fix_add;   /* .bss reserve */
            else off[s]+=it->n;
        }
        for (int i=0;i<a->nit;i++){
            struct item *it=&a->it[i];
            if (it->kind!=IT_JMP || it->wide) continue;
            struct sym*t=sym_find(a,it->jsym);
            if (!t||!t->defined) continue;
            long rel=t->off-(it->off+2);
            if (rel<-128||rel>127){ it->wide=1; changed=1; }
        }
        for (int s=0;s<SEC_N;s++) a->secsize[s]=off[s];
    }
}

/* ---------- emit + output ---------- */

struct outrel { long off; char *sym; int kind; long add; };  /* .text relocs */
struct secbuf { unsigned char *p; long n, cap; };

/* Fill each section's final bytes; collect .text relocations. */
static void emit_bytes(struct as *a, struct secbuf out[SEC_N],
                       struct outrel **rels, int *nrel){
    int cap=0; *rels=NULL; *nrel=0;
    for (int i=0;i<a->nit;i++){
        struct item *it=&a->it[i]; int s=it->sec;
        if (it->kind==IT_LABEL) continue;
        if (it->kind==IT_BYTES && it->fix_kind==-3) continue;   /* .bss reserve: no bytes */
        long need=0;
        if (it->kind==IT_JMP) need=jmp_size(it);
        else if (it->fix_kind==-2){ long al=it->fix_add; need=al>1?(al-(out[s].n%al))%al:0; }
        else need=it->n;
        if (out[s].n+need+8 > out[s].cap){ out[s].cap=(out[s].n+need+64)*2; out[s].p=xrealloc(out[s].p,(size_t)out[s].cap); }
        if (it->kind==IT_JMP){
            struct sym*t=sym_find(a,it->jsym);
            long tgt = t?t->off:0;
            long rel = tgt - (it->off + jmp_size(it));
            if (!it->wide){ out[s].p[out[s].n++]=(unsigned char)it->op8; out[s].p[out[s].n++]=(unsigned char)(rel&0xff); }
            else { if(it->op32b){ out[s].p[out[s].n++]=(unsigned char)it->op32a; out[s].p[out[s].n++]=(unsigned char)it->op32b; }
                   else out[s].p[out[s].n++]=(unsigned char)it->op32a;
                   for(int k=0;k<4;k++) out[s].p[out[s].n++]=(unsigned char)((rel>>(8*k))&0xff); }
            continue;
        }
        if (it->fix_kind==-2){ for(long k=0;k<need;k++) out[s].p[out[s].n++]=0x00; continue; }  /* align pad */
        long base=out[s].n;
        for (int k=0;k<it->n;k++) out[s].p[out[s].n++]=it->b[k];
        if (it->has_fix){
            struct sym*t=sym_find(a,it->fix_sym);
            int local = t && t->defined && !t->ext;
            if (it->fix_kind==FIX_REL32 && local && t->sec==s){
                long rel = t->off + it->fix_add + 4 - (base + it->fix_at + 4);  /* addend folded */
                /* fix_add is -4 (from end-of-insn convention); field = target - (site+4) */
                rel = t->off - (base + it->fix_at + 4);
                for (int k=0;k<4;k++) out[s].p[base+it->fix_at+k]=(unsigned char)((rel>>(8*k))&0xff);
            } else {
                /* a relocation: only .text supported by the writer (extern calls) */
                if (*nrel==cap){ cap=cap?cap*2:16; *rels=xrealloc(*rels,(size_t)cap*sizeof(struct outrel)); }
                (*rels)[*nrel].off=base+it->fix_at; (*rels)[*nrel].sym=it->fix_sym;
                (*rels)[*nrel].kind=it->fix_kind; (*rels)[*nrel].add=it->fix_add; (*nrel)++;
            }
        }
    }
}

static int write_elf(struct as *a, const char *out){
    struct secbuf sec[SEC_N]; memset(sec,0,sizeof sec);
    struct outrel *rels; int nrel;
    emit_bytes(a, sec, &rels, &nrel);

    struct elfw *w = elfw_new(EM_X86_64);
    int shndx[SEC_N]={0,0,0,0};
    /* Emit section headers in source-appearance order (nasm's convention), so
     * both the header table and the section symbols below match byte-for-byte. */
    for (int oi=0;oi<a->nsecorder;oi++){
        int s=a->secorder[oi];
        long sz = (s==SEC_BSS) ? a->secsize[SEC_BSS] : sec[s].n;
        if (sz==0) continue;
        Elf64_Word type = (s==SEC_BSS)?SHT_NOBITS:SHT_PROGBITS;
        Elf64_Xword fl = SHF_ALLOC | ((s==SEC_TEXT)?SHF_EXECINSTR:(s==SEC_RODATA?0:SHF_WRITE));
        shndx[s]=elfw_add_section(w, sec_name[s], type, fl,
                                  (s==SEC_BSS)?NULL:sec[s].p, sz, (s==SEC_TEXT)?16:8);
    }
    /* nasm emits a leading STT_FILE symbol (the input path, SHN_ABS). */
    elfw_add_symbol(w, a->file, 0, 0, ELF64_ST_INFO(STB_LOCAL,STT_FILE), SHN_ABS);
    /* Section symbols (STT_SECTION, local) next, in the same appearance order —
     * a relocation against a symbol DEFINED in this object references its section
     * symbol + addend, as nasm does. Then local labels, globals, externs. */
    int secsym[SEC_N]; for (int s=0;s<SEC_N;s++) secsym[s]=-1;
    for (int oi=0;oi<a->nsecorder;oi++){
        int s=a->secorder[oi];
        if (shndx[s])
            secsym[s]=elfw_add_symbol(w, "", 0, 0,   /* section symbols: st_name=0 (nasm/gcc) */
                                      ELF64_ST_INFO(STB_LOCAL,STT_SECTION),
                                      (Elf64_Half)shndx[s]);
    }
    /* nasm's symtab order: local symbols first, then global (ELF requires it),
     * and within each bind class by definition sequence — the source point where
     * a label is defined or a symbol is declared `extern` (a forward `global`
     * does not count). externs (global, undefined) interleave with defined
     * globals by that same sequence. Build the order, then emit. */
    int *symidx=xcalloc((size_t)(a->nsy?a->nsy:1),sizeof(int));
    for (int i=0;i<a->nsy;i++) symidx[i]=-1;
    int *ord=xcalloc((size_t)(a->nsy?a->nsy:1),sizeof(int)), no=0;
    for (int grp=0; grp<=1; grp++){        /* 0 = locals (defined non-global), 1 = globals + externs */
        int start=no;
        for (int i=0;i<a->nsy;i++){
            struct sym*sy=&a->sy[i];
            int is_local = sy->defined && !sy->global;
            if ((grp==0) == (is_local!=0)) ord[no++]=i;
        }
        for (int i=start+1;i<no;i++){       /* stable insertion sort by seq */
            int v=ord[i], j=i-1;
            while (j>=start && a->sy[ord[j]].seq > a->sy[v].seq){ ord[j+1]=ord[j]; j--; }
            ord[j+1]=v;
        }
    }
    for (int oi=0;oi<no;oi++){
        int i=ord[oi]; struct sym*sy=&a->sy[i];
        int is_local = sy->defined && !sy->global;
        Elf64_Uchar info = ELF64_ST_INFO(is_local?STB_LOCAL:STB_GLOBAL, STT_NOTYPE);
        Elf64_Half sh = sy->defined ? (Elf64_Half)shndx[sy->sec] : SHN_UNDEF;
        symidx[i]=elfw_add_symbol(w, sy->name, sy->defined?sy->off:0, 0, info, sh);
    }
    free(ord);
    for (int i=0;i<nrel;i++){
        struct sym*t=sym_find(a,rels[i].sym);
        int type = rels[i].kind==FIX_ABS64?R_X86_64_64:rels[i].kind==FIX_ABS32?R_X86_64_32:R_X86_64_PC32;
        int sidx; long add=rels[i].add;
        if (t && t->defined && secsym[t->sec]>=0){    /* defined -> section + offset */
            sidx=secsym[t->sec]; add += t->off;
        } else sidx=symidx[t-a->sy];                   /* extern -> the symbol */
        elfw_add_rela(w, shndx[SEC_TEXT], rels[i].off, sidx, type, add);
    }
    int rc = elfw_write(w, out);
    elfw_free(w); free(symidx); free(rels);
    for (int s=0;s<SEC_N;s++) free(sec[s].p);
    return rc;
}

int as_assemble(const char *in_path, const char *out_path, enum as_format fmt){
    FILE *f=fopen(in_path,"rb");
    if (!f){ fprintf(stderr,"embas: cannot open %s\n",in_path); return 1; }
    fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
    char *text=xmalloc((size_t)len+1); if(fread(text,1,(size_t)len,f)!=(size_t)len){fclose(f);return 1;} text[len]=0; fclose(f);

    struct as a; memset(&a,0,sizeof a); a.file=in_path; a.cur=SEC_TEXT; a.bits=64;
    int nl; char **lines=preprocess(&a,text,&nl);
    for (int i=0;i<nl;i++){ process_line(&a,i+1,lines[i]); if(a.err) return 1; }
    place(&a);
    if (a.err) return 1;
    if (fmt==AS_BIN){ fprintf(stderr,"embas: -f bin not yet implemented\n"); return 1; }
    return write_elf(&a, out_path);
}

