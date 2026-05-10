#!/usr/bin/env bash
# Regenerate the fuzz corpus seeds. Run from the project root or the fuzz/ dir.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)/corpus"
mkdir -p "$DIR"

SOH=$'\x01'

write() { printf '%s' "$2" > "$DIR/$1"; }

# Compute FIX checksum for a body (sum of bytes mod 256, zero-padded to 3).
checksum() {
    python3 -c "import sys; b=sys.stdin.buffer.read(); print(f'{sum(b)%256:03d}')"
}

# NewOrderSingle (D) — buy 100 @ 50000 of symbol 1, GTC limit
body=$'35=D\x0149=42\x0156=EX\x0111=1001\x0155=1\x0154=1\x0138=100\x0144=50000\x0140=2\x0159=1\x01'
header="8=FIX.4.2${SOH}9=${#body}${SOH}"
prechecksum="${header}${body}"
cs=$(printf '%s' "$prechecksum" | checksum)
write new_order_single.fix "${prechecksum}10=${cs}${SOH}"

# OrderCancelRequest (F)
body=$'35=F\x0149=42\x0156=EX\x0141=1001\x0111=1002\x0155=1\x01'
header="8=FIX.4.2${SOH}9=${#body}${SOH}"
prechecksum="${header}${body}"
cs=$(printf '%s' "$prechecksum" | checksum)
write cancel_request.fix "${prechecksum}10=${cs}${SOH}"

# OrderCancelReplaceRequest (G)
body=$'35=G\x0149=42\x0156=EX\x0141=1001\x0111=1003\x0155=1\x0154=1\x0138=200\x0144=49500\x0140=2\x01'
header="8=FIX.4.2${SOH}9=${#body}${SOH}"
prechecksum="${header}${body}"
cs=$(printf '%s' "$prechecksum" | checksum)
write cancel_replace.fix "${prechecksum}10=${cs}${SOH}"

# Edge cases — these intentionally violate the spec; the parser must not crash.
write empty.fix ""
write only_soh.fix "${SOH}"
write only_equals.fix "="
write huge_tag.fix "9999999999999999999999=x${SOH}"
write tag_with_letters.fix "abc=x${SOH}"
write missing_soh.fix "35=D"
write checksum_in_value.fix $'35=D\x0158=10=999\x0110=000\x01'
write truncated_header.fix "8=FIX"

# Two messages glued together (gateway should frame, but parser may see this).
write back_to_back.fix $'35=D\x0111=1\x0135=F\x0141=1\x01'

echo "wrote corpus to $DIR"
