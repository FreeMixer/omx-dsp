// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx-rt-gate — one static real-time gate for plugin and engine C (omx-rt-gate(1)).
 *
 * The input is the compiler's own call graph: every real-time translation unit of a consumer is
 * compiled with the consumer's flags and `-O0 -fno-inline -fcallgraph-info` (macros expanded, a
 * static function named per unit, an indirect call marked at its site), and preprocessed with the
 * same flags. The graphs are joined by symbol name to the graphs omx-dsp ships for its compiled
 * units. -O2 is never an input: dead-code removal takes a planted malloc out of the graph.
 *
 * The roots are derived, never listed: the ABI tables of the preprocessed unit (an LV2_Descriptor,
 * a clap_plugin, a pw_filter_events initializer or a store into one of their fields) name the
 * functions a host enters, each field's thread role declared once in the ABI's roles file.
 *
 * From each root the walk reads every call:
 *   - a forbidden symbol (forbidden.tbl: allocation, lock, blocking, I/O, logging) is red;
 *   - an external symbol is classified by classes.tbl (maths, bulk memory, word atomic, clock, FTZ
 *     register, wake) or it is red;
 *   - an omx-dsp function tagged `@thread worker` or `@thread control` reached from an audio root
 *     is red;
 *   - an indirect call is classified from the field at its site: a declared role of a bound ABI, or a
 *     vtable whose targets are the functions an initializer or a store puts into that field;
 *     anything else is red;
 *   - the worker's result crosses by value (lv2.abi `result` and `result-out` rows).
 *
 * A red reads `<code> <symbol> at <file>:<line>:<col> via <root>[<role>] -> ...`. Exit 1 red, 2 not
 * judged (a table refused at load, a unit that does not compile, no root, the positive control
 * blind), 0 only when every root was walked and nothing is red. The positive control runs first:
 * a planted malloc behind a probe root, through the same compiler, tables and walk.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef OMX_RT_GATE_DATADIR
#define OMX_RT_GATE_DATADIR "/usr/share/omxdsp/rt-gate"
#endif

/* ---- small things ---------------------------------------------------------------------------- */

static void die_nj(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die_nj(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("NOT-JUDGED ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
  fflush(stdout);
  exit(2);
}

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) die_nj("out of memory");
  return q;
}
static char *xstrdup(const char *s) {
  char *d = strdup(s);
  if (!d) die_nj("out of memory");
  return d;
}
static char *xstrndup(const char *s, size_t n) {
  char *d = strndup(s, n);
  if (!d) die_nj("out of memory");
  return d;
}
static char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *xasprintf(const char *fmt, ...) {
  va_list ap;
  char *s = NULL;
  va_start(ap, fmt);
  if (vasprintf(&s, fmt, ap) < 0) die_nj("out of memory");
  va_end(ap);
  return s;
}

#define PUSH(arr, n, cap, val)                                     \
  do {                                                             \
    if ((n) == (cap)) {                                            \
      (cap) = (cap) ? 2 * (cap) : 16;                              \
      (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
    }                                                              \
    (arr)[(n)++] = (val);                                          \
  } while (0)

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 1 << 16, n = 0;
  char *b = xrealloc(NULL, cap);
  size_t r;
  while ((r = fread(b + n, 1, cap - n - 1, f)) > 0) {
    n += r;
    if (cap - n < 2) b = xrealloc(b, cap *= 2);
  }
  fclose(f);
  b[n] = 0;
  if (len) *len = n;
  return b;
}

static int is_ident(const char *s) {
  if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
  for (; *s; s++)
    if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
  return 1;
}

/* string -> int map, open addressing */
struct map { char **k; int *v; size_t cap, n; };
static size_t hstr(const char *s) {
  size_t h = 1469598103934665603ull;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
  return h;
}
static void map_grow(struct map *m);
static int *map_slot(struct map *m, const char *k, int create) {
  if (!m->cap) {
    if (!create) return NULL;
    m->cap = 64;
    m->k = calloc(m->cap, sizeof *m->k);
    m->v = calloc(m->cap, sizeof *m->v);
    if (!m->k || !m->v) die_nj("out of memory");
  }
  size_t i = hstr(k) & (m->cap - 1);
  while (m->k[i]) {
    if (!strcmp(m->k[i], k)) return &m->v[i];
    i = (i + 1) & (m->cap - 1);
  }
  if (!create) return NULL;
  if (2 * (m->n + 1) > m->cap) {
    map_grow(m);
    return map_slot(m, k, 1);
  }
  m->k[i] = xstrdup(k);
  m->v[i] = -1;
  m->n++;
  return &m->v[i];
}
static void map_grow(struct map *m) {
  struct map g = {0};
  g.cap = m->cap * 2;
  g.k = calloc(g.cap, sizeof *g.k);
  g.v = calloc(g.cap, sizeof *g.v);
  if (!g.k || !g.v) die_nj("out of memory");
  for (size_t i = 0; i < m->cap; i++)
    if (m->k[i]) {
      *map_slot(&g, m->k[i], 1) = m->v[i];
      free(m->k[i]);
    }
  free(m->k);
  free(m->v);
  *m = g;
}
static int map_get(struct map *m, const char *k) {
  int *p = map_slot(m, k, 0);
  return p ? *p : -1;
}
static void map_put(struct map *m, const char *k, int v) { *map_slot(m, k, 1) = v; }

/* ---- the closed sets -------------------------------------------------------------------------- */

enum role { ROLE_AUDIO, ROLE_WORKER, ROLE_CONTROL, ROLE_NONE };
static const char *ROLE_NAMES[] = {"audio", "worker", "control"};
static int role_of(const char *s) {
  for (int i = 0; i < 3; i++)
    if (!strcmp(s, ROLE_NAMES[i])) return i;
  return ROLE_NONE;
}

/* the codes a forbidden row may carry */
static const char *FORBIDDEN_CODES[] = {"RT-ALLOC", "RT-LOCK", "RT-BLOCK", "RT-IO", "RT-LOG", NULL};
/* the classes a classification row may carry: nothing else, so no waiver, no known red, no
 * "cannot prove" */
static const char *CLASSES[] = {"maths", "bulk-memory", "word-atomic", "clock", "ftz-register", "wake", NULL};

static int in_set(const char *s, const char **set) {
  for (int i = 0; set[i]; i++)
    if (!strcmp(s, set[i])) return 1;
  return 0;
}

/* ---- the tables ------------------------------------------------------------------------------- */

struct forb { char *sym, *code, *reason; };
struct cls { char *sym, *cls, *arm, *reason; char *where; };
enum abikind { AB_PLUGIN, AB_HOST, AB_HANDED, AB_RESULT, AB_RESULT_OUT };
struct abirow { int kind; char *abi, *type, *field; int role, a, b; char *where; };

static struct forb *FORB; static int NFORB, CFORB;
static struct cls *CLS; static int NCLS, CCLS;
static struct abirow *ABI; static int NABI, CABI;
static struct map FORB_MAP, CLS_MAP;

static int refusals;
static void refuse(const char *where, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void refuse(const char *where, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("REFUSED %s: ", where);
  vprintf(fmt, ap);
  putchar('\n');
  va_end(ap);
  refusals++;
}

/* one table line split into words; the reason is the rest of the line after `nwords` words */
static int split_words(char *line, char **w, int max, char **rest) {
  int n = 0;
  char *p = line;
  *rest = NULL;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (n == max) {
      *rest = p;
      break;
    }
    w[n++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
  }
  if (*rest) {
    char *e = *rest + strlen(*rest);
    while (e > *rest && isspace((unsigned char)e[-1])) *--e = 0;
    if (!**rest) *rest = NULL;
  }
  return n;
}

static char **table_lines(const char *path, int *nlines) {
  char *text = slurp(path, NULL);
  if (!text) die_nj("table %s cannot be read: %s", path, strerror(errno));
  char **lines = NULL;
  int n = 0, cap = 0;
  for (char *p = text, *e; *p; p = e) {
    e = strchr(p, '\n');
    if (e) *e++ = 0;
    else e = p + strlen(p);
    char *h = strchr(p, '#');
    if (h) *h = 0;
    PUSH(lines, n, cap, p);
  }
  *nlines = n;
  return lines;
}

/* a symbol a row may name: a C identifier, never a site (file:line), a field (a.b, p->b), a file
 * (x.c) or a function of ours (omx_*, omxdsp_*) */
static const char *waiver_shape(const char *s) {
  if (strchr(s, ':')) return "a site";
  if (strstr(s, "->") || strchr(s, '.')) {
    size_t n = strlen(s);
    if (n > 2 && s[n - 2] == '.' && (s[n - 1] == 'c' || s[n - 1] == 'h')) return "a file";
    return "a field";
  }
  if (strchr(s, '/')) return "a file";
  if (!strncmp(s, "omx_", 4) || !strncmp(s, "omxdsp_", 7) || !strncmp(s, "OMX", 3)) return "a function of ours";
  if (!is_ident(s)) return "no symbol";
  return NULL;
}

static void load_forbidden(const char *path) {
  int n;
  char **lines = table_lines(path, &n);
  for (int i = 0; i < n; i++) {
    char *w[2], *rest;
    int k = split_words(lines[i], w, 2, &rest);
    if (k == 0) continue;
    char *where = xasprintf("%s:%d", path, i + 1);
    if (k < 2 || !rest) { refuse(where, "a forbidden row is `<symbol> <code> <reason>`"); continue; }
    const char *shape = waiver_shape(w[0]);
    if (shape) { refuse(where, "`%s` names %s, a row names a symbol", w[0], shape); continue; }
    if (!in_set(w[1], FORBIDDEN_CODES)) { refuse(where, "`%s` is not a forbidden code (RT-ALLOC RT-LOCK RT-BLOCK RT-IO RT-LOG)", w[1]); continue; }
    if (map_get(&FORB_MAP, w[0]) >= 0) { refuse(where, "`%s` is forbidden twice", w[0]); continue; }
    map_put(&FORB_MAP, w[0], NFORB);
    PUSH(FORB, NFORB, CFORB, ((struct forb){xstrdup(w[0]), xstrdup(w[1]), xstrdup(rest)}));
  }
}

static int arm_shape(const char *a) {
  const char *c = strchr(a, ':');
  if (!c || c == a || !c[1]) return 0;
  for (const char *p = a; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == ':' || *p == '/' || *p == '.')) return 0;
  return 1;
}

static void load_classes(const char *path) {
  int n;
  char **lines = table_lines(path, &n);
  for (int i = 0; i < n; i++) {
    char *w[3], *rest;
    int k = split_words(lines[i], w, 3, &rest);
    if (k == 0) continue;
    char *where = xasprintf("%s:%d", path, i + 1);
    if (k < 3 || !rest) { refuse(where, "a classification row is `<symbol> <class> <arm>:<case> <reason>`: no reason or no arm"); continue; }
    const char *shape = waiver_shape(w[0]);
    if (shape) { refuse(where, "`%s` names %s, a row names a symbol: a waiver is refused", w[0], shape); continue; }
    if (!in_set(w[1], CLASSES)) { refuse(where, "`%s` is not a class (maths bulk-memory word-atomic clock ftz-register wake)", w[1]); continue; }
    if (!arm_shape(w[2])) { refuse(where, "`%s` names no arm that measured it (`<arm>:<case>`)", w[2]); continue; }
    if (map_get(&FORB_MAP, w[0]) >= 0) { refuse(where, "`%s` is forbidden: a forbidden symbol has no class", w[0]); continue; }
    if (map_get(&CLS_MAP, w[0]) >= 0) { refuse(where, "`%s` is classified twice", w[0]); continue; }
    map_put(&CLS_MAP, w[0], NCLS);
    PUSH(CLS, NCLS, CCLS, ((struct cls){xstrdup(w[0]), xstrdup(w[1]), xstrdup(w[2]), xstrdup(rest), where}));
  }
}

/* an ABI's roles file:
 *   plugin <type> <field> <role>        the bound repo provides it: a function stored there is a root
 *   host <type> <field> <role>          the other side provides it: a call through it has that role
 *   handed <type> <field> <param> <role>  the function stored in <type>.<field> is handed, as its
 *                                       parameter <param>, a function of that role (LV2 work's respond)
 *   result <type> <field> <param>       that root's parameter <param> is the worker's result: used at
 *                                       most once, as the source of a by-value copy
 *   result-out <type> <field> <param> <arg>  in that root, calls through parameter <param> pass the
 *                                       result as argument <arg>: the address of a pointer-free local */
static void load_abi(const char *path, const char *abi) {
  int n;
  char **lines = table_lines(path, &n);
  for (int i = 0; i < n; i++) {
    char *w[6], *rest;
    int k = split_words(lines[i], w, 6, &rest);
    if (k == 0) continue;
    char *where = xasprintf("%s:%d", path, i + 1);
    struct abirow r = {0};
    r.abi = xstrdup(abi);
    r.where = where;
    r.role = ROLE_NONE;
    r.a = r.b = -1;
    if (!strcmp(w[0], "plugin") || !strcmp(w[0], "host")) {
      if (k != 4 || rest || role_of(w[3]) > ROLE_WORKER) { refuse(where, "`%s <type> <field> audio|worker`", w[0]); continue; }
      r.kind = w[0][0] == 'p' ? AB_PLUGIN : AB_HOST;
      r.role = role_of(w[3]);
    } else if (!strcmp(w[0], "handed")) {
      if (k != 5 || rest || role_of(w[4]) > ROLE_WORKER || !isdigit((unsigned char)w[3][0])) { refuse(where, "`handed <type> <field> <param> audio|worker`"); continue; }
      r.kind = AB_HANDED;
      r.a = atoi(w[3]);
      r.role = role_of(w[4]);
    } else if (!strcmp(w[0], "result")) {
      if (k != 4 || rest || !isdigit((unsigned char)w[3][0])) { refuse(where, "`result <type> <field> <param>`"); continue; }
      r.kind = AB_RESULT;
      r.a = atoi(w[3]);
    } else if (!strcmp(w[0], "result-out")) {
      if (k != 5 || rest || !isdigit((unsigned char)w[3][0]) || !isdigit((unsigned char)w[4][0])) { refuse(where, "`result-out <type> <field> <param> <arg>`"); continue; }
      r.kind = AB_RESULT_OUT;
      r.a = atoi(w[3]);
      r.b = atoi(w[4]);
    } else {
      refuse(where, "`%s` is not a row kind (plugin host handed result result-out)", w[0]);
      continue;
    }
    if (!is_ident(w[1]) || !is_ident(w[2])) { refuse(where, "a role names a type and a field"); continue; }
    r.type = xstrdup(w[1]);
    r.field = xstrdup(w[2]);
    int dup = 0;
    for (int j = 0; j < NABI; j++)
      if (ABI[j].kind == r.kind && !strcmp(ABI[j].type, r.type) && !strcmp(ABI[j].field, r.field) && ABI[j].a == r.a)
        dup = 1;
    if (dup) { refuse(where, "`%s.%s` is declared twice", r.type, r.field); continue; }
    PUSH(ABI, NABI, CABI, r);
  }
}

/* the bound ABIs: `lv2` binds the plugin side, `lv2:host` the host side */
struct bound { char *abi; int host; };
static struct bound *BOUND; static int NBOUND, CBOUND;

static int bound_side(const struct abirow *r) {
  for (int i = 0; i < NBOUND; i++)
    if (!strcmp(BOUND[i].abi, r->abi)) return BOUND[i].host;
  return -1;
}
/* the rows whose functions this binding provides (its roots) */
static int row_is_root(const struct abirow *r) {
  int s = bound_side(r);
  return (s == 0 && r->kind == AB_PLUGIN) || (s == 1 && r->kind == AB_HOST);
}
/* the rows the other side provides (callable from here, with their role) */
static int row_is_callable(const struct abirow *r) {
  int s = bound_side(r);
  return (s == 0 && r->kind == AB_HOST) || (s == 1 && r->kind == AB_PLUGIN);
}

/* ---- thread tags (omx-dsp's `@thread` lines, one per function) -------------------------------- */

static struct map TAGS;
static int NTAGS;
static void load_tags(const char *path) {
  int n;
  char **lines = table_lines(path, &n);
  for (int i = 0; i < n; i++) {
    char *w[3], *rest;
    int k = split_words(lines[i], w, 3, &rest);
    if (k == 0) continue;
    char *where = xasprintf("%s:%d", path, i + 1);
    if (k < 2 || role_of(w[1]) == ROLE_NONE || !is_ident(w[0])) { refuse(where, "a tag row is `<function> audio|worker|control [<file>:<line>]`"); continue; }
    if (map_get(&TAGS, w[0]) >= 0) { refuse(where, "`%s` carries two tags", w[0]); continue; }
    map_put(&TAGS, w[0], role_of(w[1]));
    NTAGS++;
  }
}

/* ---- the preprocessed unit: tokens, structs, typedefs, functions, stores ----------------------- */

enum { TK_ID, TK_NUM, TK_STR, TK_P };
struct tok { const char *s; int len; int kind; int file; int line; int depth; };

struct member { char *name; char *tag; char *tdef; int ptr; int fnptr; int array; };
struct sdef { char *tag; struct member *m; int n, cap; int known; };
struct tdef { char *name; char *tag; int ptr; int scalar; char *base; };
struct fdef { char *name; int file, line; char **params; int nparams; int open, close; int defined; int tok; };
struct store { char *tag; char *field; char *fn; int tok; };

struct unit {
  char *src;      /* the unit as the graph titles it */
  char *ipath;    /* its preprocessed text */
  char *cipath;   /* its call graph */
  char *text;
  struct tok *t; int nt, ct;
  char **files; int nfiles, cfiles;
  struct sdef *sd; int nsd, csd; struct map sdmap;
  struct tdef *td; int ntd, ctd; struct map tdmap;
  struct fdef *fd; int nfd, cfd; struct map fdmap;
  struct store *st; int nst, cst;
  int anon;
};

static int tis(const struct tok *t, const char *s) {
  return (int)strlen(s) == t->len && !memcmp(t->s, s, (size_t)t->len);
}
static char *tstr(const struct tok *t) { return xstrndup(t->s, (size_t)t->len); }

static int file_id(struct unit *u, const char *name, size_t n) {
  for (int i = 0; i < u->nfiles; i++)
    if (strlen(u->files[i]) == n && !memcmp(u->files[i], name, n)) return i;
  PUSH(u->files, u->nfiles, u->cfiles, xstrndup(name, n));
  return u->nfiles - 1;
}

static void lex(struct unit *u) {
  const char *p = u->text;
  int line = 1, file = file_id(u, u->src, strlen(u->src)), bol = 1, depth = 0;
  static const char *P3[] = {"...", "<<=", ">>=", NULL};
  static const char *P2[] = {"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=",
                             "*=", "/=", "%=", "&=", "|=", "^=", "##", NULL};
  while (*p) {
    if (*p == '\n') { line++; bol = 1; p++; continue; }
    if (isspace((unsigned char)*p)) { p++; continue; }
    if (bol && *p == '#') {
      /* a line marker `# 12 "file" 1 3`, or a #pragma kept by the preprocessor */
      const char *q = p + 1;
      while (*q == ' ' || *q == '\t') q++;
      if (!strncmp(q, "line", 4)) q += 4;
      while (*q == ' ' || *q == '\t') q++;
      if (isdigit((unsigned char)*q)) {
        int ln = atoi(q);
        while (isdigit((unsigned char)*q)) q++;
        while (*q == ' ') q++;
        if (*q == '"') {
          const char *e = strchr(q + 1, '"');
          if (e) file = file_id(u, q + 1, (size_t)(e - q - 1));
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
        line = ln;
        continue;
      }
      while (*p && *p != '\n') p++;
      continue;
    }
    bol = 0;
    struct tok t = {p, 0, TK_P, file, line, depth};
    if (isalpha((unsigned char)*p) || *p == '_' || *p == '$') {
      const char *q = p;
      while (isalnum((unsigned char)*q) || *q == '_' || *q == '$') q++;
      t.kind = TK_ID;
      t.len = (int)(q - p);
    } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
      const char *q = p;
      while (isalnum((unsigned char)*q) || *q == '.' || *q == '_' ||
             ((*q == '+' || *q == '-') && strchr("eEpP", q[-1])))
        q++;
      t.kind = TK_NUM;
      t.len = (int)(q - p);
    } else if (*p == '"' || *p == '\'') {
      const char *q = p + 1;
      while (*q && *q != *p) {
        if (*q == '\\' && q[1]) q++;
        if (*q == '\n') line++;
        q++;
      }
      if (*q) q++;
      t.kind = TK_STR;
      t.len = (int)(q - p);
    } else {
      t.len = 1;
      for (int i = 0; P3[i]; i++)
        if (!strncmp(p, P3[i], 3)) t.len = 3;
      if (t.len == 1)
        for (int i = 0; P2[i]; i++)
          if (!strncmp(p, P2[i], 2)) t.len = 2;
      if (*p == '{') depth++;
      if (*p == '}') { depth--; t.depth = depth; }
    }
    PUSH(u->t, u->nt, u->ct, t);
    p += t.len;
  }
}

/* the matching close of an open bracket at i, or u->nt */
static int match(struct unit *u, int i) {
  char o = u->t[i].s[0], c = o == '(' ? ')' : o == '[' ? ']' : '}';
  int d = 0;
  for (int j = i; j < u->nt; j++) {
    if (u->t[j].kind != TK_P || u->t[j].len != 1) continue;
    if (u->t[j].s[0] == o) d++;
    else if (u->t[j].s[0] == c && --d == 0) return j;
  }
  return u->nt;
}
/* the matching open of a close bracket at i, or -1 */
static int rmatch(struct unit *u, int i) {
  char c = u->t[i].s[0], o = c == ')' ? '(' : c == ']' ? '[' : '{';
  int d = 0;
  for (int j = i; j >= 0; j--) {
    if (u->t[j].kind != TK_P || u->t[j].len != 1) continue;
    if (u->t[j].s[0] == c) d++;
    else if (u->t[j].s[0] == o && --d == 0) return j;
  }
  return -1;
}
static int is_open(const struct tok *t) { return t->kind == TK_P && t->len == 1 && strchr("([{", t->s[0]); }
static int is_p(const struct tok *t, char c) { return t->kind == TK_P && t->len == 1 && t->s[0] == c; }

static const char *SCALAR_WORDS[] = {"void", "char", "short", "int", "long", "float", "double", "signed",
                                     "unsigned", "_Bool", "bool", "_Complex", "__int128", "_Float16", "_Float32",
                                     "_Float64", "__fp16", NULL};
static const char *QUAL_WORDS[] = {"const", "volatile", "restrict", "__restrict", "__restrict__", "static",
                                   "extern", "inline", "__inline", "__inline__", "register", "_Thread_local",
                                   "__thread", "_Atomic", "__extension__", "_Alignas", "auto", "typedef", NULL};
static int tin(const struct tok *t, const char **set) {
  if (t->kind != TK_ID) return 0;
  for (int i = 0; set[i]; i++)
    if (tis(t, set[i])) return 1;
  return 0;
}
static int is_attr(const struct tok *t) {
  return t->kind == TK_ID && (tis(t, "__attribute__") || tis(t, "__attribute") || tis(t, "__asm__") ||
                              tis(t, "__asm") || tis(t, "asm") || tis(t, "__declspec") || tis(t, "_Alignas") ||
                              tis(t, "__typeof__") || tis(t, "typeof") || tis(t, "__typeof"));
}

/* a pointer star in [a, b), outside braces (a struct body) and attribute groups */
static int has_star(struct unit *u, int a, int b) {
  for (int k = a; k < b; k++) {
    if (is_p(&u->t[k], '{')) { k = match(u, k); continue; }
    if (is_attr(&u->t[k]) && k + 1 < b && is_p(&u->t[k + 1], '(')) { k = match(u, k + 1); continue; }
    if (is_p(&u->t[k], '*')) return 1;
  }
  return 0;
}

static struct sdef *sdef_of(struct unit *u, const char *tag, int create) {
  int i = map_get(&u->sdmap, tag);
  if (i >= 0) return &u->sd[i];
  if (!create) return NULL;
  struct sdef s = {0};
  s.tag = xstrdup(tag);
  PUSH(u->sd, u->nsd, u->csd, s);
  map_put(&u->sdmap, tag, u->nsd - 1);
  return &u->sd[u->nsd - 1];
}
static struct tdef *tdef_of(struct unit *u, const char *name) {
  int i = map_get(&u->tdmap, name);
  return i >= 0 ? &u->td[i] : NULL;
}

/* the struct a type name means: a tag, or a typedef's tag (one level of typedef chains) */
static const char *canon_tag(struct unit *u, const char *name) {
  for (int hop = 0; hop < 8; hop++) {
    struct tdef *t = tdef_of(u, name);
    if (!t || t->ptr) break;
    if (t->tag) return t->tag;
    if (!t->base) break;
    name = t->base;
  }
  return sdef_of(u, name, 0) ? name : NULL;
}

/* one declarator in [a, b): its name, whether it is a pointer (or a function pointer), an array.
 * Attribute groups are skipped. Returns the name's token or -1. */
static int declarator(struct unit *u, int a, int b, int *ptr, int *fnptr, int *array) {
  int name = -1;
  *ptr = *fnptr = *array = 0;
  for (int i = a; i < b; i++) {
    struct tok *t = &u->t[i];
    if (is_attr(t)) {
      if (i + 1 < b && is_p(&u->t[i + 1], '(')) i = match(u, i + 1);
      continue;
    }
    if (is_p(t, '(')) {
      int e = match(u, i);
      /* (*name) or (* const name) or (CLAP_ABI *name): a function pointer */
      int star = -1, id = -1;
      for (int j = i + 1; j < e; j++) {
        if (is_p(&u->t[j], '*')) star = j;
        else if (u->t[j].kind == TK_ID && !tin(&u->t[j], QUAL_WORDS) && !is_attr(&u->t[j])) id = j;
        else if (is_p(&u->t[j], '[')) j = match(u, j);
      }
      if (star >= 0 && id > star) {
        name = id;
        *ptr = *fnptr = 1;
        return name;
      }
      if (name >= 0) return name; /* the parameter list of a function declarator */
      i = e;
      continue;
    }
    if (is_p(t, '{')) {
      i = match(u, i);
      name = -1;
      *ptr = 0;
      continue;
    }
    if (is_p(t, '[')) {
      *array = 1;
      i = match(u, i);
      continue;
    }
    if (is_p(t, '*')) *ptr = 1;
    if (is_p(t, ':') || is_p(t, '=')) break;
    if (t->kind == TK_ID && !tin(t, QUAL_WORDS)) name = i;
  }
  return name;
}

/* the type part of a declaration in [a, b) (b is the declarator's name): the struct tag it names
 * (anonymous ones were tagged by the struct pass at their `{`), the typedef name, scalar */
struct tyref { char *tag; char *tdef; int scalar; };
static struct map ANON_AT; /* "unit-token index" -> anonymous struct's synthetic tag index, per unit */

static struct tyref type_of(struct unit *u, int a, int b) {
  struct tyref r = {0};
  for (int i = a; i < b; i++) {
    struct tok *t = &u->t[i];
    if (is_attr(t)) {
      if (i + 1 < b && is_p(&u->t[i + 1], '(')) i = match(u, i + 1);
      continue;
    }
    if ((tis(t, "struct") || tis(t, "union")) && i + 1 < b) {
      if (u->t[i + 1].kind == TK_ID) {
        r.tag = tstr(&u->t[i + 1]);
        i++;
        if (i + 1 < b && is_p(&u->t[i + 1], '{')) i = match(u, i + 1);
      } else if (is_p(&u->t[i + 1], '{')) {
        char key[64];
        snprintf(key, sizeof key, "%s#%d", u->src, i + 1);
        int k = map_get(&ANON_AT, key);
        if (k >= 0) r.tag = xstrdup(u->sd[k].tag);
        i = match(u, i + 1);
      }
      continue;
    }
    if (tis(t, "enum")) {
      r.scalar = 1;
      if (i + 1 < b && u->t[i + 1].kind == TK_ID) i++;
      if (i + 1 < b && is_p(&u->t[i + 1], '{')) i = match(u, i + 1);
      continue;
    }
    if (tin(t, SCALAR_WORDS)) { r.scalar = 1; continue; }
    if (t->kind == TK_ID && !tin(t, QUAL_WORDS)) {
      char name[256];
      snprintf(name, sizeof name, "%.*s", t->len, t->s);
      if (tdef_of(u, name)) r.tdef = xstrdup(name);
    }
  }
  return r;
}

static void parse_struct_body(struct unit *u, int open, struct sdef *s);

/* every struct or union definition, anywhere: `struct tag {`, and anonymous ones, which take the
 * typedef name that declares them (`typedef struct { ... } LV2_Worker_Interface;`) or a synthetic tag */
static void pass_structs(struct unit *u) {
  for (int i = 0; i + 1 < u->nt; i++) {
    struct tok *t = &u->t[i];
    if (!(tis(t, "struct") || tis(t, "union"))) continue;
    int open = -1;
    char *tag = NULL;
    if (u->t[i + 1].kind == TK_ID && i + 2 < u->nt && is_p(&u->t[i + 2], '{')) {
      tag = tstr(&u->t[i + 1]);
      open = i + 2;
    } else if (is_p(&u->t[i + 1], '{')) {
      open = i + 1;
      int close = match(u, open);
      /* typedef context: the statement this struct opens starts with `typedef` */
      int typedef_ctx = 0;
      for (int j = i - 1; j >= 0; j--) {
        struct tok *q = &u->t[j];
        if (is_p(q, ';') || is_p(q, '{') || is_p(q, '}')) break;
        if (tis(q, "typedef")) typedef_ctx = 1;
      }
      if (typedef_ctx && close + 1 < u->nt) {
        int e = close + 1;
        while (e < u->nt && !is_p(&u->t[e], ';') && !is_p(&u->t[e], ',')) e++;
        int ptr, fp, arr, nm = declarator(u, close + 1, e, &ptr, &fp, &arr);
        if (nm >= 0 && !ptr) tag = tstr(&u->t[nm]);
      }
      if (!tag) tag = xasprintf("<anonymous %s#%d>", u->src, ++u->anon);
      char key[64];
      snprintf(key, sizeof key, "%s#%d", u->src, open);
      map_put(&ANON_AT, key, -1);
    } else {
      continue;
    }
    struct sdef *s = sdef_of(u, tag, 1);
    if (open >= 0 && !s->known) {
      char key[64];
      snprintf(key, sizeof key, "%s#%d", u->src, open);
      map_put(&ANON_AT, key, (int)(s - u->sd));
      parse_struct_body(u, open, s);
    }
  }
}

static void parse_struct_body(struct unit *u, int open, struct sdef *s) {
  int close = match(u, open);
  s->known = 1;
  int a = open + 1;
  for (int i = a; i <= close; i++) {
    if (i < close && is_open(&u->t[i])) { i = match(u, i); continue; }
    if (i < close && !is_p(&u->t[i], ';')) continue;
    if (i == a) { a = i + 1; continue; }
    /* one member declaration [a, i): split its declarators at depth-0 commas */
    int start = a, first = 1;
    int spec_end = -1;
    for (int j = a; j <= i; j++) {
      if (j < i && is_open(&u->t[j])) {
        j = match(u, j);
        continue;
      }
      if (j < i && !is_p(&u->t[j], ',')) continue;
      int ptr, fp, arr;
      int nm = declarator(u, first ? a : start, j, &ptr, &fp, &arr);
      if (nm >= 0) {
        if (first) spec_end = nm;
        /* a later declarator's pointer stars are its own */
        ptr = fp || has_star(u, first ? a : start, nm);
        struct tyref ty = type_of(u, a, spec_end >= 0 ? spec_end : nm);
        struct member m = {tstr(&u->t[nm]), ty.tag, ty.tdef, ptr, fp, arr};
        PUSH(s->m, s->n, s->cap, m);
      }
      first = 0;
      start = j + 1;
    }
    a = i + 1;
  }
}

/* every typedef: its name, and what it names (a struct tag, a pointer, a scalar, another typedef) */
static void pass_typedefs(struct unit *u) {
  for (int i = 0; i < u->nt; i++) {
    if (!tis(&u->t[i], "typedef")) continue;
    int e = i + 1;
    while (e < u->nt && !is_p(&u->t[e], ';')) {
      if (is_open(&u->t[e])) e = match(u, e);
      e++;
    }
    int start = i + 1, first = 1, spec_end = -1;
    for (int j = i + 1; j <= e && j < u->nt; j++) {
      if (j < e && is_open(&u->t[j])) { j = match(u, j); continue; }
      if (j < e && !is_p(&u->t[j], ',')) continue;
      int ptr, fp, arr;
      int nm = declarator(u, start, j, &ptr, &fp, &arr);
      if (nm >= 0) {
        if (first) spec_end = nm;
        struct tyref ty = type_of(u, i + 1, spec_end);
        int p = fp || has_star(u, start, nm);
        struct tdef d = {tstr(&u->t[nm]), ty.tag, p, ty.scalar, ty.tdef};
        if (map_get(&u->tdmap, d.name) < 0) {
          PUSH(u->td, u->ntd, u->ctd, d);
          map_put(&u->tdmap, d.name, u->ntd - 1);
        }
      }
      first = 0;
      start = j + 1;
    }
    i = e;
  }
}

/* the parameter names of a function declarator's list (open paren at po) */
static void params_of(struct unit *u, int po, struct fdef *f) {
  int pc = match(u, po);
  int a = po + 1;
  for (int j = a; j <= pc; j++) {
    if (j < pc && is_open(&u->t[j])) { j = match(u, j); continue; }
    if (j < pc && !is_p(&u->t[j], ',')) continue;
    int ptr, fp, arr;
    int nm = declarator(u, a, j, &ptr, &fp, &arr);
    char *name = NULL;
    if (nm >= 0) {
      /* a lone type (`void`, `int`, a typedef) names no parameter */
      char buf[256];
      snprintf(buf, sizeof buf, "%.*s", u->t[nm].len, u->t[nm].s);
      int only = 1;
      for (int k = a; k < j; k++)
        if (k != nm && u->t[k].kind == TK_ID && !tin(&u->t[k], QUAL_WORDS)) only = 0;
      if (!(only && (tin(&u->t[nm], SCALAR_WORDS) || tdef_of(u, buf)))) name = xstrdup(buf);
    }
    if (!(j == pc && j == a)) {
      f->params = xrealloc(f->params, (size_t)(f->nparams + 1) * sizeof *f->params);
      f->params[f->nparams++] = name;
    }
    a = j + 1;
  }
}

/* every function at file scope: definitions (`name(params) {`) and prototypes (`name(params);`) */
static void pass_functions(struct unit *u) {
  for (int i = 0; i + 1 < u->nt; i++) {
    struct tok *t = &u->t[i];
    if (t->depth != 0 || t->kind != TK_ID || !is_p(&u->t[i + 1], '(')) continue;
    if (tin(t, QUAL_WORDS) || tin(t, SCALAR_WORDS) || is_attr(t) || tis(t, "_Static_assert") || tis(t, "static_assert"))
      continue;
    /* not inside an initializer or a typedef: walk back to the statement's start */
    int bad = 0;
    for (int j = i - 1; j >= 0; j--) {
      struct tok *q = &u->t[j];
      if (q->depth != 0) continue;
      if (is_p(q, ';') || is_p(q, '}')) break;
      /* an attribute's group, `__attribute__((always_inline))`, is stepped over */
      if (is_p(q, ')')) {
        int o = rmatch(u, j);
        if (o > 0 && is_attr(&u->t[o - 1])) { j = o - 1; continue; }
        if (o > 1 && is_p(&u->t[o - 1], '(') && is_attr(&u->t[o - 2])) { j = o - 2; continue; }
        bad = 1;
        break;
      }
      if (is_p(q, '=') || tis(q, "typedef") || is_p(q, '(')) { bad = 1; break; }
    }
    if (bad) continue;
    int pc = match(u, i + 1);
    int k = pc + 1;
    while (k < u->nt && is_attr(&u->t[k])) k = (k + 1 < u->nt && is_p(&u->t[k + 1], '(')) ? match(u, k + 1) + 1 : k + 1;
    if (k >= u->nt) break;
    int def = is_p(&u->t[k], '{');
    if (!def && !is_p(&u->t[k], ';') && !is_p(&u->t[k], ',')) continue;
    char *name = tstr(t);
    int idx = map_get(&u->fdmap, name);
    if (idx >= 0 && (!def || u->fd[idx].defined)) { i = def ? match(u, k) : pc; continue; }
    struct fdef f = {0};
    f.name = name;
    f.file = t->file;
    f.line = t->line;
    f.tok = i;
    params_of(u, i + 1, &f);
    f.defined = def;
    if (def) {
      f.open = k;
      f.close = match(u, k);
    }
    if (idx >= 0) u->fd[idx] = f;
    else {
      PUSH(u->fd, u->nfd, u->cfd, f);
      map_put(&u->fdmap, name, u->nfd - 1);
    }
    i = def ? f.close : pc;
  }
}

static struct fdef *fdef_of(struct unit *u, const char *name) {
  int i = map_get(&u->fdmap, name);
  return i >= 0 ? &u->fd[i] : NULL;
}

/* a value [a, b) reduced to the function it names, or NULL: casts, `&` and parentheses stripped */
static char *value_fn(struct unit *u, int a, int b) {
  for (int guard = 0; guard < 16 && a < b; guard++) {
    if (is_p(&u->t[a], '&')) { a++; continue; }
    if (is_p(&u->t[a], '(')) {
      int e = match(u, a);
      if (e == b - 1) { a++; b--; continue; } /* (fn) */
      if (e < b - 1) { a = e + 1; continue; } /* a cast */
    }
    break;
  }
  if (b - a != 1 || u->t[a].kind != TK_ID) return NULL;
  char *n = tstr(&u->t[a]);
  if (fdef_of(u, n)) return n;
  free(n);
  return NULL;
}

static void add_store(struct unit *u, const char *tag, const char *field, char *fn, int tok) {
  struct store s = {tag ? xstrdup(tag) : NULL, xstrdup(field), fn, tok};
  PUSH(u->st, u->nst, u->cst, s);
}

static int member_index(struct sdef *s, const char *name) {
  for (int i = 0; s && i < s->n; i++)
    if (!strcmp(s->m[i].name, name)) return i;
  return -1;
}

/* an initializer list at `open` for a value of struct `tag` (or an array of them) */
static void parse_init(struct unit *u, int open, const char *tag, int array) {
  int close = match(u, open);
  struct sdef *s = tag ? sdef_of(u, tag, 0) : NULL;
  int pos = 0;
  int a = open + 1;
  for (int j = a; j <= close; j++) {
    if (j < close && is_open(&u->t[j])) { j = match(u, j); continue; }
    if (j < close && !is_p(&u->t[j], ',')) continue;
    if (j == a) { a = j + 1; continue; }
    /* one element [a, j) */
    int v = a;
    char *field = NULL;
    if (array) {
      if (is_p(&u->t[v], '[')) {
        v = match(u, v) + 1;
        if (v < j && is_p(&u->t[v], '=')) v++;
      }
      if (v < j && is_p(&u->t[v], '{')) parse_init(u, v, tag, 0);
      a = j + 1;
      continue;
    }
    if (is_p(&u->t[v], '.') && v + 1 < j && u->t[v + 1].kind == TK_ID) {
      field = tstr(&u->t[v + 1]);
      v += 2;
      /* a nested designator .a.b = : the outer field holds a struct; follow one level */
      if (v < j && is_p(&u->t[v], '.') && v + 1 < j && u->t[v + 1].kind == TK_ID) {
        int mi = member_index(s, field);
        const char *inner = mi >= 0 ? s->m[mi].tag : NULL;
        if (!inner && mi >= 0 && s->m[mi].tdef) inner = canon_tag(u, s->m[mi].tdef);
        char *f2 = tstr(&u->t[v + 1]);
        v += 2;
        if (v < j && is_p(&u->t[v], '=')) v++;
        char *fn = value_fn(u, v, j);
        if (fn) add_store(u, inner, f2, fn, v);
        a = j + 1;
        continue;
      }
      if (v < j && is_p(&u->t[v], '=')) v++;
      int mi = member_index(s, field);
      if (mi >= 0) pos = mi + 1;
    } else {
      if (s && pos < s->n) field = xstrdup(s->m[pos].name);
      pos++;
    }
    if (field && v < j) {
      int mi = member_index(s, field);
      if (is_p(&u->t[v], '{')) {
        if (mi >= 0) {
          const char *inner = s->m[mi].tag;
          if (!inner && s->m[mi].tdef) inner = canon_tag(u, s->m[mi].tdef);
          parse_init(u, v, inner, s->m[mi].array);
        }
      } else {
        char *fn = value_fn(u, v, j);
        if (fn) add_store(u, s ? s->tag : tag, field, fn, v);
      }
    }
    a = j + 1;
  }
}

/* the declaration a name at `nm` belongs to: its type tokens start after the previous ; { } at the
 * same depth (a later declarator of a list shares the first one's type) */
static int decl_start(struct unit *u, int nm) {
  int d = 0;
  for (int j = nm - 1; j >= 0; j--) {
    struct tok *q = &u->t[j];
    if (is_p(q, ')') || is_p(q, ']')) { d++; continue; }
    if (is_p(q, '(') || is_p(q, '[')) {
      if (d == 0) return j + 1;
      d--;
      continue;
    }
    if (d) continue;
    if (is_p(q, ';') || is_p(q, '{') || is_p(q, '}')) return j + 1;
  }
  return 0;
}

/* every initializer `T v = {...}`, `T v[] = {...}`, compound literal `(T){...}` and store
 * `x.f = fn;` / `x->f = fn;` */
static void pass_stores(struct unit *u) {
  for (int i = 1; i < u->nt; i++) {
    struct tok *t = &u->t[i];
    if (is_p(t, '{') && is_p(&u->t[i - 1], '=') && i >= 2) {
      int e = i - 2;
      int array = 0;
      while (e >= 0 && is_p(&u->t[e], ']')) {
        array = 1;
        e = rmatch(u, e) - 1;
      }
      while (e >= 0 && is_attr(&u->t[e])) e--;
      if (e < 0 || u->t[e].kind != TK_ID) continue;
      int a = decl_start(u, e);
      struct tyref ty = type_of(u, a, e);
      const char *tag = ty.tag ? ty.tag : ty.tdef ? canon_tag(u, ty.tdef) : NULL;
      if (ty.tdef && !ty.tag) {
        struct tdef *td = tdef_of(u, ty.tdef);
        if (td && td->ptr) tag = NULL;
      }
      if (tag) parse_init(u, i, tag, array);
      continue;
    }
    if (is_p(t, '{') && is_p(&u->t[i - 1], ')')) {
      int po = rmatch(u, i - 1);
      if (po < 1) continue;
      struct tok *b = &u->t[po - 1];
      if (b->kind == TK_ID && !tis(b, "return") && !tis(b, "sizeof")) continue; /* a call or a definition */
      if (is_p(b, ')') || is_p(b, ']')) continue;
      struct tyref ty = type_of(u, po + 1, i - 1);
      const char *tag = ty.tag ? ty.tag : ty.tdef ? canon_tag(u, ty.tdef) : NULL;
      int array = 0;
      for (int k = po + 1; k < i - 1; k++)
        if (is_p(&u->t[k], '[')) array = 1;
      if (tag) parse_init(u, i, tag, array);
      continue;
    }
    if (t->kind == TK_ID && i + 1 < u->nt && is_p(&u->t[i + 1], '=') &&
        (is_p(&u->t[i - 1], '.') || tis(&u->t[i - 1], "->"))) {
      if (u->t[i].depth == 0 && i >= 2 && (is_p(&u->t[i - 2], '{') || is_p(&u->t[i - 2], ','))) continue;
      int e = i + 2;
      while (e < u->nt && !is_p(&u->t[e], ';') && !is_p(&u->t[e], ',') && !is_p(&u->t[e], '}')) {
        if (is_open(&u->t[e])) e = match(u, e);
        e++;
      }
      char *fn = value_fn(u, i + 2, e);
      if (fn) {
        char *field = tstr(t);
        add_store(u, NULL, field, fn, i + 2);
        free(field);
      }
    }
  }
}

static void parse_unit(struct unit *u) {
  size_t n;
  u->text = slurp(u->ipath, &n);
  if (!u->text) die_nj("%s cannot be read", u->ipath);
  lex(u);
  pass_structs(u);
  pass_typedefs(u);
  /* member types named by typedefs defined after the struct: resolve again */
  for (int i = 0; i < u->nsd; i++)
    for (int k = 0; k < u->sd[i].n; k++)
      if (!u->sd[i].m[k].tag && u->sd[i].m[k].tdef) {
        const char *c = canon_tag(u, u->sd[i].m[k].tdef);
        if (c) u->sd[i].m[k].tag = xstrdup(c);
      }
  pass_functions(u);
  pass_stores(u);
  if (getenv("OMX_RT_GATE_DEBUG")) {
    for (int i = 0; i < u->nsd; i++) {
      fprintf(stderr, "struct %s:", u->sd[i].tag);
      for (int k = 0; k < u->sd[i].n; k++) fprintf(stderr, " %s%s", u->sd[i].m[k].name, u->sd[i].m[k].ptr ? "*" : "");
      fputc('\n', stderr);
    }
    for (int i = 0; i < u->ntd; i++) fprintf(stderr, "typedef %s -> %s%s\n", u->td[i].name, u->td[i].tag ? u->td[i].tag : u->td[i].base ? u->td[i].base : "-", u->td[i].ptr ? " *" : "");
    for (int i = 0; i < u->nfd; i++) if (u->fd[i].defined) fprintf(stderr, "fn %s/%d\n", u->fd[i].name, u->fd[i].nparams);
    for (int i = 0; i < u->nst; i++) fprintf(stderr, "store %s.%s = %s\n", u->st[i].tag ? u->st[i].tag : "?", u->st[i].field, u->st[i].fn);
  }
}

/* ---- the call graphs -------------------------------------------------------------------------- */

struct edge { int to; char *site; };
struct node {
  char *title, *name, *loc;
  int defined;
  int unit;          /* the unit whose graph defines it, -1 for a shipped graph or an external */
  struct edge *e; int ne, ce;
};
static struct node *N; static int NN, CN;
static struct map NODE_MAP;
static int INDIRECT = -1;

static int node_of(const char *title) {
  int i = map_get(&NODE_MAP, title);
  if (i >= 0) return i;
  struct node n = {0};
  n.title = xstrdup(title);
  const char *c = strrchr(title, ':');
  n.name = xstrdup(c ? c + 1 : title);
  n.unit = -1;
  PUSH(N, NN, CN, n);
  map_put(&NODE_MAP, title, NN - 1);
  return NN - 1;
}

/* the value of `key: "..."` on a .ci line */
static char *ci_field(const char *line, const char *key) {
  const char *p = strstr(line, key);
  if (!p) return NULL;
  p = strchr(p, '"');
  if (!p) return NULL;
  const char *e = p + 1;
  while (*e && *e != '"') {
    if (*e == '\\' && e[1]) e++;
    e++;
  }
  return xstrndup(p + 1, (size_t)(e - p - 1));
}

static int load_graph(const char *path, int unit) {
  char *text = slurp(path, NULL);
  if (!text) return -1;
  int nodes = 0;
  for (char *p = text, *e; *p; p = e) {
    e = strchr(p, '\n');
    if (e) *e++ = 0;
    else e = p + strlen(p);
    if (!strncmp(p, "node:", 5)) {
      char *title = ci_field(p, "title:");
      char *label = ci_field(p, "label:");
      if (!title || !label) continue;
      int i = node_of(title);
      if (!strcmp(title, "__indirect_call")) {
        INDIRECT = i;
        continue;
      }
      int defined = !strstr(p, "shape : ellipse");
      char *nl = strstr(label, "\\n");
      if (defined && !N[i].defined) {
        N[i].defined = 1;
        N[i].unit = unit;
        free(N[i].name);
        N[i].name = nl ? xstrndup(label, (size_t)(nl - label)) : xstrdup(label);
        if (nl) {
          char *l2 = nl + 2, *l3 = strstr(l2, "\\n");
          N[i].loc = l3 ? xstrndup(l2, (size_t)(l3 - l2)) : xstrdup(l2);
        }
      }
      nodes++;
    } else if (!strncmp(p, "edge:", 5)) {
      char *s = ci_field(p, "sourcename:");
      char *t = ci_field(p, "targetname:");
      char *l = ci_field(p, "label:");
      if (!s || !t) continue;
      int a = node_of(s), b = node_of(t);
      if (!strcmp(t, "__indirect_call")) INDIRECT = b;
      PUSH(N[a].e, N[a].ne, N[a].ce, ((struct edge){b, l ? l : xstrdup("?")}));
    }
  }
  return nodes;
}

/* a call's target as the join sees it: the defined node of the same title, wherever it came from */
static int resolve(int i) {
  if (N[i].defined) return i;
  int j = map_get(&NODE_MAP, N[i].name);
  return (j >= 0 && N[j].defined) ? j : i;
}

/* ---- sources: an indirect call's site read back ------------------------------------------------ */

struct srcfile { char *path; char **lines; int n; };
static struct srcfile *SRC; static int NSRC, CSRC;
static struct srcfile *source(const char *path) {
  for (int i = 0; i < NSRC; i++)
    if (!strcmp(SRC[i].path, path)) return &SRC[i];
  struct srcfile f = {xstrdup(path), NULL, 0};
  char *text = slurp(path, NULL);
  int cap = 0;
  if (text)
    for (char *p = text, *e; *p; p = e) {
      e = strchr(p, '\n');
      if (e) *e++ = 0;
      else e = p + strlen(p);
      PUSH(f.lines, f.n, cap, p);
    }
  PUSH(SRC, NSRC, CSRC, f);
  return &SRC[NSRC - 1];
}

/* "file:line:col" split */
static int site_split(const char *site, char **file, int *line, int *col) {
  const char *c2 = strrchr(site, ':');
  if (!c2) return 0;
  const char *c1 = c2 - 1;
  while (c1 > site && *c1 != ':') c1--;
  if (c1 <= site) return 0;
  *file = xstrndup(site, (size_t)(c1 - site));
  *line = atoi(c1 + 1);
  *col = atoi(c2 + 1);
  return 1;
}

/* the callee at an indirect site: the field it reads (`p->f(`, `s.f(`, `(p->f)(`), or the plain name
 * (`fp(`, `(*fp)(`). Returns 1 field, 2 plain name, 0 not read. `text` gets the callee's spelling. */
static int callee_at(const char *site, char **name, char **text) {
  char *file;
  int line, col;
  *name = *text = NULL;
  if (!site_split(site, &file, &line, &col)) return 0;
  struct srcfile *f = source(file);
  if (line < 1 || line > f->n) return 0;
  /* the text from the column on, up to four lines */
  char *buf = NULL;
  for (int l = line; l <= f->n && l < line + 4; l++) {
    const char *s = f->lines[l - 1];
    if (l == line) {
      if ((size_t)col - 1 > strlen(s)) return 0;
      s += col - 1;
    }
    size_t n = strlen(s), have = buf ? strlen(buf) : 0;
    buf = xrealloc(buf, have + n + 2);
    memcpy(buf + have, s, n);
    buf[have + n] = ' ';
    buf[have + n + 1] = 0;
  }
  if (!buf) return 0;
  /* scan the postfix chain: a primary, then . -> [ ] ( ) */
  const char *p = buf;
  int depth = 0;
  const char *last_field = NULL, *last_id = NULL;
  size_t last_field_n = 0, last_id_n = 0;
  int last_was_field = 0;
  const char *call_open = NULL;
  int kind_at_call = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '*' || *p == '&') { p++; continue; }
    if (isalpha((unsigned char)*p) || *p == '_') {
      const char *q = p;
      while (isalnum((unsigned char)*q) || *q == '_') q++;
      last_id = p;
      last_id_n = (size_t)(q - p);
      last_was_field = 0;
      p = q;
      continue;
    }
    if (p[0] == '-' && p[1] == '>') { p += 2; goto field; }
    if (*p == '.') { p++; goto field; }
    if (*p == '[') {
      int d = 0;
      for (; *p; p++) {
        if (*p == '[') d++;
        else if (*p == ']' && --d == 0) { p++; break; }
      }
      continue;
    }
    if (*p == ')') {
      /* closes a paren this scan did not open: `(*fp)(` or `(p->f)(` */
      if (depth == 0) { p++; continue; }
      depth--;
      p++;
      continue;
    }
    if (*p == '(') {
      /* a group (a cast or a wrapper) when nothing callable precedes it */
      if (!last_id) { depth++; p++; continue; }
      /* a call: is the chain over after its argument list? */
      call_open = p;
      kind_at_call = last_was_field ? 1 : 2;
      if (kind_at_call == 1) { last_field = last_id; last_field_n = last_id_n; }
      int d = 0;
      for (; *p; p++) {
        if (*p == '(') d++;
        else if (*p == ')' && --d == 0) { p++; break; }
      }
      const char *q = p;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '(' || *q == '[' || *q == '.' || (q[0] == '-' && q[1] == '>')) continue;
      break;
    }
    break;
  field:
    while (*p == ' ' || *p == '\t') p++;
    {
      const char *q = p;
      while (isalnum((unsigned char)*q) || *q == '_') q++;
      if (q == p) break;
      last_id = p;
      last_id_n = (size_t)(q - p);
      last_was_field = 1;
      p = q;
    }
    continue;
  }
  if (!call_open) return 0;
  *text = xstrndup(buf, (size_t)(call_open - buf));
  { /* the spelling: trimmed, without the parentheses and stars the column cut in half */
    char *e = *text + strlen(*text);
    for (;;) {
      while (e > *text && isspace((unsigned char)e[-1])) *--e = 0;
      int bal = 0;
      for (char *q = *text; *q; q++) bal += *q == '(' ? 1 : *q == ')' ? -1 : 0;
      if (bal < 0 && e > *text && e[-1] == ')') { *--e = 0; continue; }
      break;
    }
    char *b = *text;
    while (*b == '*' || *b == ' ') b++;
    memmove(*text, b, strlen(b) + 1);
  }
  if (kind_at_call == 1) {
    *name = xstrndup(last_field, last_field_n);
    return 1;
  }
  *name = xstrndup(last_id, last_id_n);
  return 2;
}

/* ---- roots ------------------------------------------------------------------------------------ */

struct root { int node; int role; int unit; char *fn; const struct abirow *row; };
static struct root *ROOTS; static int NROOTS, CROOTS;
static struct unit *UNITS; static int NUNITS;

static int type_matches(struct unit *u, const char *store_tag, const char *row_type) {
  if (!store_tag) return 0;
  if (!strcmp(store_tag, row_type)) return 1;
  const char *c = canon_tag(u, row_type);
  return c && !strcmp(c, store_tag);
}

static int node_for_fn(int unit, const char *fn) {
  char *t = xasprintf("%s:%s", UNITS[unit].src, fn);
  int i = map_get(&NODE_MAP, t);
  free(t);
  if (i >= 0 && N[i].defined) return i;
  i = map_get(&NODE_MAP, fn);
  return (i >= 0 && N[i].defined) ? i : -1;
}

static void add_root(int unit, const char *fn, int role, const struct abirow *row) {
  for (int i = 0; i < NROOTS; i++)
    if (ROOTS[i].unit == unit && !strcmp(ROOTS[i].fn, fn) && ROOTS[i].row == row) return;
  int node = node_for_fn(unit, fn);
  if (node < 0)
    die_nj("root %s (%s.%s, %s) in %s is in no call graph: is it defined in a unit this run compiled?",
           fn, row->type, row->field, ROLE_NAMES[role], UNITS[unit].src);
  PUSH(ROOTS, NROOTS, CROOTS, ((struct root){node, role, unit, xstrdup(fn), row}));
}

static void derive_roots(void) {
  for (int ui = 0; ui < NUNITS; ui++) {
    struct unit *u = &UNITS[ui];
    for (int s = 0; s < u->nst; s++) {
      struct store *st = &u->st[s];
      const struct abirow *best = NULL;
      for (int r = 0; r < NABI; r++) {
        const struct abirow *row = &ABI[r];
        if (!row_is_root(row) || strcmp(row->field, st->field)) continue;
        if (st->tag ? type_matches(u, st->tag, row->type) : 1) {
          /* a store whose struct is unknown (`p->f = fn;`) takes the strictest role its field has */
          if (!best || row->role < best->role) best = row;
        }
      }
      if (best) add_root(ui, st->fn, best->role, best);
    }
    /* host side: a function handed to the other side's callback (LV2 work's respond) */
    for (int r = 0; r < NABI; r++) {
      const struct abirow *row = &ABI[r];
      int s = bound_side(row);
      if (row->kind != AB_HANDED || s != 1) continue;
      for (int i = 1; i + 1 < u->nt; i++) {
        if (!(u->t[i].kind == TK_ID && is_p(&u->t[i + 1], '(') && (is_p(&u->t[i - 1], '.') || tis(&u->t[i - 1], "->"))))
          continue;
        char *f = tstr(&u->t[i]);
        int hit = !strcmp(f, row->field);
        free(f);
        if (!hit) continue;
        int pc = match(u, i + 1), a = i + 2, arg = 0;
        for (int j = a; j <= pc; j++) {
          if (j < pc && is_open(&u->t[j])) { j = match(u, j); continue; }
          if (j < pc && !is_p(&u->t[j], ',')) continue;
          if (arg == row->a) {
            char *fn = value_fn(u, a, j);
            if (fn) add_root(ui, fn, row->role, row);
          }
          arg++;
          a = j + 1;
        }
      }
    }
  }
}

/* ---- reds ------------------------------------------------------------------------------------- */

static struct map RED_SEEN;
static int REDS;
static char **PATH; static int NPATH, CPATH;

static void red(const char *code, const char *sym, const char *site, const struct root *r) {
  char *key = xasprintf("%s %s %s", code, sym, site);
  if (map_get(&RED_SEEN, key) >= 0) { free(key); return; }
  map_put(&RED_SEEN, key, 1);
  free(key);
  printf("%s %s at %s via %s[%s]", code, sym, site, r->fn, ROLE_NAMES[r->role]);
  for (int i = 1; i < NPATH; i++) printf(" -> %s", PATH[i]);
  putchar('\n');
  REDS++;
}

/* a .i token's place in its own source file: the column is found on the source line */
static char *tok_site(struct unit *u, int ti) {
  struct tok *t = &u->t[ti];
  const char *file = u->files[t->file];
  struct srcfile *f = source(file);
  int col = 1;
  if (t->line >= 1 && t->line <= f->n) {
    const char *l = f->lines[t->line - 1], *p = l;
    for (p = l; *p; p++)
      if (!strncmp(p, t->s, (size_t)t->len) &&
          (t->kind != TK_ID || ((p == l || !(isalnum((unsigned char)p[-1]) || p[-1] == '_')) &&
                                !(isalnum((unsigned char)p[t->len]) || p[t->len] == '_')))) {
        col = (int)(p - l) + 1;
        break;
      }
  }
  return xasprintf("%s:%d:%d", file, t->line, col);
}

/* ---- the worker's result crosses by value ----------------------------------------------------- */

static int type_pointer_free(struct unit *u, const char *tag, const char *tdef, int depth);
static int tag_pointer_free(struct unit *u, const char *tag, int depth) {
  struct sdef *s = sdef_of(u, tag, 0);
  if (!s || !s->known || depth > 8) return 0;
  for (int i = 0; i < s->n; i++) {
    struct member *m = &s->m[i];
    if (m->ptr || m->fnptr) return 0;
    if ((m->tag || m->tdef) && !type_pointer_free(u, m->tag, m->tdef, depth + 1)) return 0;
  }
  return 1;
}
static int type_pointer_free(struct unit *u, const char *tag, const char *tdef, int depth) {
  if (tag) return tag_pointer_free(u, tag, depth);
  if (tdef) {
    struct tdef *t = tdef_of(u, tdef);
    if (!t || t->ptr) return 0;
    if (t->tag) return tag_pointer_free(u, t->tag, depth);
    if (t->base) return type_pointer_free(u, NULL, t->base, depth + 1);
    return t->scalar;
  }
  return 1; /* a scalar spelled with keywords */
}

/* a local `T name;` / `T name = ...;` / `T name[...]` declared in [open, close): pointer-free? */
static int local_pointer_free(struct unit *u, int open, int close, const char *name) {
  for (int m = open + 1; m < close; m++) {
    if (u->t[m].kind != TK_ID || (int)strlen(name) != u->t[m].len || memcmp(u->t[m].s, name, (size_t)u->t[m].len)) continue;
    struct tok *nx = &u->t[m + 1], *pv = &u->t[m - 1];
    if (!(is_p(nx, ';') || is_p(nx, '=') || is_p(nx, '[') || is_p(nx, ','))) continue;
    if (!(pv->kind == TK_ID || is_p(pv, '*'))) continue;
    int a = decl_start(u, m);
    if (has_star(u, a, m)) return 0;
    struct tyref ty = type_of(u, a, m);
    if (!ty.tag && !ty.tdef && !ty.scalar) return 0;
    return type_pointer_free(u, ty.tag, ty.tdef, 0);
  }
  return 0;
}

static const struct root *CUR_ROOT;

static void check_results(void) {
  for (int ri = 0; ri < NROOTS; ri++) {
    struct root *r = &ROOTS[ri];
    struct unit *u = &UNITS[r->unit];
    struct fdef *f = fdef_of(u, r->fn);
    if (!f || !f->defined) continue;
    for (int k = 0; k < NABI; k++) {
      const struct abirow *row = &ABI[k];
      if (row->kind != AB_RESULT && row->kind != AB_RESULT_OUT) continue;
      if (bound_side(row) != 0) continue;
      if (strcmp(row->type, r->row->type) || strcmp(row->field, r->row->field)) continue;
      if (row->a >= f->nparams || !f->params[row->a]) continue;
      const char *p = f->params[row->a];
      size_t pn = strlen(p);
      CUR_ROOT = r;
      NPATH = 0;
      PUSH(PATH, NPATH, CPATH, r->fn);
      if (row->kind == AB_RESULT) {
        int uses = 0;
        for (int i = f->open + 1; i < f->close; i++) {
          struct tok *t = &u->t[i];
          if (t->kind != TK_ID || (size_t)t->len != pn || memcmp(t->s, p, pn)) continue;
          if (is_p(&u->t[i - 1], '.') || tis(&u->t[i - 1], "->")) continue;
          /* `(void)body;` says it is not used */
          if (i >= 3 && is_p(&u->t[i - 1], ')') && tis(&u->t[i - 2], "void") && is_p(&u->t[i - 3], '(') && is_p(&u->t[i + 1], ';'))
            continue;
          uses++;
          int ok = 0;
          if (uses == 1) {
            /* memcpy(&local, body, n): the body is the second argument and the destination a
             * pointer-free local */
            if (is_p(&u->t[i - 1], ',') && is_p(&u->t[i + 1], ',')) {
              int po = -1, d = 0;
              for (int j = i - 2; j > f->open; j--) {
                if (is_p(&u->t[j], ')')) d++;
                else if (is_p(&u->t[j], '(')) {
                  if (d == 0) { po = j; break; }
                  d--;
                } else if (d == 0 && is_p(&u->t[j], ',')) { po = -2; break; }
              }
              if (po > 0 && (tis(&u->t[po - 1], "memcpy") || tis(&u->t[po - 1], "__builtin_memcpy"))) {
                int a = po + 1;
                if (is_p(&u->t[a], '&')) a++;
                if (a == i - 2 && u->t[a].kind == TK_ID) {
                  char *dst = tstr(&u->t[a]);
                  ok = local_pointer_free(u, f->open, f->close, dst);
                  free(dst);
                }
              }
            }
            /* *(const T *)body: a copy of a pointer-free T */
            if (!ok && is_p(&u->t[i - 1], ')')) {
              int po = rmatch(u, i - 1);
              if (po > 0 && is_p(&u->t[po - 1], '*')) {
                int stars = 0;
                for (int k2 = po + 1; k2 < i - 1; k2++)
                  if (is_p(&u->t[k2], '*')) stars++;
                struct tyref ty = type_of(u, po + 1, i - 1);
                if (stars == 1 && (ty.tag || ty.tdef || ty.scalar)) ok = type_pointer_free(u, ty.tag, ty.tdef, 0);
              }
            }
          }
          if (!ok) {
            char *site = tok_site(u, i);
            red("RT-RESULT", p, site, r);
            free(site);
          }
        }
      } else {
        /* respond(handle, size, &local): the result is the address of a pointer-free local */
        for (int i = f->open + 1; i + 1 < f->close; i++) {
          struct tok *t = &u->t[i];
          if (t->kind != TK_ID || (size_t)t->len != pn || memcmp(t->s, p, pn) || !is_p(&u->t[i + 1], '(')) continue;
          if (is_p(&u->t[i - 1], '.') || tis(&u->t[i - 1], "->")) continue;
          int pc = match(u, i + 1), a = i + 2, arg = 0, ok = 0, at = i;
          for (int j = a; j <= pc; j++) {
            if (j < pc && is_open(&u->t[j])) { j = match(u, j); continue; }
            if (j < pc && !is_p(&u->t[j], ',')) continue;
            if (arg == row->b) {
              at = a;
              if (j - a == 2 && is_p(&u->t[a], '&') && u->t[a + 1].kind == TK_ID) {
                char *loc = tstr(&u->t[a + 1]);
                ok = local_pointer_free(u, f->open, f->close, loc);
                free(loc);
              }
            }
            arg++;
            a = j + 1;
          }
          if (!ok) {
            char *site = tok_site(u, at);
            char *sym = xstrndup(u->t[at].s, (size_t)u->t[at].len);
            if (is_p(&u->t[at], '&') && at + 1 < pc) {
              free(sym);
              sym = xasprintf("&%.*s", u->t[at + 1].len, u->t[at + 1].s);
            }
            red("RT-RESULT", sym, site, r);
            free(site);
            free(sym);
          }
        }
      }
    }
  }
}

/* ---- the walk --------------------------------------------------------------------------------- */

static unsigned char *VISITED;
static int REACHED;
static unsigned char *EVER;

/* the strictest role a bound ABI declares callable through this field (-1: none) */
static int callable_role(const char *field) {
  int role = -1;
  for (int r = 0; r < NABI; r++)
    if (row_is_callable(&ABI[r]) && !strcmp(ABI[r].field, field) && ABI[r].role > role) role = ABI[r].role;
  return role;
}

static void visit(int n);

static void call_to(int tgt_raw, const char *site) {
  const struct root *r = CUR_ROOT;
  int tgt = resolve(tgt_raw);
  const char *name = N[tgt].name;
  int fi = map_get(&FORB_MAP, name);
  if (fi >= 0) {
    red(FORB[fi].code, name, site, r);
    return;
  }
  if (N[tgt].defined) {
    int tag = map_get(&TAGS, name);
    if (r->role == ROLE_AUDIO && (tag == ROLE_WORKER || tag == ROLE_CONTROL)) {
      red("RT-THREAD", name, site, r);
      return;
    }
    visit(tgt);
    return;
  }
  if (map_get(&CLS_MAP, name) >= 0) return;
  red("RT-EXTERNAL", name, site, r);
}

static void indirect(int from, const char *site) {
  const struct root *r = CUR_ROOT;
  char *name, *text;
  int kind = callee_at(site, &name, &text);
  if (kind == 0) {
    red("RT-INDIRECT", "<unread>", site, r);
    return;
  }
  if (kind == 2) {
    /* a function handed to this root as a parameter, with a declared role */
    if (from == r->node) {
      struct fdef *f = fdef_of(&UNITS[r->unit], r->fn);
      for (int k = 0; f && k < NABI; k++) {
        const struct abirow *row = &ABI[k];
        if (row->kind != AB_HANDED || bound_side(row) != 0) continue;
        if (strcmp(row->type, r->row->type) || strcmp(row->field, r->row->field)) continue;
        if (row->a < f->nparams && f->params[row->a] && !strcmp(f->params[row->a], name)) {
          if (row->role == ROLE_WORKER && r->role == ROLE_AUDIO) red("RT-ROLE", text, site, r);
          return;
        }
      }
    }
    red("RT-INDIRECT", text, site, r);
    return;
  }
  int role = callable_role(name);
  if (role >= 0) {
    if (role == ROLE_WORKER && r->role == ROLE_AUDIO) red("RT-ROLE", text, site, r);
    return;
  }
  /* a vtable: the functions an initializer or a store puts into this field, in any unit of the run */
  int targets = 0;
  for (int ui = 0; ui < NUNITS; ui++)
    for (int s = 0; s < UNITS[ui].nst; s++) {
      struct store *st = &UNITS[ui].st[s];
      if (strcmp(st->field, name)) continue;
      int n = node_for_fn(ui, st->fn);
      if (n < 0) continue;
      targets++;
      call_to(n, site);
    }
  if (!targets) red("RT-INDIRECT", text, site, r);
}

static void visit(int n) {
  if (VISITED[n]) return;
  VISITED[n] = 1;
  if (!EVER[n]) { EVER[n] = 1; REACHED++; }
  PUSH(PATH, NPATH, CPATH, N[n].name);
  for (int i = 0; i < N[n].ne; i++) {
    struct edge *e = &N[n].e[i];
    if (e->to == INDIRECT) indirect(n, e->site);
    else call_to(e->to, e->site);
  }
  NPATH--;
}

static void walk(void) {
  VISITED = calloc((size_t)NN + 1, 1);
  EVER = calloc((size_t)NN + 1, 1);
  if (!VISITED || !EVER) die_nj("out of memory");
  for (int i = 0; i < NROOTS; i++) {
    CUR_ROOT = &ROOTS[i];
    memset(VISITED, 0, (size_t)NN);
    NPATH = 0;
    /* the root itself carries its tag: a worker or control function is no audio root */
    int tag = map_get(&TAGS, ROOTS[i].fn);
    const char *loc = N[ROOTS[i].node].loc ? N[ROOTS[i].node].loc : "?";
    if (ROOTS[i].role == ROLE_AUDIO && (tag == ROLE_WORKER || tag == ROLE_CONTROL)) {
      PUSH(PATH, NPATH, CPATH, ROOTS[i].fn);
      red("RT-THREAD", ROOTS[i].fn, loc, &ROOTS[i]);
      NPATH = 0;
      continue;
    }
    visit(ROOTS[i].node);
  }
}

/* ---- compiling the units ---------------------------------------------------------------------- */

static int run(char **argv, const char *log) {
  pid_t pid = fork();
  if (pid < 0) die_nj("fork: %s", strerror(errno));
  if (pid == 0) {
    if (log) {
      FILE *f = freopen(log, "w", stderr);
      (void)f;
    }
    execvp(argv[0], argv);
    fprintf(stderr, "omx-rt-gate: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0)
    if (errno != EINTR) die_nj("waitpid: %s", strerror(errno));
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static void show_log(const char *log) {
  char *t = slurp(log, NULL);
  if (t) fputs(t, stderr);
}

/* the consumer's flags, without what would change the input: optimisation, output, LTO, the
 * dependency files; then -O0 -fno-inline -fcallgraph-info */
static char **cc_argv(char **cc, int ncc, const char *unit, const char *out, int preprocess) {
  char **a = xrealloc(NULL, (size_t)(ncc + 16) * sizeof *a);
  int n = 0;
  for (int i = 0; i < ncc; i++) {
    const char *f = cc[i];
    if (i > 0) {
      if (!strncmp(f, "-O", 2) || !strcmp(f, "-c") || !strcmp(f, "-S") || !strcmp(f, "-E") ||
          !strncmp(f, "-flto", 5) || !strcmp(f, "-MD") || !strcmp(f, "-MMD") || !strcmp(f, "-MP") ||
          !strncmp(f, "-finline", 8) || !strncmp(f, "-fcallgraph-info", 16) || !strncmp(f, "-fdump", 6) ||
          !strcmp(f, "-pipe") || !strncmp(f, "-save-temps", 11))
        continue;
      if (!strcmp(f, "-o") || !strcmp(f, "-MF") || !strcmp(f, "-MT") || !strcmp(f, "-MQ")) {
        i++;
        continue;
      }
    }
    a[n++] = (char *)f;
  }
  a[n++] = "-O0";
  a[n++] = "-fno-inline";
  /* a distribution's -D_FORTIFY_SOURCE warns at -O0, and a -Werror build stops on it */
  a[n++] = "-Wp,-U_FORTIFY_SOURCE";
  /* the stack protector's __stack_chk_fail is the compiler's own guard, not a call the source makes:
   * the graph is read without it */
  a[n++] = "-fno-stack-protector";
  if (preprocess) {
    a[n++] = "-E";
  } else {
    a[n++] = "-fcallgraph-info";
    a[n++] = "-c";
  }
  a[n++] = (char *)unit;
  a[n++] = "-o";
  a[n++] = (char *)out;
  a[n] = NULL;
  return a;
}

static void compile_units(char **cc, int ncc, char **units, int nunits, const char *work) {
  UNITS = calloc((size_t)nunits, sizeof *UNITS);
  if (!UNITS) die_nj("out of memory");
  for (int i = 0; i < nunits; i++) {
    struct unit *u = &UNITS[NUNITS++];
    u->src = xstrdup(units[i]);
    char *obj = xasprintf("%s/u%d.o", work, i);
    u->ipath = xasprintf("%s/u%d.i", work, i);
    u->cipath = xasprintf("%s/u%d.ci", work, i);
    char *log = xasprintf("%s/u%d.log", work, i);
    if (run(cc_argv(cc, ncc, units[i], obj, 0), log) != 0) {
      show_log(log);
      die_nj("%s does not compile with the consumer's flags at -O0 -fno-inline -fcallgraph-info", units[i]);
    }
    if (run(cc_argv(cc, ncc, units[i], u->ipath, 1), log) != 0) {
      show_log(log);
      die_nj("%s does not preprocess with the consumer's flags", units[i]);
    }
    if (load_graph(u->cipath, i) < 0) die_nj("%s: the compiler wrote no call graph %s (gcc 10 or later has -fcallgraph-info)", units[i], u->cipath);
    parse_unit(u);
  }
}

static void load_graph_dir(const char *dir) {
  struct stat sb;
  if (stat(dir, &sb) != 0) die_nj("graphs %s: %s", dir, strerror(errno));
  if (!S_ISDIR(sb.st_mode)) {
    if (load_graph(dir, -1) < 0) die_nj("graph %s cannot be read", dir);
    return;
  }
  DIR *d = opendir(dir);
  if (!d) die_nj("graphs %s: %s", dir, strerror(errno));
  struct dirent *e;
  int n = 0;
  char **names = NULL;
  int cn = 0, nn = 0;
  while ((e = readdir(d))) {
    size_t l = strlen(e->d_name);
    if (l > 3 && !strcmp(e->d_name + l - 3, ".ci")) PUSH(names, nn, cn, xasprintf("%s/%s", dir, e->d_name));
  }
  closedir(d);
  for (int i = 0; i < nn; i++)
    if (load_graph(names[i], -1) >= 0) n++;
  if (n == 0) die_nj("graphs %s holds no .ci graph", dir);
}

/* ---- the positive control --------------------------------------------------------------------- */

static const char PROBE_SRC[] =
    "#include <stdlib.h>\n"
    "struct omx_rt_gate_probe { void (*run)(void); };\n"
    "static void probe_leaf(void) { void *p = malloc(1); free(p); }\n"
    "static void probe_run(void) { probe_leaf(); }\n"
    "const struct omx_rt_gate_probe omx_rt_gate_probe_entry = { probe_run };\n";

/* the same compiler, tables and walk over a planted malloc: it must read
 * `RT-ALLOC malloc at probe.c:3:41 via probe_run[audio] -> probe_leaf`. A gate that cannot see it
 * judges nothing. Run in a child, so the probe's graph never joins the consumer's. */
static void positive_control(const char *cc0, const char *work) {
  char *dir = xasprintf("%s/probe", work);
  if (mkdir(dir, 0700) != 0 && errno != EEXIST) die_nj("%s: %s", dir, strerror(errno));
  char *src = xasprintf("%s/probe.c", dir);
  FILE *f = fopen(src, "w");
  if (!f) die_nj("%s: %s", src, strerror(errno));
  fputs(PROBE_SRC, f);
  fclose(f);
  char *out = xasprintf("%s/probe.out", dir);
  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) die_nj("fork: %s", strerror(errno));
  if (pid == 0) {
    if (!freopen(out, "w", stdout)) _exit(3);
    static struct abirow probe_row = {AB_PLUGIN, "omx-rt-gate-probe", "omx_rt_gate_probe", "run", ROLE_AUDIO, -1, -1, "probe"};
    ABI = NULL;
    NABI = CABI = 0;
    PUSH(ABI, NABI, CABI, probe_row);
    BOUND = NULL;
    NBOUND = CBOUND = 0;
    PUSH(BOUND, NBOUND, CBOUND, ((struct bound){"omx-rt-gate-probe", 0}));
    char *cc[] = {(char *)cc0};
    char *units[] = {src};
    compile_units(cc, 1, units, 1, dir);
    derive_roots();
    walk();
    fflush(stdout);
    _exit(REDS ? 1 : 0);
  }
  int st;
  while (waitpid(pid, &st, 0) < 0)
    if (errno != EINTR) die_nj("waitpid: %s", strerror(errno));
  char *got = slurp(out, NULL);
  char *want = xasprintf("RT-ALLOC malloc at %s:3:", src);
  int ok = WIFEXITED(st) && WEXITSTATUS(st) == 1 && got && strstr(got, want) &&
           strstr(got, "via probe_run[audio] -> probe_leaf");
  if (!ok) {
    if (got) fputs(got, stderr);
    die_nj("positive control: the planted malloc behind a probe root was not read red (%s): this gate is blind and judges nothing", want);
  }
  fprintf(stderr, "omx-rt-gate: positive control: the planted malloc reads red\n");
}


/* ---- the tag check: every function of the shipped headers carries exactly one `@thread` --------- */

/* the comment block right above source line `line` (blank and preprocessor lines skipped): its
 * first and last line, or 0 */
static int block_above(struct srcfile *f, int line, int *first, int *last) {
  int l = line - 1;
  while (l >= 1) {
    const char *s = f->lines[l - 1];
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#') { l--; continue; }
    break;
  }
  if (l < 1) return 0;
  const char *s = f->lines[l - 1];
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) n--;
  if (n < 2 || s[n - 2] != '*' || s[n - 1] != '/') return 0;
  *last = l;
  for (int k = l; k >= 1; k--)
    if (strstr(f->lines[k - 1], "/*")) {
      *first = k;
      return 1;
    }
  return 0;
}

static int thread_tags(char **cc, int ncc, const char *unit, const char *under, const char *except, const char *out, const char *selfrun, const char *work) {
  struct unit u = {0};
  u.src = xstrdup(unit);
  u.ipath = xasprintf("%s/tags.i", work);
  char *log = xasprintf("%s/tags.log", work);
  if (run(cc_argv(cc, ncc, unit, u.ipath, 1), log) != 0) {
    show_log(log);
    die_nj("%s does not preprocess", unit);
  }
  parse_unit(&u);
  FILE *o = fopen(out, "w");
  if (!o) die_nj("%s: %s", out, strerror(errno));
  fprintf(o, "# the @thread tag of every function of the shipped headers (omx-rt-gate --thread-tags)\n");
  int bad = 0, n = 0;
  int *roles = calloc((size_t)u.nfd + 1, sizeof *roles);
  for (int i = 0; i < u.nfd; i++) {
    struct fdef *fd = &u.fd[i];
    roles[i] = ROLE_NONE;
    const char *file = u.files[u.t[fd->tok].file];
    if (strncmp(file, under, strlen(under))) continue;
    /* the header omx-contract renders from data/ carries no tag: its items are documented there */
    if (except && !strcmp(file, except)) continue;
    int start = decl_start(&u, fd->tok);
    int line = u.t[start].file == u.t[fd->tok].file ? u.t[start].line : fd->line;
    struct srcfile *sf = source(file);
    int first, last, count = 0, role = ROLE_NONE, invalid = 0;
    if (block_above(sf, line, &first, &last))
      for (int l = first; l <= last; l++)
        for (const char *p = sf->lines[l - 1]; (p = strstr(p, "@thread")); p += 7) {
          const char *q = p + 7;
          while (*q == ' ' || *q == '\t') q++;
          char w[16] = {0};
          for (int k = 0; k < 15 && isalpha((unsigned char)q[k]); k++) w[k] = q[k];
          int r = role_of(w);
          if (r == ROLE_NONE) invalid = 1;
          else role = r;
          count++;
        }
    n++;
    if (count == 0) { printf("TAG-MISSING %s at %s:%d: no `@thread audio|worker|control` in the comment above it\n", fd->name, file, line); bad++; }
    else if (count > 1) { printf("TAG-TWICE %s at %s:%d: %d `@thread` tags, one is the rule\n", fd->name, file, line, count); bad++; }
    else if (invalid) { printf("TAG-INVALID %s at %s:%d: `@thread` takes audio, worker or control\n", fd->name, file, line); bad++; }
    else {
      fprintf(o, "%s %s %s:%d\n", fd->name, ROLE_NAMES[role], file, line);
      roles[i] = role;
    }
  }
  fclose(o);
  if (selfrun) {
    /* the self-run unit: the address of every tagged function, in the entry of its tag */
    FILE *g = fopen(selfrun, "w");
    if (!g) die_nj("%s: %s", selfrun, strerror(errno));
    char *abs = realpath(unit, NULL);
    fprintf(g, "/* generated by omx-rt-gate --thread-tags: the address of every tagged function of %s */\n", under);
    fprintf(g, "#include \"%s\"\n", abs ? abs : unit);
    for (int r = 0; r < 3; r++) {
      fprintf(g, "struct omx_rt_gate_%s_entry { void (*fn)(void); };\n", ROLE_NAMES[r]);
      fprintf(g, "const struct omx_rt_gate_%s_entry omx_rt_gate_%s[] = {\n", ROLE_NAMES[r], ROLE_NAMES[r]);
      for (int i = 0; i < u.nfd; i++)
        if (roles[i] == r) fprintf(g, "  {(void (*)(void))%s},\n", u.fd[i].name);
      fprintf(g, "  {0}\n};\n");
    }
    fclose(g);
  }
  fprintf(stderr, "omx-rt-gate: %d functions under %s, %d without exactly one tag\n", n, under, bad);
  if (n == 0) die_nj("no function under %s in %s", under, unit);
  return bad ? 1 : 0;
}

/* ---- the shipped graphs: each compiled unit's call graph, read the way the gate reads a consumer's ------- */

static int write_graphs(char **cc, int ncc, char **units, int nunits, const char *dir) {
  mkdir(dir, 0755);
  for (int i = 0; i < nunits; i++) {
    const char *slash = strrchr(units[i], '/');
    char *base = xstrdup(slash ? slash + 1 : units[i]);
    size_t l = strlen(base);
    if (l > 2 && !strcmp(base + l - 2, ".c")) base[l - 2] = 0;
    char *obj = xasprintf("%s/%s.o", dir, base), *ci = xasprintf("%s/%s.ci", dir, base), *log = xasprintf("%s/%s.log", dir, base);
    unlink(ci);
    if (run(cc_argv(cc, ncc, units[i], obj, 0), log) != 0) {
      show_log(log);
      die_nj("%s does not compile at -O0 -fno-inline -fcallgraph-info", units[i]);
    }
    struct stat sb;
    if (stat(ci, &sb) != 0) die_nj("%s: the compiler wrote no call graph %s", units[i], ci);
    unlink(obj);
    unlink(log);
  }
  fprintf(stderr, "omx-rt-gate: %d call graphs under %s\n", nunits, dir);
  return 0;
}

/* ---- main ------------------------------------------------------------------------------------- */

static void usage(void) {
  fputs("usage: omx-rt-gate [--tables DIR] --abi NAME[:host]... [--tags FILE]... [--graphs DIR|FILE]...\n"
        "                   [--work DIR] [--list-roots] -- CC [FLAGS...] UNIT.c...\n"
        "       omx-rt-gate [--tables DIR] [--tags FILE]... --check-tables\n"
        "       omx-rt-gate --thread-tags OUT [--selfrun UNIT.c] [--under DIR] [--except FILE] [--work DIR] -- CC [FLAGS...] HEADERS.c\n"
        "       omx-rt-gate --write-graphs DIR -- CC [FLAGS...] UNIT.c...\n", stderr);
  exit(2);
}

/* the installed data: <prefix>/share/omxdsp/rt-gate beside <prefix>/bin/omx-rt-gate, else the
 * directory this build was configured with */
static const char *datadir(void) {
  static char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 64);
  if (n > 0) {
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    if (slash) {
      strcpy(slash, "/../share/omxdsp/rt-gate");
      struct stat sb;
      if (stat(buf, &sb) == 0 && S_ISDIR(sb.st_mode)) return buf;
    }
  }
  return OMX_RT_GATE_DATADIR;
}

int main(int argc, char **argv) {
  const char *data = datadir();
  const char *tables = xasprintf("%s/tables", data);
  char **tags = NULL, **graphs = NULL;
  int ntags = 0, ctags = 0, ngraphs = 0, cgraphs = 0, check_only = 0, list_roots = 0, defaults = 1;
  const char *work = NULL, *tags_out = NULL, *selfrun_out = NULL, *under = "include/", *except = NULL, *graphs_out = NULL;
  int i = 1;
  setvbuf(stdout, NULL, _IOLBF, 0);
  for (; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--tables") && i + 1 < argc) { tables = argv[++i]; defaults = 0; }
    else if (!strcmp(a, "--abi") && i + 1 < argc) {
      char *v = xstrdup(argv[++i]), *c = strchr(v, ':');
      int host = 0;
      if (c) {
        if (strcmp(c, ":host")) usage();
        *c = 0;
        host = 1;
      }
      PUSH(BOUND, NBOUND, CBOUND, ((struct bound){v, host}));
    } else if (!strcmp(a, "--tags") && i + 1 < argc) PUSH(tags, ntags, ctags, argv[++i]);
    else if (!strcmp(a, "--graphs") && i + 1 < argc) PUSH(graphs, ngraphs, cgraphs, argv[++i]);
    else if (!strcmp(a, "--work") && i + 1 < argc) work = argv[++i];
    else if (!strcmp(a, "--check-tables")) check_only = 1;
    else if (!strcmp(a, "--list-roots")) list_roots = 1;
    else if (!strcmp(a, "--thread-tags") && i + 1 < argc) tags_out = argv[++i];
    else if (!strcmp(a, "--selfrun") && i + 1 < argc) selfrun_out = argv[++i];
    else if (!strcmp(a, "--under") && i + 1 < argc) under = argv[++i];
    else if (!strcmp(a, "--except") && i + 1 < argc) except = argv[++i];
    else if (!strcmp(a, "--write-graphs") && i + 1 < argc) graphs_out = argv[++i];
    else usage();
  }
  if (graphs_out) {
    char **cc = NULL, **units = NULL;
    int nc = 0, cc_cap = 0, nu = 0, cu = 0;
    for (int k = i; k < argc; k++) {
      size_t l = strlen(argv[k]);
      if (k > i && l > 2 && !strcmp(argv[k] + l - 2, ".c") && argv[k][0] != '-') PUSH(units, nu, cu, argv[k]);
      else PUSH(cc, nc, cc_cap, argv[k]);
    }
    if (nc < 1 || nu == 0) usage();
    return write_graphs(cc, nc, units, nu, graphs_out);
  }
  if (tags_out) {
    int ncc = argc - i;
    if (ncc < 2) usage();
    char tmpl2[] = "/tmp/omx-rt-gate.XXXXXX";
    if (!work) work = mkdtemp(tmpl2);
    else mkdir(work, 0700);
    if (!work) die_nj("mkdtemp: %s", strerror(errno));
    return thread_tags(argv + i, ncc - 1, argv[argc - 1], under, except, tags_out, selfrun_out, work);
  }
  /* one forbidden list and one class table: a tables directory with any other list is refused */
  {
    DIR *d = opendir(tables);
    if (!d) die_nj("tables %s: %s", tables, strerror(errno));
    struct dirent *e;
    while ((e = readdir(d))) {
      size_t l = strlen(e->d_name);
      if (l > 4 && !strcmp(e->d_name + l - 4, ".tbl") && strcmp(e->d_name, "forbidden.tbl") && strcmp(e->d_name, "classes.tbl"))
        refuse(tables, "%s: one forbidden list and one class table, no other list", e->d_name);
    }
    closedir(d);
  }
  char *fp = xasprintf("%s/forbidden.tbl", tables), *cp = xasprintf("%s/classes.tbl", tables);
  load_forbidden(fp);
  load_classes(cp);
  if (NFORB == 0) refuse(fp, "the forbidden list is empty: nothing would be red");
  if (check_only) {
    /* every roles file of the tables directory */
    DIR *d = opendir(tables);
    struct dirent *e;
    while (d && (e = readdir(d))) {
      size_t l = strlen(e->d_name);
      if (l > 4 && !strcmp(e->d_name + l - 4, ".abi")) {
        char *ap = xasprintf("%s/%s", tables, e->d_name);
        load_abi(ap, xstrndup(e->d_name, l - 4));
      }
    }
    if (d) closedir(d);
  }
  for (int b = 0; b < NBOUND; b++) {
    char *ap = xasprintf("%s/%s.abi", tables, BOUND[b].abi);
    if (access(ap, R_OK) != 0) die_nj("--abi %s: no roles file %s", BOUND[b].abi, ap);
    load_abi(ap, BOUND[b].abi);
  }
  if (defaults && ntags == 0) {
    char *t = xasprintf("%s/omx-dsp.tags", data);
    if (access(t, R_OK) == 0) PUSH(tags, ntags, ctags, t);
  }
  if (defaults && ngraphs == 0) {
    char *g = xasprintf("%s/graphs", data);
    if (access(g, R_OK) == 0) PUSH(graphs, ngraphs, cgraphs, g);
  }
  for (int t = 0; t < ntags; t++) load_tags(tags[t]);
  /* a function of ours is never classified: it is walked */
  for (int c = 0; c < NCLS; c++)
    if (map_get(&TAGS, CLS[c].sym) >= 0) refuse(CLS[c].where, "`%s` is an omx-dsp function: it is walked, never classified", CLS[c].sym);
  if (refusals) die_nj("%d table row%s refused at load", refusals, refusals == 1 ? "" : "s");
  fprintf(stderr, "omx-rt-gate: %d forbidden, %d classified, %d ABI roles, %d tags\n", NFORB, NCLS, NABI, NTAGS);
  if (check_only) return 0;
  if (NBOUND == 0) die_nj("no --abi: a consumer names the ABIs it binds");
  int ncc = argc - i;
  if (ncc < 2) usage();
  char **cc = NULL, **units = NULL;
  int nc = 0, cc_cap = 0, nu = 0, cu = 0;
  for (int k = i; k < argc; k++) {
    size_t l = strlen(argv[k]);
    if (k > i && l > 2 && !strcmp(argv[k] + l - 2, ".c") && argv[k][0] != '-') PUSH(units, nu, cu, argv[k]);
    else PUSH(cc, nc, cc_cap, argv[k]);
  }
  if (nu == 0) die_nj("no translation unit (*.c) after --");
  char tmpl[] = "/tmp/omx-rt-gate.XXXXXX";
  if (!work) {
    const char *td = getenv("TMPDIR");
    char *t = td && *td ? xasprintf("%s/omx-rt-gate.XXXXXX", td) : tmpl;
    work = mkdtemp(t);
    if (!work) die_nj("mkdtemp: %s", strerror(errno));
  } else if (mkdir(work, 0700) != 0 && errno != EEXIST) {
    die_nj("%s: %s", work, strerror(errno));
  }

  positive_control(cc[0], work);

  for (int g = 0; g < ngraphs; g++) load_graph_dir(graphs[g]);
  compile_units(cc, nc, units, nu, work);
  /* a classified symbol the run defines is a function of ours */
  for (int c = 0; c < NCLS; c++) {
    int n = map_get(&NODE_MAP, CLS[c].sym);
    if (n >= 0 && N[n].defined) refuse(CLS[c].where, "`%s` is defined in this run's graphs: it is walked, never classified", CLS[c].sym);
  }
  if (refusals) die_nj("%d table row%s refused at load", refusals, refusals == 1 ? "" : "s");
  derive_roots();
  if (NROOTS == 0) die_nj("no root: no function of the units is stored in a field the bound ABIs declare");
  int na = 0, nw = 0;
  for (int r = 0; r < NROOTS; r++) {
    if (ROOTS[r].role == ROLE_AUDIO) na++;
    else nw++;
    if (list_roots)
      printf("ROOT %s[%s] %s.%s %s\n", ROOTS[r].fn, ROLE_NAMES[ROOTS[r].role], ROOTS[r].row->type, ROOTS[r].row->field,
             N[ROOTS[r].node].loc ? N[ROOTS[r].node].loc : "?");
  }
  check_results();
  walk();
  fprintf(stderr, "omx-rt-gate: %d roots (%d audio, %d worker), %d functions reached, %d red\n", NROOTS, na, nw, REACHED, REDS);
  printf("SUMMARY roots=%d audio=%d worker=%d reached=%d red=%d\n", NROOTS, na, nw, REACHED, REDS);
  return REDS ? 1 : 0;
}
