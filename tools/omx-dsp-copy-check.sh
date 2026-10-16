#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# omx-dsp-copy-check — finds the functions of libomxdsp that a consumer wrote again instead of
# calling, and the list that drives it.
#
#   omx-dsp-copy-check [--root DIR] [--exports FILE] [--allow FILE] [--enforce]
#       Scans the consumer's tracked C (.c, .h) and TypeScript/JavaScript sources under DIR (default
#       the current directory) against the exports file and prints one line per copy. Report mode
#       (the default) always exits 0; --enforce exits 1 when a copy is not covered by the allowlist
#       or the allowlist has a stale or incomplete entry.
#   omx-dsp-copy-check --generate [--version V] OMXDSP_TREE
#       Writes omx-dsp-exports.json for the headers under OMXDSP_TREE/include/omxdsp and the
#       compiled unit OMXDSP_TREE/src to stdout (make exports).
#   omx-dsp-copy-check --self-test [--sabotage]
#       From an omx-dsp source tree: plants copies in a scratch consumer and requires each to be
#       found and each legitimate use to pass; with --sabotage, breaks each rule in turn and
#       requires the self-test to go red.
#
# The exports file lists every public export (a function or macro named omx_*, omxdsp_* or OMX_*)
# with its file, signature, a fingerprint (its body as normalised tokens: identifiers folded to I,
# keywords, maths calls and literals kept, C and Math.* maths names reduced to one spelling), its
# distinctive numeric literals, its maths calls, its inline kernels (the short expressions a tiny
# export is made of, such as `20 * log10 (` or the mono sum `0.5 * ( I [ I ] + I [ I ] )`) and its
# twins (the names a doc comment tags with `@twin`, the same law written in another language).
#
# A consumer function or macro is a copy when
#   name     it defines a name the library exports;
#   twin     it defines a name an export's @twin tag names;
#   body     it shares >= 90 % of the 6-token shingles of the smaller of the two bodies, both at least
#            30 tokens long, and at least one shared shingle carries a maths call or a literal other
#            than 0 and 1 (so struct init, memset and early returns never match);
#   literals it carries two of an export's distinctive literals and every maths call that export
#            makes;
#   kernel   it contains an export's inline kernel (each line is reported).
#
# Out of scope by path: tests, fakes, mocks, fixtures, oracles, generated files (a generated/
# directory or a *.generated.* name), declaration files, build and dependency directories. A
# generated file whose path reads as a source is scanned: its generator writes the same text, and
# the finding points at the code to change.
#
# The allowlist (DIR/omx-dsp-copies.allow) holds one entry per line:
#   path:symbol | export | reason | owner issue
# symbol is the enclosing function's name (`-` at file scope, `*` for the whole file); export is the
# export or twin the copy resembles (`*` for any). An entry that covers no copy is stale and an entry
# without a reason or an owner is incomplete: both fail --enforce, so the list only shrinks.
set -euo pipefail

ME="omx-dsp-copy-check"
AWK="${AWK:-awk}"

# The path rule: what is not a shipped source of the consumer.
SCOPE_OUT='(^|/)(tests?|__tests__|__mocks__|__conformance__|test-support|testing|testdata|fixtures?|mocks?|fakes?|oracles?|e2e|generated|node_modules|dist|build|vendor|third_party|\.nuxt|\.output|coverage)/|\.(test|spec|fake|mock|oracle|e2e)\.[A-Za-z]+$|(^|/)(test|fake|mock)_[^/]*$|_(test|fake|mock|oracle)\.[ch]$|-oracle\.[ch]$|[._-]generated\.|\.gen\.|\.d\.[cm]?ts$'

read -r -d '' PROG <<'AWKEOF' || true
BEGIN {
  # keywords and types stay in the fingerprint; every other name folds to I
  nkw = split("if else for while do return switch case default break continue goto sizeof static inline const volatile extern register struct union enum typedef unsigned signed void char short int long float double bool _Bool size_t ssize_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t NULL true false", a, " ")
  for (i = 1; i <= nkw; i++) { KWC[a[i]] = 1; KWT[a[i]] = 1 }
  nkw = split("typeof let var new delete in of instanceof function class this null undefined async await yield export import from as number boolean string readonly try catch finally throw", a, " ")
  for (i = 1; i <= nkw; i++) KWT[a[i]] = 1
  nctl = split("if for while switch catch return sizeof typeof new await yield with do else case function", a, " ")
  for (i = 1; i <= nctl; i++) CTL[a[i]] = 1
  nb = split("pow exp exp2 expm1 log log10 log2 log1p sin cos tan sinh cosh tanh asin acos atan atan2 sqrt cbrt fabs floor ceil round trunc lrint lround rint fmin fmax fmod hypot", a, " ")
  for (i = 1; i <= nb; i++) { MF[a[i]] = a[i]; MF[a[i] "f"] = a[i]; MF[a[i] "l"] = a[i]; MATHY[a[i]] = 1 }
  MF["fabsf"] = "fabs"
  JS["abs"] = "fabs"; JS["min"] = "fmin"; JS["max"] = "fmax"
  for (i = 1; i <= nb; i++) JS[a[i]] = a[i]
  nc = split("M_PI M_PI_2 M_PI_4 M_2_PI M_SQRT2 M_SQRT1_2 M_LN10 M_LN2 M_E M_LOG10E M_LOG2E", a, " ")
  for (i = 1; i <= nc; i++) { MC[a[i]] = a[i]; MATHY[a[i]] = 1 }
  JSC["PI"] = "M_PI"; JSC["SQRT2"] = "M_SQRT2"; JSC["SQRT1_2"] = "M_SQRT1_2"; JSC["LN10"] = "M_LN10"
  JSC["LN2"] = "M_LN2"; JSC["E"] = "M_E"; JSC["LOG10E"] = "M_LOG10E"; JSC["LOG2E"] = "M_LOG2E"
  nt = split("0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 32 64 128 256 512 1024 100 1000 0.5 0.25 0.75 0.1 0.01 0.001 1e-06 1000000 1e+06 1e+09 1000000000 -1 24 60 127 255 999 32767 32768 65535 2147483647 2147483648 4294967295 4294967296 44100 48000 88200 96000 176400 192000", a, " ")
  for (i = 1; i <= nt; i++) TRIV[a[i]] = 1
  nre = split("( , = : [ ! & | ? { } ; + - * % < > ~ ^ => && || ?? == != <= >= += -= *= /= return typeof case do else in of", a, " ")
  for (i = 1; i <= nre; i++) REOK[a[i]] = 1
  SAB = sabotage
  nfile = 0; nexp = 0; nfind = 0
  if (mode == "check") load_exports()
}

# app: A[k] with v appended after sep (mawk creates A[k] while it evaluates `A[k] = (k in A) ? …`)
function app(A, k, v, sep) { if (k in A) A[k] = A[k] sep v; else A[k] = v }

# ---- the exports file (check mode) ----------------------------------------------------------------
function junesc(s) { gsub(/\\"/, "\"", s); gsub(/\\\\/, "\\", s); return s }
function jstr(line, key,   p, rest, i, n, ch, out) {
  p = index(line, "\"" key "\":\"")
  if (!p) return ""
  rest = substr(line, p + length(key) + 4); n = length(rest); out = ""
  for (i = 1; i <= n; i++) {
    ch = substr(rest, i, 1)
    if (ch == "\\") { out = out ch substr(rest, i + 1, 1); i++; continue }
    if (ch == "\"") break
    out = out ch
  }
  return junesc(out)
}
# jarr: a JSON array of strings as one string joined by "\t"
function jarr(line, key,   p, rest, i, n, ch, out, instr, cur) {
  p = index(line, "\"" key "\":[")
  if (!p) return ""
  rest = substr(line, p + length(key) + 4); n = length(rest); out = ""; instr = 0; cur = ""
  for (i = 1; i <= n; i++) {
    ch = substr(rest, i, 1)
    if (instr) {
      if (ch == "\\") { cur = cur ch substr(rest, i + 1, 1); i++; continue }
      if (ch == "\"") { instr = 0; out = out (out == "" ? "" : "\t") junesc(cur); cur = ""; continue }
      cur = cur ch; continue
    }
    if (ch == "\"") { instr = 1; continue }
    if (ch == "]") break
  }
  return out
}
function load_exports(   line, e, name, kind, fp, nt, t, i, j, sh, lits, nl, l, mths, nm, kerns, nk, k, tw, nw, seen, key) {
  if ((getline line < exports) <= 0) { print "ERROR\tcannot read the exports file " exports; bad_exports = 1; exit 2 }
  do {
    name = jstr(line, "name")
    if (name == "") continue
    e = ++nexp; EN[e] = name; kind = jstr(line, "kind"); EK[e] = kind
    if (kind == "default") continue
    app(NAMEX, name, e, SUBSEP)
    if (kind != "function") continue
    fp = jstr(line, "fingerprint")
    nt = split(fp, t, " "); ENT[e] = nt
    if (nt >= 30) {
      split("", seen); ns = 0
      for (i = 1; i + 5 <= nt; i++) {
        sh = t[i]; for (j = 1; j < 6; j++) sh = sh " " t[i + j]
        if (sh in seen) continue
        seen[sh] = 1; ns++
        app(SHX, sh, e, SUBSEP)
      }
      ENS[e] = ns
    }
    lits = jarr(line, "literals"); nl = (lits == "") ? 0 : split(lits, l, "\t")
    mths = jarr(line, "maths"); nm = (mths == "") ? 0 : split(mths, k, "\t")
    EM[e] = mths
    if (nl >= 2 && nm >= 1) for (i = 1; i <= nl; i++) app(LITX, l[i], e, SUBSEP)
    kerns = jarr(line, "kernels"); nk = (kerns == "") ? 0 : split(kerns, k, "\t")
    for (i = 1; i <= nk; i++) {
      key = k[i]
      if (!(key in KID)) {
        KID[key] = ++nkern; KS[nkern] = key; KNT[nkern] = split(key, t, " ")
        for (j = 1; j <= KNT[nkern]; j++) KT[nkern, j] = t[j]
        app(KFIRST, t[1], nkern, SUBSEP)
      }
      app(KEX, KID[key], name, ",")
    }
    tw = jarr(line, "twins"); nw = (tw == "") ? 0 : split(tw, k, "\t")
    for (i = 1; i <= nw; i++) { sub(/^.*:/, "", k[i]); app(TWX, k[i], name, ",") }
  } while ((getline line < exports) > 0)
  close(exports)
}

# ---- the lexer --------------------------------------------------------------------------------------
function emit(raw, ln, cls) { n++; T[n] = raw; L[n] = ln; C[n] = cls }
function strend(s, q,   i, len, ch) {
  len = length(s)
  for (i = 2; i <= len; i++) { ch = substr(s, i, 1); if (ch == "\\") { i++; continue } if (ch == q) return i }
  return 0
}
function tplend(s,   i, len, ch) {
  len = length(s)
  for (i = 1; i <= len; i++) { ch = substr(s, i, 1); if (ch == "\\") { i++; continue } if (ch == "`") return i }
  return 0
}
function reend(s,   i, len, ch, cls) {
  len = length(s); cls = 0
  for (i = 2; i <= len; i++) {
    ch = substr(s, i, 1)
    if (ch == "\\") { i++; continue }
    if (cls) { if (ch == "]") cls = 0; continue }
    if (ch == "[") { cls = 1; continue }
    if (ch == "/") { while (i < len && substr(s, i + 1, 1) ~ /[a-z]/) i++; return i }
  }
  return 0
}
function twinscan(s,   t) {
  while (match(s, /@twin[ \t]+[A-Za-z_$][A-Za-z0-9_$:.]*/)) {
    t = substr(s, RSTART, RLENGTH); sub(/^@twin[ \t]+/, "", t); sub(/[.:]+$/, "", t)
    ntw++; TW[ntw] = t; TWI[ntw] = n + 1
    s = substr(s, RSTART + RLENGTH)
  }
}
function define(d, ln,   name, body, fnlike) {
  sub(/^[ \t]*#[ \t]*define[ \t]+/, "", d)
  if (!match(d, /^[A-Za-z_][A-Za-z0-9_]*/)) return
  name = substr(d, 1, RLENGTH); body = substr(d, RLENGTH + 1); fnlike = (substr(body, 1, 1) == "(")
  gsub(/\/\*([^*]|\*+[^*\/])*\*+\//, "", body); sub(/\/\/.*$/, "", body); gsub(/^[ \t]+|[ \t]+$/, "", body)
  nmac++; MN[nmac] = name; ML[nmac] = ln; MB[nmac] = body; MFN[nmac] = fnlike; MCF[nmac] = (name == ifndef_name)
}
function lex(s, ln,   c, c2, p, pt) {
  while (s != "") {
    if (inblock) {
      p = index(s, "*/")
      if (!p) { twinscan(s); return }
      twinscan(substr(s, 1, p)); s = substr(s, p + 2); inblock = 0; continue
    }
    if (intpl) { p = tplend(s); if (!p) return; s = substr(s, p + 1); intpl = 0; continue }
    if (match(s, /^[ \t\r\f\v]+/)) { s = substr(s, RLENGTH + 1); continue }
    c = substr(s, 1, 1)
    if (c == "/") {
      c2 = substr(s, 2, 1)
      if (c2 == "/") { twinscan(s); return }
      if (c2 == "*") { inblock = 1; s = substr(s, 3); continue }
      pt = (n > fstart) ? T[n] : ";"
      if (lang == "ts" && (pt in REOK) && C[n] != "N") {
        p = reend(s)
        if (p) { emit("/re/", ln, "S"); s = substr(s, p + 1); continue }
      }
    }
    if (c == "\"" || c == "\047") {
      p = strend(s, c); emit(c c, ln, "S")
      if (!p) return
      s = substr(s, p + 1); continue
    }
    if (c == "`") {
      emit("``", ln, "S"); s = substr(s, 2); p = tplend(s)
      if (!p) { intpl = 1; return }
      s = substr(s, p + 1); continue
    }
    if (match(s, /^(0[xX][0-9a-fA-F_]+[uUlLn]*|[0-9][0-9_]*\.?[0-9_]*([eE][-+]?[0-9]+)?[fFlLuUn]*|\.[0-9][0-9_]*([eE][-+]?[0-9]+)?[fFlL]*)/)) {
      emit(substr(s, 1, RLENGTH), ln, "N"); s = substr(s, RLENGTH + 1); continue
    }
    if (match(s, /^[A-Za-z_$][A-Za-z0-9_$]*/)) { emit(substr(s, 1, RLENGTH), ln, "W"); s = substr(s, RLENGTH + 1); continue }
    if (match(s, /^(>>>=|\.\.\.|===|!==|\*\*=|<<=|>>=|>>>|=>|==|!=|<=|>=|&&|\|\||\?\?|\?\.|<<|>>|\+=|-=|\*=|\/=|%=|&=|\|=|\^=|->|\+\+|--|\*\*|::)/)) {
      emit(substr(s, 1, RLENGTH), ln, "O"); s = substr(s, RLENGTH + 1); continue
    }
    emit(c, ln, "O"); s = substr(s, 2)
  }
}

FNR == 1 {
  if (nfile) finish()
  nfile++; path = FILENAME; n = 0; fstart = 0; inblock = 0; intpl = 0; ppcont = 0; ntw = 0; nmac = 0; skip = 0
  lang = (path ~ /\.[ch]$/) ? "c" : "ts"
}
{
  line = $0
  if (lang == "c" && ppcont) { ppbuf = ppbuf " " line; if (line !~ /\\[ \t]*$/) { ppcont = 0; ppdone(ppbuf) } ; next }
  if (lang == "c" && !inblock && line ~ /^[ \t]*#/) {
    ppbuf = line; ppline = FNR
    if (line ~ /\\[ \t]*$/) { ppcont = 1; next }
    ppdone(ppbuf); next
  }
  lex(line, FNR)
}
function ppdone(d,   t) {
  gsub(/\\[ \t]*/, " ", d)
  if (d ~ /^[ \t]*#[ \t]*define[ \t]/) define(d, ppline)
  # `#ifndef X` then `#define X`: a default the consumer may set, not an export
  t = d; ifndef_name = ""
  if (t ~ /^[ \t]*#[ \t]*ifndef[ \t]/) { sub(/^[ \t]*#[ \t]*ifndef[ \t]+/, "", t); sub(/[ \t].*$/, "", t); ifndef_name = t }
  t = d; gsub(/\/\*([^*]|\*+[^*\/])*\*+\//, "", t)
  if (index(t, "/*")) inblock = 1
}

# ---- one file's tokens: fold, normalise, find the functions -----------------------------------------
function isnum(t) { return t ~ /^([0-9]|\.[0-9])/ }
function canon(t,   s) {
  s = t; gsub(/_/, "", s)
  if (s ~ /^0[xX]/) { sub(/[uUlLn]+$/, "", s); return tolower(s) }
  sub(/[fFlLuUn]+$/, "", s)
  return sprintf("%.15g", s + 0)
}
function trivial(t) { return (t in TRIV) || t ~ /^0x/ }
function mathy(t) { return (t in MATHY) || (isnum(t) && !trivial(t)) }
function fold(   i, m2, r) {
  for (i = n + 1; i <= n + 3; i++) { T[i] = ""; C[i] = "" }
  m = 0
  for (i = 1; i <= n; i++) {
    r = T[i]
    if (lang == "ts" && r == "Math" && T[i + 1] == "." && C[i + 2] == "W" && ((T[i + 2] in JS) || (T[i + 2] in JSC))) {
      m++; R[m] = "Math." T[i + 2]; K[m] = "W"; LN[m] = L[i]; SRC[m] = i
      NR_[m] = (T[i + 2] in JS) ? JS[T[i + 2]] : JSC[T[i + 2]]
      i += 2; continue
    }
    m++; R[m] = r; K[m] = C[i]; LN[m] = L[i]; SRC[m] = i
    if (C[i] == "N") NR_[m] = canon(r)
    else if (C[i] == "S") NR_[m] = "S"
    else if (C[i] == "W") {
      if ((r in MF) && T[i + 1] == "(" && T[i - 1] != ".") NR_[m] = MF[r]
      else if (r in MC) NR_[m] = MC[r]
      else if (lang == "c" ? (r in KWC) : (r in KWT)) NR_[m] = r
      else NR_[m] = "I"
    } else if (r == "===") NR_[m] = "=="
    else if (r == "!==") NR_[m] = "!="
    else NR_[m] = r
  }
  for (i = m + 1; i <= m + 20; i++) { R[i] = ""; NR_[i] = ""; K[i] = ""; MT[i] = 0 }
  # matching brackets and brace depth; `extern "C" {` is transparent
  sp = 0; dep = 0
  for (i = 1; i <= m; i++) {
    MT[i] = 0; D[i] = dep
    r = R[i]
    if (r == "(" || r == "[" || r == "{") {
      sp++; ST[sp] = i
      STT[sp] = (r == "{" && i > 2 && R[i - 2] == "extern" && K[i - 1] == "S") ? 1 : 0
      if (r == "{" && !STT[sp]) dep++
    } else if (r == ")" || r == "]" || r == "}") {
      if (sp > 0) {
        MT[ST[sp]] = i; MT[i] = ST[sp]
        if (r == "}" && !STT[sp]) dep--
        sp--
      }
      if (dep < 0) dep = 0
      D[i] = dep
    }
  }
  for (i = 1; i <= sp; i++) MT[ST[i]] = m
}
# skip a TypeScript type that starts after index k (k is the ':'); returns the index of the token
# that ends it: the body's '{', or '=>' when arrow is set
function skiptype(k, arrow,   r, p) {
  k++
  while (k <= m) {
    r = R[k]; p = R[k - 1]
    if (r == "{") {
      if (p == ":" || p == "|" || p == "&" || p == "<" || p == "," || p == "(" || p == "[" || p == "=>" || p == "?") { k = MT[k] + 1; continue }
      return k
    }
    if (r == "=>" && arrow) return k
    if (r == ";" || r == "=") return 0
    if (r == "(" || r == "[") { k = MT[k] + 1; continue }
    k++
  }
  return 0
}
function skipgen(k,   d2) {
  d2 = 0
  while (k <= m) {
    if (R[k] == "<") d2++
    else if (R[k] == ">") d2--
    else if (R[k] == ">>") d2 -= 2
    else if (R[k] == ";" || R[k] == "{") return 0
    k++
    if (d2 <= 0) return k
  }
  return 0
}
function addfn(name, ni, b0, b1, kind) { nf++; FN[nf] = name; FNI[nf] = ni; FB0[nf] = b0; FB1[nf] = b1; FK[nf] = kind }
function exprend(k,   d2, r, l0) {
  d2 = 0; l0 = LN[k]
  while (k <= m) {
    r = R[k]
    if (r == "(" || r == "[" || r == "{") d2++
    else if (r == ")" || r == "]" || r == "}") { d2--; if (d2 < 0) return k - 1 }
    else if (d2 == 0 && (r == ";" || r == ",")) return k - 1
    else if (d2 == 0 && LN[k] > l0 && (r == "const" || r == "let" || r == "export" || r == "function" || r == "return" || r == "if" || r == "import")) return k - 1
    k++
  }
  return m
}
function functions(   i, j, k, r, b) {
  nf = 0; nd = 0
  for (i = 1; i <= m; i++) {
    if (K[i] != "W" || (R[i] in CTL) || R[i] == "Math.") continue
    if (lang == "c") {
      if (D[i] != 0 || R[i + 1] != "(" || (R[i] in KWC)) continue
      j = MT[i + 1]
      if (j && R[j + 1] == "{") { addfn(R[i], i, j + 2, MT[j + 1] - 1, "fn"); i = MT[j + 1] }
      else if (j && R[j + 1] == ";") { nd++; DN[nd] = R[i]; DNI[nd] = i }
      continue
    }
    if (i > 1 && (R[i - 1] == "." || R[i - 1] == "?.")) continue
    j = i + 1
    if (R[j] == "<") { j = skipgen(j); if (!j) continue }
    if (R[j] == "(" && !(R[i] in KWT)) {
      k = MT[j] + 1
      if (R[k] == ":") k = skiptype(k, 0)
      if (k && R[k] == "{") { addfn(R[i], i, k + 1, MT[k] - 1, "fn"); continue }
    }
    if (R[i + 1] == "=" || (R[i + 1] == ":" && (R[i - 1] == "const" || R[i - 1] == "let" || R[i - 1] == "var"))) {
      if (R[i - 1] != "const" && R[i - 1] != "let" && R[i - 1] != "var" && R[i - 1] != "readonly" && R[i - 1] != "static" && R[i - 1] != "private" && R[i - 1] != "public") continue
      j = i + 1
      if (R[j] == ":") { j = skiptype(j, 0); if (!j) { j = i + 1; while (j <= m && R[j] != "=" && R[j] != ";") j++ } }
      if (R[j] != "=") continue
      j++
      if (R[j] == "async") j++
      if (R[j] == "function") { j++; if (K[j] == "W") j++ }
      if (R[j] == "<") { j = skipgen(j); if (!j) continue }
      if (R[j] == "(") { k = MT[j] + 1 }
      else if (K[j] == "W" && R[j + 1] == "=>") { k = j + 1 }
      else continue
      if (R[k] == ":") { k = skiptype(k, 1); if (!k) continue }
      if (R[k] == "=>") {
        k++
        if (R[k] == "{") addfn(R[i], i, k + 1, MT[k] - 1, "fn")
        else addfn(R[i], i, k, exprend(k), "fn")
      } else if (R[k] == "{") addfn(R[i], i, k + 1, MT[k] - 1, "fn")
    }
  }
  # the innermost function owns each token
  for (i = 1; i <= m; i++) OWN[i] = 0
  for (b = 1; b <= nf; b++) for (i = FB0[b]; i <= FB1[b]; i++) if (!OWN[i] || FB0[OWN[i]] <= FB0[b]) OWN[i] = b
}

function finish() {
  if (skip) return
  fold(); functions()
  if (mode == "gen") gen_file(); else check_file()
}
END {
  if (bad_exports) exit 2
  if (nfile) finish()
  if (mode == "check") check_end()
}

# ---- generate ---------------------------------------------------------------------------------------
function jesc(s) { gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); return s }
function jlist(arr, cnt,   i, out) { out = ""; for (i = 1; i <= cnt; i++) out = out (i > 1 ? "," : "") "\"" jesc(arr[i]) "\""; return "[" out "]" }
function sig(f,   s, i, out) {
  s = FNI[f] - 1
  while (s >= 1 && R[s] != ";" && R[s] != "}" && R[s] != "{") s--
  out = R[s + 1]
  for (i = s + 2; i <= MT[FNI[f] + 1]; i++) out = out " " R[i]
  gsub(/ ,/, ",", out); gsub(/\( /, "(", out); gsub(/ \)/, ")", out); gsub(/ \(/, "(", out)
  gsub(/ \[/, "[", out); gsub(/\[ /, "[", out); gsub(/ \]/, "]", out)
  return out
}
# whole(i): the expression starting at token i is a whole value (returned, assigned or an argument),
# not one term of a larger one: a kernel is the export itself, never a fragment of another formula
function whole(i,   p) { p = R[i - 1]; return p == "return" || p == "=" || p == "(" || p == "," }
# kernels: the inline expressions a short export is made of, in normalised tokens
#   L * f (            a literal times a maths call, the call's argument free (20 * log10 ()
#   f ( … L … )        a maths call with a literal argument (pow ( 10 , I * 0.05 ))
#   L * ( … [ … )      a literal times an element-wise group (the mono sum)
function kernels(f, b0, b1,   j, k, has, out, i, cnt) {
  cnt = 0
  for (j = b0; j <= b1; j++) {
    if ((NR_[j] in MATHY) && R[j + 1] == "(" && NR_[j] != "fmin" && NR_[j] != "fmax" && NR_[j] != "fabs") {
      k = MT[j + 1]
      if (j - 2 >= b0 && isnum(NR_[j - 2]) && NR_[j - 2] != "0" && NR_[j - 2] != "1" && NR_[j - 2] != "2" && (R[j - 1] == "*" || R[j - 1] == "/")) {
        if (whole(j - 2)) KO[++cnt] = NR_[j - 2] " " R[j - 1] " " NR_[j] " ("
      } else if (k - j <= 14 && whole(j)) {
        has = 0; for (i = j + 2; i < k; i++) if (isnum(NR_[i]) && !trivial(NR_[i]) || NR_[i] == "10") has = 1
        if (has) { out = NR_[j]; for (i = j + 1; i <= k; i++) out = out " " NR_[i]; KO[++cnt] = out }
      }
    }
    if (isnum(NR_[j]) && R[j + 1] == "*" && R[j + 2] == "(" && whole(j)) {
      k = MT[j + 2]
      if (k - j <= 16) {
        has = 0; for (i = j + 3; i < k; i++) if (R[i] == "[") has = 1
        if (has) { out = NR_[j]; for (i = j + 1; i <= k; i++) out = out " " NR_[i]; KO[++cnt] = out }
      }
    }
  }
  return cnt
}
function gen_file(   f, i, b0, b1, fp, nl, nm, nk, nw, t, sl, sm, x, name, relp, issrc, sg, best, bname, isdecl) {
  relp = path; sub(/^.*\/include\//, "", relp); sub(/^.*\/src\//, "src/", relp)
  issrc = (path ~ /\.c$/)
  # each @twin tag names the first omx function defined or declared after it
  split("", FTW)
  for (t = 1; t <= ntw; t++) {
    best = 0; bname = ""; isdecl = 0
    for (f = 1; f <= nf; f++) if (FN[f] ~ /^omx(dsp)?_/ && FNI[f] >= TWI[t] && (!best || FNI[f] < best)) { best = FNI[f]; bname = FN[f]; isdecl = 0 }
    for (f = 1; f <= nd; f++) if (DN[f] ~ /^omx(dsp)?_/ && DNI[f] >= TWI[t] && (!best || DNI[f] < best)) { best = DNI[f]; bname = DN[f]; isdecl = 1 }
    if (!best) continue
    if (isdecl) app(DTW, bname, TW[t], "\t")
    else app(FTW, bname, TW[t], "\t")
  }
  for (f = 1; f <= nf; f++) {
    name = FN[f]
    if (name !~ /^omx(dsp)?_/) continue
    sg = sig(f)
    if (issrc && sg ~ /(^| )static / && sg !~ / inline /) continue
    b0 = FB0[f]; b1 = FB1[f]; fp = ""; nl = 0; nm = 0
    split("", sl); split("", sm)
    for (i = b0; i <= b1; i++) {
      fp = fp (i > b0 ? " " : "") NR_[i]
      if (isnum(NR_[i]) && !trivial(NR_[i]) && !(NR_[i] in sl)) { sl[NR_[i]] = 1; LIT[++nl] = NR_[i] }
      if ((NR_[i] in MATHY) && !(NR_[i] in sm)) { sm[NR_[i]] = 1; MTH[++nm] = NR_[i] }
    }
    nk = (b1 - b0 + 1 <= 40) ? kernels(f, b0, b1) : 0
    nw = 0
    if (name in FTW) nw = split(FTW[name], TWN, "\t")
    if (name in DTW) { split(DTW[name], x, "\t"); for (t = 1; x[t] != ""; t++) TWN[++nw] = x[t] }
    printf("{\"name\":\"%s\",\"kind\":\"function\",\"file\":\"%s\",\"line\":%d,\"signature\":\"%s\",\"tokens\":%d,\"fingerprint\":\"%s\",\"literals\":%s,\"maths\":%s,\"kernels\":%s,\"twins\":%s}\n",
      name, jesc(relp), LN[FNI[f]], jesc(sg), b1 - b0 + 1, jesc(fp), jlist(LIT, nl), jlist(MTH, nm), jlist(KO, nk), jlist(TWN, nw)) > out_fns
  }
  for (i = 1; i <= nmac; i++) {
    name = MN[i]
    if (name !~ /^(OMX|OMXDSP|omx)_/ || name == "OMX_CONTRACT_STAGE" || (name ~ /_H$/ && MB[i] == "")) continue
    printf("{\"name\":\"%s\",\"kind\":\"%s\",\"file\":\"%s\",\"line\":%d,\"signature\":\"%s\"}\n", name, MCF[i] ? "default" : "macro", jesc(relp), ML[i], jesc("#define " name (MFN[i] ? substr(MB[i], 1, index(MB[i], ")")) : ""))) > out_macs
  }
}

# ---- check ------------------------------------------------------------------------------------------
function find(ln, sym, rule, ex, detail,   key) {
  key = path SUBSEP sym SUBSEP rule SUBSEP ex SUBSEP (rule == "kernel" ? ln : "")
  if (key in FSEEN) return
  FSEEN[key] = 1
  nfind++; FP_[nfind] = path; FL[nfind] = ln; FSY[nfind] = sym; FR[nfind] = rule; FE[nfind] = ex; FD[nfind] = detail
}
function owner(i) { return OWN[i] ? FN[OWN[i]] : "-" }
function check_file(   f, i, j, e, x, nx, b0, b1, nt, sh, ns, cnt, mcnt, ov, lits, ms, k, kl, ok, name, ex) {
  for (f = 1; f <= nf; f++) {
    name = FN[f]; b0 = FB0[f]; b1 = FB1[f]
    if (SAB != "name" && (name in NAMEX)) find(LN[FNI[f]], name, "name", name, "")
    if (SAB != "twin" && (name in TWX)) find(LN[FNI[f]], name, "twin", TWX[name], "")
    # body: shingle overlap
    nt = b1 - b0 + 1
    if (SAB != "body" && nt >= 30) {
      split("", seen); split("", cnt); split("", mcnt); ns = 0
      for (i = b0; i + 5 <= b1; i++) {
        sh = NR_[i]; for (j = 1; j < 6; j++) sh = sh " " NR_[i + j]
        if (sh in seen) continue
        seen[sh] = 1; ns++
        if (!(sh in SHX)) continue
        ok = 0; for (j = 0; j < 6; j++) if (mathy(NR_[i + j])) ok = 1
        nx = split(SHX[sh], x, SUBSEP)
        for (j = 1; j <= nx; j++) { cnt[x[j]]++; if (ok) mcnt[x[j]]++ }
      }
      for (e in cnt) {
        ov = cnt[e] / (ns < ENS[e] ? ns : ENS[e])
        if (ov >= 0.9 && (mcnt[e] >= 1 || SAB == "fpcut")) find(LN[FNI[f]], name, "body", EN[e], sprintf("overlap %.2f", ov))
      }
    }
    # literals
    if (SAB != "literals") {
      split("", lits); split("", ms); split("", cnt)
      for (i = b0; i <= b1; i++) {
        if (isnum(NR_[i]) && !trivial(NR_[i])) lits[NR_[i]] = 1
        if (NR_[i] in MATHY) ms[NR_[i]] = 1
      }
      for (k in lits) if (k in LITX) { nx = split(LITX[k], x, SUBSEP); for (j = 1; j <= nx; j++) cnt[x[j]]++ }
      for (e in cnt) {
        if (cnt[e] < 2) continue
        nx = split(EM[e], x, "\t"); ok = 1
        for (j = 1; j <= nx; j++) if (!(x[j] in ms)) ok = 0
        if (ok) find(LN[FNI[f]], name, "literals", EN[e], cnt[e] " literals")
      }
    }
  }
  # kernels, anywhere in the file
  if (SAB != "kernel") for (i = 1; i <= m; i++) {
    if (!(NR_[i] in KFIRST)) continue
    nx = split(KFIRST[NR_[i]], x, SUBSEP)
    for (j = 1; j <= nx; j++) {
      k = x[j]; ok = 1
      for (kl = 2; kl <= KNT[k]; kl++) if (NR_[i + kl - 1] != KT[k, kl]) { ok = 0; break }
      if (ok) find(LN[i], owner(i), "kernel", KEX[k], "`" KS[k] "`")
    }
  }
  # the dB-to-linear kernel written with a divisor, pow ( 10 , x / 20 ) in dB or pow ( 10 , x / 2000 )
  # in centi-dB, pow or powf: the export's pow ( 10 , x * 0.05 ) with the reciprocal spelled out
  dbk = "pow ( 10 , I * 0.05 )"
  if (SAB != "kernel" && (dbk in KID)) for (i = 1; i <= m; i++) {
    if (NR_[i] != "pow" || R[i + 1] != "(" || NR_[i + 2] != "10" || R[i + 3] != ",") continue
    dk = MT[i + 1]
    if (dk - i > 14 || NR_[dk - 2] != "/" || (NR_[dk - 1] != "20" && NR_[dk - 1] != "2000")) continue
    dout = ""; for (dj = i; dj <= dk; dj++) dout = dout (dj > i ? " " : "") NR_[dj]
    find(LN[i], owner(i), "kernel", KEX[KID[dbk]], "`" dout "`")
  }
  # a macro that defines an exported name
  if (SAB != "name") for (i = 1; i <= nmac; i++) if (MN[i] in NAMEX) find(ML[i], MN[i], "name", MN[i], "#define")
}
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function check_end(   line, ln, nf2, fl, i, p, a, sym, ex, f, cov, nnew, nallowed, nprob, exl, nx, x, j, hit) {
  nprob = 0; na = 0; ln = 0
  if (allow != "" && (getline line < allow) > 0) {
    do {
      ln++
      if (line ~ /^[ \t]*(#|$)/) continue
      na++; AL[na] = ln; ATXT[na] = line
      nf2 = split(line, fl, "|")
      for (i = 1; i <= nf2; i++) fl[i] = trim(fl[i])
      p = fl[1]; sym = p; sub(/:[^:]*$/, "", p); sub(/^.*:/, "", sym)
      if (index(fl[1], ":") == 0) { sym = ""; p = fl[1] }
      AP[na] = p; AS[na] = sym; AE[na] = fl[2]; ABAD[na] = ""
      if (nf2 < 2 || fl[1] == "" || sym == "" || fl[2] == "") ABAD[na] = "malformed: path:symbol | export | reason | owner issue"
      else if (SAB != "reason" && (nf2 < 3 || fl[3] == "")) ABAD[na] = "no reason"
      else if (SAB != "reason" && (nf2 < 4 || fl[4] == "")) ABAD[na] = "no owner issue"
    } while ((getline line < allow) > 0)
    close(allow)
  }
  nnew = 0; nallowed = 0
  for (f = 1; f <= nfind; f++) {
    cov = 0
    for (a = 1; a <= na; a++) {
      if (AP[a] != FP_[f] || (AS[a] != "*" && AS[a] != FSY[f])) continue
      hit = 0
      if (AE[a] == "*") hit = 1
      else { nx = split(FE[f], x, ","); for (j = 1; j <= nx; j++) if (x[j] == AE[a]) hit = 1 }
      if (hit) { cov = a; AUSED[a] = 1; break }
    }
    if (cov && ABAD[cov] == "") { nallowed++; st = "allowed" } else { nnew++; st = "copy" }
    printf("%s\t%s\t%d\t%s\t%s\t%s\t%s\n", st, FP_[f], FL[f], FSY[f], FR[f], FE[f], FD[f])
  }
  for (a = 1; a <= na; a++) {
    if (ABAD[a] != "") { nprob++; printf("problem\t%s\t%d\tincomplete\t%s\t%s\n", allow, AL[a], ABAD[a], ATXT[a]) }
    else if (!AUSED[a] && SAB != "stale") { nprob++; printf("problem\t%s\t%d\tstale\tcovers no copy\t%s\n", allow, AL[a], ATXT[a]) }
  }
  printf("summary\t%d\t%d\t%d\t%d\t%d\n", nfind, nallowed, nnew, nprob, nfile)
}
AWKEOF

die() { echo "$ME: $*" >&2; exit 2; }

# generate <omx-dsp tree> <version>: the exports file on stdout
generate() {
  local tree="$1" version="$2" tmp
  [ -d "$tree/include/omxdsp" ] || die "$tree has no include/omxdsp"
  tmp="$(mktemp -d)"
  local files=()
  while IFS= read -r f; do files+=("$f"); done < <(cd "$tree" && { find include/omxdsp -name '*.h' | LC_ALL=C sort; find src -maxdepth 1 -name '*.c' 2>/dev/null | LC_ALL=C sort; })
  (cd "$tree" && "$AWK" -v mode=gen -v out_fns="$tmp/fns" -v out_macs="$tmp/macs" -v sabotage="${OMX_COPY_CHECK_SABOTAGE:-}" "$PROG" "${files[@]}")
  : >>"$tmp/fns"; : >>"$tmp/macs"
  {
    printf '{"format":"omx-dsp-exports 1","omxdsp_version":"%s","shingle":6,"min_tokens":30,"overlap":0.9,"exports":[\n' "$version"
    "$AWK" '!seen[$0]++' "$tmp/fns" "$tmp/macs" | "$AWK" 'BEGIN{first=1} { if (index($0, "\"kind\":\"macro\"") || index($0, "\"kind\":\"default\"")) { split($0, a, "\""); if (a[4] in mac) next; mac[a[4]] = 1 } printf("%s%s", first ? "" : ",\n", $0); first = 0 } END { printf("\n") }'
    printf ']}\n'
  }
  rm -rf "$tmp"
}

# find_exports: the installed exports file, next to this script or where pkg-config says
find_exports() {
  local here d
  here="$(cd "$(dirname "$0")" && pwd)"
  for d in "$here/../share/omxdsp" "$here/../build" "$(dirname "$(pkg-config --variable=exports omxdsp 2>/dev/null)")" /usr/share/omxdsp /usr/local/share/omxdsp; do
    [ -f "$d/omx-dsp-exports.json" ] && { echo "$d/omx-dsp-exports.json"; return; }
  done
  return 1
}

# sources <root>: the consumer's C and TypeScript sources in scope, relative to root
sources() {
  local root="$1" list
  if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    list="$(cd "$root" && git ls-files -- '*.c' '*.h' '*.ts' '*.mts' '*.cts' '*.js' '*.mjs' '*.cjs')"
  else
    list="$(cd "$root" && find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.ts' -o -name '*.mts' -o -name '*.cts' -o -name '*.js' -o -name '*.mjs' -o -name '*.cjs' \) | sed 's|^\./||')"
  fi
  if [ "${OMX_COPY_CHECK_SABOTAGE:-}" = scope ]; then printf '%s\n' "$list"; else printf '%s\n' "$list" | grep -vE "$SCOPE_OUT" || true; fi | LC_ALL=C sort | while IFS= read -r f; do [ -f "$root/$f" ] && printf '%s\n' "$f"; done
}

# check <root> <exports> <allow> <enforce>
check() {
  local root="$1" exports="$2" allow="$3" enforce="$4" out files=() rc=0
  [ -f "$exports" ] || die "no exports file at $exports"
  case "$exports" in /*) ;; *) exports="$PWD/$exports" ;; esac
  case "$allow" in ''|/*) ;; *) allow="$PWD/$allow" ;; esac
  [ -n "$allow" ] && [ ! -f "$allow" ] && allow=""
  while IFS= read -r f; do [ -n "$f" ] && files+=("$f"); done < <(sources "$root")
  if [ "${#files[@]}" -eq 0 ]; then echo "$ME: no C or TypeScript sources in scope under $root"; return 0; fi
  out="$(cd "$root" && "$AWK" -v mode=check -v exports="$exports" -v allow="$allow" -v sabotage="${OMX_COPY_CHECK_SABOTAGE:-}" "$PROG" "${files[@]}")" || rc=$?
  [ "$rc" = 0 ] || die "the scan failed: $out"
  printf '%s\n' "$out" | "$AWK" -F'\t' -v me="$ME" -v enforce="$enforce" -v allowf="${allow:-none}" '
    $1 == "copy" || $1 == "allowed" { printf("%-7s %s:%s %s %s %s%s\n", $1, $2, $3, $4, $5, $6, ($7 == "" ? "" : "  (" $7 ")")) }
    $1 == "problem" { printf("allow   %s:%s %s: %s: %s\n", $2, $3, $4, $5, $6) }
    $1 == "summary" { s = $0; split($0, x, "\t") }
    END {
      printf("%s: %d files, %d copies (%d allowed, %d not), %d allowlist problems (allowlist: %s)%s\n", me, x[6], x[2], x[3], x[4], x[5], allowf, enforce ? "" : "; report mode")
      if (enforce && (x[4] > 0 || x[5] > 0)) exit 1
    }'
}

# ---- self-test --------------------------------------------------------------------------------------
# plant <dir>: a consumer tree with one planted case per rule, an allowlist, and legitimate uses
plant() {
  local d="$1"
  mkdir -p "$d/src" "$d/web" "$d/tests" "$d/tools"
  # exact copy: an export's body under its own name
  cat >"$d/src/exact.c" <<'EOF'
#include <stdint.h>
#include <math.h>
static inline float omx_meter_block_peak(const float *a, const float *b, uint32_t n) {
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    float x = fabsf(a[i]);
    if (x > peak) peak = x;
    if (b != NULL && b != a) {
      float y = fabsf(b[i]);
      if (y > peak) peak = y;
    }
  }
  return peak;
}
EOF
  # renamed copy: the same body, every name changed
  cat >"$d/src/renamed.c" <<'EOF'
#include <stdint.h>
#include <math.h>
float leg_peak(const float *left, const float *right, uint32_t frames) {
  float top = 0.0f;
  for (uint32_t k = 0; k < frames; k++) {
    float v = fabsf(left[k]);
    if (v > top) top = v;
    if (right != NULL && right != left) {
      float w = fabsf(right[k]);
      if (w > top) top = w;
    }
  }
  return top;
}
EOF
  # twin-tagged copy: the TypeScript law the library names as its twin
  cat >"$d/web/width.ts" <<'EOF'
export function octavesToQ(octaves: number): number {
  const n = Math.pow(2, octaves);
  return Math.sqrt(n) / (n - 1);
}
EOF
  # literals: two of an export's constants with its maths calls, written another way
  cat >"$d/web/pitch.ts" <<'EOF2'
export const noteOf = (hz: number): number => Math.floor(69 + 12 * Math.log2(hz / 440) + 0.5);
EOF2
  # inline kernels: a mono sum and a dB conversion inside unrelated functions
  cat >"$d/src/inline.c" <<'EOF'
#include <stdint.h>
#include <math.h>
void key_build(float *kb, const float *l, const float *r, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) kb[i] = 0.5f * (l[i] + r[i]);
}
double report_level(double rms) {
  return rms > 0 ? 20.0 * log10(rms) : -999.0;
}
float headamp_gain(int cdb) {
  return powf(10.0f, (float)cdb / 2000.0f);
}
double fader_gain(double db) {
  return pow(10.0, db / 20.0);
}
EOF
  # legitimate: calls the library, and boilerplate shaped like the library's state init
  cat >"$d/src/legit.c" <<'EOF'
#include <stdint.h>
#include <string.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/mix/omx_meter.h>
struct slot { int on; float gain; };
struct bus { struct slot slot[8]; };
double strip_level(const float *l, const float *r, uint32_t n) {
  return omx_lin_to_db_d(omx_meter_block_peak(l, r, n), 1e-9);
}
uint32_t count_enabled(const struct bus *b, uint32_t n) {
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++)
    if (b->slot[i].on) c++;
  return c;
}
EOF
  cat >"$d/web/legit.ts" <<'EOF'
export function clampPercent(x: number): number {
  return Math.min(100, Math.max(0, Math.round(x)));
}
EOF
  # out of scope by path: a test and a fake carry copies on purpose
  cp "$d/src/exact.c" "$d/tests/peak_oracle.c"
  cp "$d/src/renamed.c" "$d/src/meter.fake.c"
  cp "$d/web/width.ts" "$d/web/width.test.ts"
  cat >"$d/omx-dsp-copies.allow" <<'EOF'
# path:symbol | export | reason | owner issue
src/renamed.c:leg_peak | omx_meter_block_peak | measurement tool, keeps its own peak on purpose | example#1
src/gone.c:old_peak | omx_meter_block_peak | the file was removed | example#2
web/pitch.ts:noteOf | omx_hrp_midi_note | | example#3
EOF
  # tracked files when git is there (the consumer's CI case), every file otherwise
  if command -v git >/dev/null 2>&1; then git -C "$d" init -q && git -C "$d" add -A; fi
}

# expect <output> <present|absent> <pattern> <what>
expect() {
  if printf '%s\n' "$1" | grep -qE -- "$3"; then
    [ "$2" = present ] && return 0
    echo "  FAIL: $4 (unexpected: $(printf '%s\n' "$1" | grep -E -- "$3" | head -1))"; return 1
  fi
  [ "$2" = absent ] && return 0
  echo "  FAIL: $4 (missing)"; return 1
}

self_test_once() {
  local tree="$1" d out rc fails=0
  d="$(mktemp -d)"
  generate "$tree" self-test >"$d/exports.json"
  plant "$d/c"
  out="$(check "$d/c" "$d/exports.json" "$d/c/omx-dsp-copies.allow" 0 2>&1)" || { echo "  FAIL: report mode exited non-zero"; fails=$((fails + 1)); }
  [ -n "${OMX_COPY_CHECK_VERBOSE:-}" ] && printf '%s\n' "$out"
  expect "$out" present '^copy +src/exact\.c:3 omx_meter_block_peak name ' "exact copy found by name" || fails=$((fails + 1))
  expect "$out" present '^copy +src/exact\.c:3 omx_meter_block_peak body ' "exact copy found by body" || fails=$((fails + 1))
  expect "$out" present '^allowed src/renamed\.c:3 leg_peak body omx_meter_block_peak' "renamed copy found by body, covered by its entry" || fails=$((fails + 1))
  expect "$out" present '^copy +web/width\.ts:1 octavesToQ twin omx_eq_octaves_q' "twin-tagged copy found" || fails=$((fails + 1))
  expect "$out" present '^copy +web/pitch\.ts:1 noteOf literals omx_hrp_midi_note' "literal copy found" || fails=$((fails + 1))
  expect "$out" present '^copy +src/inline\.c:4 key_build kernel omx_rta_tap_mono_sum' "inline mono sum found" || fails=$((fails + 1))
  expect "$out" present '^copy +src/inline\.c:7 report_level kernel .*omx_lin_to_db_d' "inline dB conversion found" || fails=$((fails + 1))
  expect "$out" present '^copy +src/inline\.c:10 headamp_gain kernel .*omx_db_to_lin' "divisor form in centi-dB found" || fails=$((fails + 1))
  expect "$out" present '^copy +src/inline\.c:13 fader_gain kernel .*omx_db_to_lin' "divisor form in dB found" || fails=$((fails + 1))
  expect "$out" absent 'legit\.(c|ts)' "legitimate callers and boilerplate pass" || fails=$((fails + 1))
  expect "$out" absent '(tests/|\.fake\.|\.test\.)' "tests, fakes and oracles are out of scope" || fails=$((fails + 1))
  expect "$out" present '^allow .*:3 stale: .*src/gone\.c:old_peak' "stale allowlist entry fails" || fails=$((fails + 1))
  expect "$out" present '^allow .*:4 incomplete: no reason' "allowlist entry without a reason fails" || fails=$((fails + 1))
  rc=0; check "$d/c" "$d/exports.json" "$d/c/omx-dsp-copies.allow" 1 >/dev/null 2>&1 || rc=$?
  [ "$rc" = 1 ] || { echo "  FAIL: --enforce exited $rc over copies, want 1"; fails=$((fails + 1)); }
  # a clean consumer passes --enforce
  mkdir -p "$d/clean/src"; cp "$d/c/src/legit.c" "$d/clean/src/"
  rc=0; check "$d/clean" "$d/exports.json" "" 1 >/dev/null 2>&1 || rc=$?
  [ "$rc" = 0 ] || { echo "  FAIL: --enforce exited $rc over a clean consumer, want 0"; fails=$((fails + 1)); }
  rm -rf "$d"
  return "$fails"
}

SABOTAGES="name twin body literals kernel fpcut scope stale reason"

self_test() {
  local tree sab="$1" s fails
  tree="$(cd "$(dirname "$0")/.." && pwd)"
  [ -d "$tree/include/omxdsp" ] || die "--self-test runs from an omx-dsp source tree"
  if [ "$sab" = 0 ]; then
    fails=0; self_test_once "$tree" || fails=$?
    [ "$fails" = 0 ] || { echo "$ME --self-test: $fails failures"; exit 1; }
    echo "$ME --self-test: every planted copy found, every legitimate use passes, the allowlist rules hold"
    return
  fi
  for s in $SABOTAGES; do
    fails=0
    OMX_COPY_CHECK_SABOTAGE="$s" self_test_once "$tree" >/dev/null || fails=$?
    if [ "$fails" = 0 ]; then echo "$ME --self-test --sabotage: rule '$s' broken and the self-test stayed green"; exit 1; fi
    echo "$ME --self-test --sabotage: rule '$s' broken: red ($fails failures), as required"
  done
}

# ---- main -------------------------------------------------------------------------------------------
root=.; exports=""; allow=""; enforce=0; mode=check; version=unknown; sab=0; tree=""
while [ $# -gt 0 ]; do
  case "$1" in
    --root) root="$2"; shift 2 ;;
    --exports) exports="$2"; shift 2 ;;
    --allow) allow="$2"; shift 2 ;;
    --enforce) enforce=1; shift ;;
    --generate) mode=generate; shift ;;
    --version) version="$2"; shift 2 ;;
    --self-test) mode=self-test; shift ;;
    --sabotage) sab=1; shift ;;
    -h|--help) sed -n '5,47p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) die "unknown option $1 (see --help)" ;;
    *) tree="$1"; shift ;;
  esac
done
case "$mode" in
  generate) [ -n "$tree" ] || die "--generate needs the omx-dsp tree"; generate "$tree" "$version" ;;
  self-test) self_test "$sab" ;;
  check)
    [ -d "$root" ] || die "no directory $root"
    [ -n "$exports" ] || exports="$(find_exports)" || die "no omx-dsp-exports.json found: install omx-dsp-devel / libomxdsp-dev or pass --exports"
    [ -n "$allow" ] || allow="$root/omx-dsp-copies.allow"
    check "$root" "$exports" "$allow" "$enforce" ;;
esac
