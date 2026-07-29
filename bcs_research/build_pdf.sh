#!/usr/bin/env bash
# Regenerate bcs_paper_draft.pdf from bcs_paper_draft.md.
#
# Run from the repo root:
#   bash bcs_research/build_pdf.sh
set -euo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/paper" && pwd)"
SRC="bcs_paper_draft.md"
OUT="bcs_paper_draft.pdf"
ENGINE="tectonic"

command -v pandoc >/dev/null 2>&1 || {
  echo "error: pandoc not found — brew install pandoc" >&2
  exit 1
}
command -v "$ENGINE" >/dev/null 2>&1 || {
  echo "error: $ENGINE not found — brew install tectonic" >&2
  exit 1
}

# Build from the paper directory so the ../results/figures/*.png paths written in
# the markdown resolve as-is: pandoc resolves relative image paths against the
# working directory, not against the input file.
cd "$PAPER_DIR"

# Figures are embedded into the PDF by the LaTeX engine as a matter of course —
# a PDF cannot reference an external image file. pandoc's --embed-resources is an
# HTML-only flag (it inlines assets as data: URIs) with no PDF equivalent.
#
# --number-sections is deliberately NOT set: every heading in the source is
# already hand-numbered ("## 1. Introduction", "### 4.4 Experiment 4"), so
# enabling it renders "1 1. Introduction". Strip the manual numbers first if you
# want pandoc to own section numbering.
# Latin Modern has no glyph for these six characters and XeTeX drops them
# silently — "≥ 5" would render as " 5". Map them to their math equivalents
# rather than switching to a Unicode text font, which would change the paper's
# typeface. Re-check with:
#   bash bcs_research/build_pdf.sh 2>&1 | grep -c 'could not represent'
pandoc "$SRC" -o "$OUT" \
  --pdf-engine="$ENGINE" \
  -V documentclass=article \
  -V papersize=a4 \
  -V geometry:a4paper \
  -V geometry:margin=2.5cm \
  -V fontsize=11pt \
  -V colorlinks=true \
  -V linkcolor=blue \
  -V header-includes='\usepackage{newunicodechar}' \
  -V header-includes='\newunicodechar{≈}{\ensuremath{\approx}}' \
  -V header-includes='\newunicodechar{≥}{\ensuremath{\geq}}' \
  -V header-includes='\newunicodechar{≤}{\ensuremath{\leq}}' \
  -V header-includes='\newunicodechar{σ}{\ensuremath{\sigma}}' \
  -V header-includes='\newunicodechar{λ}{\ensuremath{\lambda}}' \
  -V header-includes='\newunicodechar{⁻}{\ensuremath{^{-}}}'

echo "wrote $PAPER_DIR/$OUT ($(wc -c <"$OUT" | tr -d ' ') bytes)"
