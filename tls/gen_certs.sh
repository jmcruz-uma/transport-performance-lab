#!/usr/bin/env bash
# Generates the shared TLS material for the tls / tls-framed experiments:
#   tls/ca.crt tls/ca.key   -- throwaway test CA
#   tls/server.crt tls/server.key -- server leaf, EC P-256, SAN DNS:localhost
# Every arm's server loads server.{crt,key}; every arm's client pins ca.crt.
# Idempotent: regenerates only when a file is missing.
set -eu

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if [ -f ca.crt ] && [ -f server.crt ] && [ -f server.key ]; then
    echo "tls/gen_certs.sh: certificates already present"
    exit 0
fi

echo "tls/gen_certs.sh: generating test CA + server certificate"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout ca.key -out ca.crt -days 30 -subj "/CN=transport-lab-test-ca" \
    -addext "basicConstraints=critical,CA:TRUE" 2>/dev/null

openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout server.key -out server.csr -subj "/CN=localhost" 2>/dev/null

openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days 30 \
    -extfile <(printf "subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE") 2>/dev/null

rm -f server.csr ca.srl
echo "tls/gen_certs.sh: done"
