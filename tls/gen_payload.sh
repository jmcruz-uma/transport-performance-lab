#!/usr/bin/env bash
# Deterministically (re)generates a transfer payload under files/ from a
# fixed key, so every machine transfers byte-identical data and "reproducible"
# means "check the hash". The bytes are the AES-256-CTR keystream over zeros --
# deterministic, fast, and openssl is already a dependency. Content is
# measurement-neutral (TLS 1.3 has no record compression).
#
#   tls/gen_payload.sh [size_bytes] [name]   default 104857600 (100 MiB), 100MB.bin
#
# Every payload is a prefix of the same keystream, so a smaller one is exactly
# the first bytes of the default one (files/10MB.bin: the D7 sweep's payload).
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE="${1:-104857600}"
NAME="${2:-100MB.bin}"
OUT="$ROOT/files/$NAME"
SUM="$OUT.sha256"

# Fixed key / IV -> deterministic keystream. Do not change (would change the hash).
KEY="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
IV="0123456789abcdef0123456789abcdef"

mkdir -p "$ROOT/files"

if [ -f "$OUT" ] && [ -f "$SUM" ] && [ "$(stat -c %s "$OUT")" = "$SIZE" ] \
   && ( cd "$ROOT/files" && sha256sum -c "$(basename "$SUM")" ) >/dev/null 2>&1; then
    echo "tls/gen_payload.sh: files/$NAME present and matches its hash"
    exit 0
fi

echo "tls/gen_payload.sh: generating $SIZE-byte deterministic payload"
head -c "$SIZE" /dev/zero \
    | openssl enc -aes-256-ctr -K "$KEY" -iv "$IV" -nosalt > "$OUT"

( cd "$ROOT/files" && sha256sum "$(basename "$OUT")" > "$(basename "$SUM")" )
echo "tls/gen_payload.sh: $(cat "$SUM")"
