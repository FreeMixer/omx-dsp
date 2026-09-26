#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# doc-check.sh — every public symbol of libomxdsp carries its structured doc comment
# (docs/design/specs/2026-09-26-dsp-primitives.md §6), checked without doxygen: pure awk.
#
# For every `include/omxdsp/*.h` (or the files given as arguments), at file scope:
#   - a function (`static inline` definition or a non-static declaration) named omx_*/omxdsp_*,
#   - a `struct omx_*`/`enum omx_*` definition or a `typedef struct { … } Omx*;`,
#   - a `#define OMX…` macro (its FIRST definition in the file; the release twin needs none),
# `omx_contract_limits.h` is GENERATED data (harness/contract-limits-gen.mjs documents it) and is
# not walked.
# must be immediately preceded (blank lines allowed) by a `/** … */` block that carries @brief;
# a function's block carries one @param per parameter, @return unless it returns void, @pre when
# its body evaluates OMX_PRE, @post for OMX_POST, @invariant for OMX_INVARIANT, and @note.
#
# Exit 1 with one line per missing item. `--self-test` runs the checker over a scratch header
# with an undocumented function and a documented one and requires the first to be refused.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"

AWK_PROG='
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function report(line, what) { printf("doc-check: %s:%d: %s\n", FILENAME, line, what); bad++ }
function param_names(sig,   inner, n, parts, i, p, m, k, name) {
  inner = sig; sub(/^[^(]*\(/, "", inner); sub(/\)[^)]*$/, "", inner)
  n = split(inner, parts, ",")
  names = ""
  for (i = 1; i <= n; i++) {
    p = trim(parts[i])
    if (p == "" || p == "void") continue
    sub(/\[[^]]*\]/, "", p); sub(/\[[^]]*\]/, "", p)
    m = split(p, k, /[ \t*]+/)
    name = k[m]
    if (name == "") name = k[m - 1]
    names = names " " name
  }
  return names
}
function check_block(line, kind, sig, body, docok, blk,   want, i, n, w) {
  if (!docok) { report(line, kind " has no /** doc block immediately before it"); return }
  if (blk !~ /@brief/) report(line, kind " doc block carries no @brief")
  if (kind == "function") {
    if (blk !~ /@note/) report(line, "function doc block carries no @note (RT-safety and thread-safety)")
    n = split(param_names(sig), w, " ")
    for (i = 1; i <= n; i++) if (w[i] != "" && blk !~ ("@param[ \t]+" w[i] "([^A-Za-z0-9_]|$)")) report(line, "no @param " w[i])
    rt = sig; sub(/[A-Za-z_][A-Za-z0-9_]*[ \t]*\(.*$/, "", rt)
    if (!(rt ~ /(^|[^A-Za-z0-9_])void([^A-Za-z0-9_]|$)/ && rt !~ /\*/) && blk !~ /@return/) report(line, "non-void function without @return")
    if (body ~ /OMX_PRE/ && blk !~ /@pre/) report(line, "body evaluates OMX_PRE but the doc block has no @pre")
    if (body ~ /OMX_POST/ && blk !~ /@post/) report(line, "body evaluates OMX_POST but the doc block has no @post")
    if (body ~ /OMX_INVARIANT/ && blk !~ /@invariant/) report(line, "body evaluates OMX_INVARIANT but the doc block has no @invariant")
  }
}
BEGIN { bad = 0 }
FNR == 1 { depth = 0; in_doc = 0; in_comment = 0; block = ""; block_end = -1; last_nonblank = -1; delete seen; pending = 0 }
{
  raw = $0
  # doc blocks and plain comments
  if (in_comment) { if (in_doc) block = block "\n" raw; if (raw ~ /\*\//) { in_comment = 0; if (in_doc) { block_end = FNR; in_doc = 0 } } last_nonblank = FNR; next }
  if (raw ~ /^[ \t]*\/\*\*/) { in_doc = 1; block = raw; if (raw ~ /\*\//) { in_doc = 0; block_end = FNR } else in_comment = 1; last_nonblank = FNR; next }
  if (raw ~ /^[ \t]*\/\*/) { if (raw !~ /\*\//) in_comment = 1; last_nonblank = FNR; next }
  code = raw; sub(/\/\/.*$/, "", code); gsub(/"[^"]*"/, "\"\"", code); sub(/\/\*.*\*\//, "", code)
  if (trim(code) == "") { if (trim(raw) != "") last_nonblank = FNR; next }
  # a signature spanning lines: accumulate until { or ;
  if (pending) {
    sig = sig " " trim(code)
    if (code ~ /[{;]/) { pending = 0; body_start = (code ~ /\{/) ? 1 : 0; if (body_start) { collecting = 1; body = ""; bdepth = 0 } else check_block(sig_line, "function", sig, "", sig_docok, sig_block) }
  } else if (depth == 0 && code ~ /^(static[ \t]+inline[ \t]+|)[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*](omx_[a-z0-9_]+|omxdsp_[a-z0-9_]+)[ \t]*\(/ && code !~ /^#/ && code !~ /^typedef/) {
    sig = trim(code); sig_line = FNR; sig_docok = (block_end == last_nonblank); sig_block = block
    if (code ~ /[{;]/) { body_start = (code ~ /\{/) ? 1 : 0; if (body_start) { collecting = 1; body = ""; bdepth = 0 } else check_block(sig_line, "function", sig, "", sig_docok, sig_block) }
    else pending = 1
  } else if (depth == 0 && code ~ /^(struct|enum)[ \t]+omx_[a-z0-9_]+[ \t]*\{/) {
    check_block(FNR, "type", "", "", block_end == last_nonblank, block)
  } else if (depth == 0 && code ~ /^typedef[ \t]+(struct|enum)[ \t]*\{/) {
    check_block(FNR, "type", "", "", block_end == last_nonblank, block)
  } else if (depth == 0 && code ~ /^#[ \t]*define[ \t]+OMX[A-Z0-9_]*/) {
    name = code; sub(/^#[ \t]*define[ \t]+/, "", name); sub(/[^A-Za-z0-9_].*$/, "", name)
    if (!(name in seen) && name !~ /_H$/ && name != "OMX_CONTRACT_STAGE") { seen[name] = 1; check_block(FNR, "macro " name, "", "", block_end == last_nonblank, block) }
  }
  # brace depth over code, and the body of the function being collected
  o = gsub(/\{/, "{", code); c = gsub(/\}/, "}", code)
  if (collecting) {
    body = body "\n" code; bdepth += o - c
    if (bdepth <= 0) { collecting = 0; check_block(sig_line, "function", sig, body, sig_docok, sig_block) }
  }
  depth += o - c
  last_nonblank = FNR
}
END { if (bad) { printf("doc-check: %d missing item(s)\n", bad); exit 1 } }
'

check() { awk "$AWK_PROG" "$@"; }

if [ "${1:-}" = "--self-test" ]; then
  t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
  cat > "$t/bad.h" <<'EOF'
#ifndef BAD_H
#define BAD_H
static inline float omx_bad_undocumented(float x) { return x; }
#endif
EOF
  cat > "$t/good.h" <<'EOF'
#ifndef GOOD_H
#define GOOD_H
/** @brief Identity.
 * @param x A sample.
 * @return `x`.
 * @note RT-safe and thread-safe. */
static inline float omx_good_documented(float x) { return x; }
#endif
EOF
  if check "$t/bad.h" >/dev/null 2>&1; then echo "doc-check --self-test: FAIL — an undocumented function was accepted"; exit 1; fi
  if ! check "$t/good.h"; then echo "doc-check --self-test: FAIL — a documented function was refused"; exit 1; fi
  echo "doc-check --self-test: ok (refuses the undocumented, accepts the documented)"
  exit 0
fi

if [ $# -gt 0 ]; then check "$@"; else
  bash "$0" --self-test
  set -- "$PKG"/include/omxdsp/*.h
  files=(); for f in "$@"; do [ "$(basename "$f")" = omx_contract_limits.h ] || files+=("$f"); done
  check "${files[@]}" && echo "doc-check: every public symbol in include/omxdsp carries its doc comment"
fi
