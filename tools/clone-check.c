// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * clone-check.c — the regions two files of the library share, and the functions two files define.
 *
 *   clone-check FILE...   prints, sorted, one line per file pair that shares a region
 *                           clone <file a> <file b> <regions> <tokens>
 *                         and one line per function name defined in more than one file
 *                           def <name> <files>
 *
 * Each file is read as tokens: comments and preprocessor lines go, every string and number literal is
 * one token, names stay as written. A region is a maximal run of at least MIN_TOKENS tokens that two
 * files share. A run that is half literals or more is a table, not code, and is not counted.
 * tools/clone-check.sh holds this output to tools/clone-baseline.txt.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_TOKENS 30u

typedef struct {
  const char *path;
  uint32_t *tok; /* each token's id in the interned table */
  uint32_t n;
} File;

static char **g_names;
static uint32_t g_nnames, g_cap;
static uint32_t g_lit; /* the id of the literal token */

#define INTERN_SLOTS (1u << 20)
static uint32_t g_slot[INTERN_SLOTS];

static uint32_t fnv(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

static void *must(void *p) {
  if (!p) {
    fprintf(stderr, "clone-check: out of memory\n");
    exit(2);
  }
  return p;
}

static uint32_t intern(const char *s, size_t n) {
  uint32_t h = fnv(s, n) & (INTERN_SLOTS - 1u);
  for (;;) {
    const uint32_t id = g_slot[h];
    if (id == 0u) break;
    if (strlen(g_names[id - 1u]) == n && memcmp(g_names[id - 1u], s, n) == 0) return id - 1u;
    h = (h + 1u) & (INTERN_SLOTS - 1u);
  }
  if (g_nnames == g_cap) {
    g_cap = g_cap ? g_cap * 2u : 4096u;
    g_names = must(realloc(g_names, g_cap * sizeof *g_names));
  }
  char *c = must(malloc(n + 1u));
  memcpy(c, s, n);
  c[n] = 0;
  g_names[g_nnames] = c;
  g_slot[h] = ++g_nnames;
  return g_nnames - 1u;
}

static int is_id(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static void tokenize(File *f) {
  FILE *fp = fopen(f->path, "rb");
  if (!fp) {
    fprintf(stderr, "clone-check: cannot read %s\n", f->path);
    exit(2);
  }
  fseek(fp, 0, SEEK_END);
  const long len = ftell(fp);
  rewind(fp);
  char *s = must(malloc((size_t)len + 2u));
  if (fread(s, 1, (size_t)len, fp) != (size_t)len) exit(2);
  s[len] = 0;
  fclose(fp);
  uint32_t cap = 1024u;
  f->tok = must(malloc(cap * sizeof *f->tok));
  f->n = 0u;
  int line_start = 1;
  for (long i = 0; i < len;) {
    const char c = s[i];
    if (c == '\n') { line_start = 1; i++; continue; }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f') { i++; continue; }
    if (c == '/' && s[i + 1] == '/') { while (i < len && s[i] != '\n') i++; continue; }
    if (c == '/' && s[i + 1] == '*') {
      i += 2;
      while (i < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
      i += 2;
      continue;
    }
    if (c == '#' && line_start) {
      while (i < len && s[i] != '\n') { if (s[i] == '\\' && s[i + 1] == '\n') i++; i++; }
      continue;
    }
    line_start = 0;
    uint32_t id;
    if (c == '"' || c == '\'') {
      long j = i + 1;
      while (j < len && s[j] != c) { if (s[j] == '\\') j++; j++; }
      i = j + 1;
      id = g_lit;
    } else if (c >= '0' && c <= '9') {
      long j = i;
      while (j < len && (is_id(s[j]) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) j++;
      i = j;
      id = g_lit;
    } else if (is_id(c)) {
      long j = i;
      while (j < len && is_id(s[j])) j++;
      id = intern(s + i, (size_t)(j - i));
      i = j;
    } else {
      id = intern(s + i, 1u);
      i++;
    }
    if (f->n == cap) {
      cap *= 2u;
      f->tok = must(realloc(f->tok, cap * sizeof *f->tok));
    }
    f->tok[f->n++] = id;
  }
  free(s);
}

typedef struct {
  uint64_t h;
  uint32_t file, pos;
} Win;

static int cmp_win(const void *a, const void *b) {
  const Win *x = a, *y = b;
  if (x->h != y->h) return x->h < y->h ? -1 : 1;
  if (x->file != y->file) return x->file < y->file ? -1 : 1;
  return x->pos < y->pos ? -1 : x->pos > y->pos;
}

typedef struct {
  uint64_t regions, tokens;
} Pair;

static const char *g_last_name;

/* The functions a file defines: at depth 0, a name, its balanced parentheses, then a brace. */
typedef struct {
  uint32_t name, file;
} Def;

static int cmp_def(const void *a, const void *b) {
  const Def *x = a, *y = b;
  if (x->name != y->name) return strcmp(g_names[x->name], g_names[y->name]);
  return x->file < y->file ? -1 : x->file > y->file;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
  const uint32_t nf = (uint32_t)argc - 1u;
  if (nf == 0u) {
    fprintf(stderr, "usage: clone-check FILE...\n");
    return 2;
  }
  g_lit = intern("<literal>", 9u);
  const uint32_t lb = intern("{", 1u), rb = intern("}", 1u), lp = intern("(", 1u), rp = intern(")", 1u);
  File *files = must(calloc(nf, sizeof *files));
  size_t nwin = 0u;
  for (uint32_t i = 0; i < nf; i++) {
    files[i].path = argv[i + 1u];
    tokenize(&files[i]);
    if (files[i].n >= MIN_TOKENS) nwin += files[i].n - MIN_TOKENS + 1u;
  }
  Win *w = must(malloc((nwin + 1u) * sizeof *w));
  size_t k = 0u;
  for (uint32_t i = 0; i < nf; i++)
    for (uint32_t p = 0; p + MIN_TOKENS <= files[i].n; p++) {
      uint64_t h = 1469598103934665603ULL;
      for (uint32_t t = 0; t < MIN_TOKENS; t++) h = (h ^ files[i].tok[p + t]) * 1099511628211ULL;
      w[k++] = (Win){h, i, p};
    }
  qsort(w, nwin, sizeof *w, cmp_win);
  Pair *pairs = must(calloc((size_t)nf * nf, sizeof *pairs));
  for (size_t a = 0; a < nwin;) {
    size_t b = a + 1u;
    while (b < nwin && w[b].h == w[a].h) b++;
    for (size_t x = a; x < b; x++)
      for (size_t y = x + 1u; y < b; y++) {
        if (w[x].file == w[y].file) continue;
        const File *fa = &files[w[x].file], *fb = &files[w[y].file];
        const uint32_t pa = w[x].pos, pb = w[y].pos;
        if (memcmp(fa->tok + pa, fb->tok + pb, MIN_TOKENS * sizeof(uint32_t)) != 0) continue;
        if (pa > 0u && pb > 0u && fa->tok[pa - 1u] == fb->tok[pb - 1u]) continue; /* not the region's start */
        uint32_t len = MIN_TOKENS, lit = 0u;
        while (pa + len < fa->n && pb + len < fb->n && fa->tok[pa + len] == fb->tok[pb + len]) len++;
        for (uint32_t t = 0; t < len; t++) lit += fa->tok[pa + t] == g_lit;
        if (2u * lit >= len) continue; /* a table */
        Pair *pr = &pairs[(size_t)w[x].file * nf + w[y].file];
        pr->regions++;
        pr->tokens += len;
      }
    a = b;
  }
  /* sorted output, whatever the order the files came in */
  char **lines = must(malloc(((size_t)nf * nf + 1u) * sizeof *lines));
  size_t nl = 0u;
  for (uint32_t i = 0; i < nf; i++)
    for (uint32_t j = i + 1u; j < nf; j++) {
      const Pair *pr = &pairs[(size_t)i * nf + j];
      if (!pr->regions) continue;
      const char *pa = argv[i + 1u], *pb = argv[j + 1u];
      if (strcmp(pa, pb) > 0) { const char *t = pa; pa = pb; pb = t; }
      const size_t sz = strlen(pa) + strlen(pb) + 64u;
      lines[nl] = must(malloc(sz));
      snprintf(lines[nl], sz, "clone %s %s %llu %llu", pa, pb, (unsigned long long)pr->regions, (unsigned long long)pr->tokens);
      nl++;
    }
  qsort(lines, nl, sizeof *lines, cmp_str);
  for (size_t i = 0; i < nl; i++) puts(lines[i]);
  /* the definitions */
  size_t ndef = 0u, capd = 1024u;
  Def *defs = must(malloc(capd * sizeof *defs));
  for (uint32_t i = 0; i < nf; i++) {
    const File *f = &files[i];
    int depth = 0;
    for (uint32_t p = 0; p < f->n; p++) {
      const uint32_t t = f->tok[p];
      if (t == lb) { depth++; continue; }
      if (t == rb) { depth--; continue; }
      if (depth != 0 || t != lp || p == 0u) continue;
      int par = 1;
      uint32_t q = p + 1u;
      while (q < f->n && par > 0) {
        if (f->tok[q] == lp) par++;
        if (f->tok[q] == rp) par--;
        q++;
      }
      if (q < f->n && f->tok[q] == lb && is_id(g_names[f->tok[p - 1u]][0])) {
        if (ndef == capd) {
          capd *= 2u;
          defs = must(realloc(defs, capd * sizeof *defs));
        }
        defs[ndef++] = (Def){f->tok[p - 1u], i};
      }
      p = q - 1u;
    }
  }
  qsort(defs, ndef, sizeof *defs, cmp_def);
  for (size_t a = 0; a < ndef;) {
    size_t b = a + 1u;
    uint32_t n = 1u;
    while (b < ndef && defs[b].name == defs[a].name) {
      n += defs[b].file != defs[b - 1u].file;
      b++;
    }
    if (n > 1u) {
      g_last_name = g_names[defs[a].name];
      printf("def %s %u\n", g_last_name, n);
    }
    a = b;
  }
  return 0;
}
