// Shared TLS configuration for every arm of the `tls` / `tls-framed` experiments.
//
// The point of this header is comparability: BSD, Asio and corosio all configure
// their SSL_CTX through the SAME two functions below, so the only thing that
// differs between arms is *how the application reaches OpenSSL* (raw C API vs a
// stream wrapper vs the TAPS SecurityParameters surface) -- which is exactly the
// variable the experiment measures. Everything below the API boundary is
// byte-identical: TLS 1.3 only, one cipher suite, one group, one ALPN id, the
// same certificate chain.
//
// The TAPS arm does not include this header's functions (it goes through
// taps::SecurityParameters) but MUST set the same constants; they are duplicated
// there deliberately and checked at run time via print_tls_identity().

#pragma once

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace tlscommon {

// ---- pinned parameters (the single source of truth) -------------------------
inline constexpr const char* kCipherSuite = "TLS_AES_128_GCM_SHA256";  // TLS 1.3
inline constexpr const char* kGroup       = "X25519";
inline constexpr const char* kAlpn        = "taps-bench/1";
inline constexpr const char* kServerName  = "localhost";               // SNI + cert identity
inline constexpr int         kMinVersion  = TLS1_3_VERSION;
inline constexpr int         kMaxVersion  = TLS1_3_VERSION;

// ---- ALPN wire helpers ------------------------------------------------------
// Length-prefixed single-protocol list for the one id we use.
inline std::string alpn_wire() {
    std::string w;
    w.push_back(static_cast<char>(std::strlen(kAlpn)));
    w += kAlpn;
    return w;
}

// Server-side ALPN selection: accept only kAlpn.
inline int alpn_select_cb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void* /*arg*/) {
    static const std::string pref = alpn_wire();
    unsigned char* chosen = nullptr;
    if (SSL_select_next_proto(&chosen, outlen,
                              reinterpret_cast<const unsigned char*>(pref.data()),
                              static_cast<unsigned int>(pref.size()),
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = chosen;
    return SSL_TLSEXT_ERR_OK;
}

// ---- context configuration (raw OpenSSL, used by every non-TAPS arm) --------
// Returns true on success; on failure prints to stderr and returns false.

inline bool apply_common(SSL_CTX* ctx) {
    if (SSL_CTX_set_min_proto_version(ctx, kMinVersion) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, kMaxVersion) != 1) {
        std::fprintf(stderr, "tls: cannot pin TLS 1.3\n");
        return false;
    }
    if (SSL_CTX_set_ciphersuites(ctx, kCipherSuite) != 1) {
        std::fprintf(stderr, "tls: unknown cipher suite %s\n", kCipherSuite);
        return false;
    }
    if (SSL_CTX_set1_groups_list(ctx, kGroup) != 1) {
        std::fprintf(stderr, "tls: unknown group %s\n", kGroup);
        return false;
    }
    // KTLS is left OFF everywhere: SSL_OP_ENABLE_KTLS is deliberately not set.
    return true;
}

inline bool configure_client_ctx(SSL_CTX* ctx, const std::string& ca_file) {
    if (!apply_common(ctx)) return false;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) != 1) {
        std::fprintf(stderr, "tls: cannot load CA %s\n", ca_file.c_str());
        return false;
    }
    const std::string w = alpn_wire();
    if (SSL_CTX_set_alpn_protos(ctx, reinterpret_cast<const unsigned char*>(w.data()),
                                static_cast<unsigned int>(w.size())) != 0) {
        std::fprintf(stderr, "tls: cannot set ALPN\n");
        return false;
    }
    return true;
}

inline bool configure_server_ctx(SSL_CTX* ctx, const std::string& cert_file,
                                 const std::string& key_file) {
    if (!apply_common(ctx)) return false;
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file.c_str()) != 1) {
        std::fprintf(stderr, "tls: cannot load cert %s\n", cert_file.c_str());
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        std::fprintf(stderr, "tls: cannot load key %s\n", key_file.c_str());
        return false;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        std::fprintf(stderr, "tls: cert/key mismatch\n");
        return false;
    }
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, nullptr);
    return true;
}

// ---- run-time identity dump (the harness asserts these match across arms) ----
inline void print_tls_identity(SSL* ssl, const char* who) {
    const unsigned char* proto = nullptr;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(ssl, &proto, &plen);
    std::fprintf(stderr,
                 "TLS_IDENTITY who=%s openssl=\"%s\" version=%s cipher=%s alpn=%.*s\n",
                 who, OpenSSL_version(OPENSSL_VERSION), SSL_get_version(ssl),
                 SSL_get_cipher_name(ssl), static_cast<int>(plen),
                 proto ? reinterpret_cast<const char*>(proto) : "");
}

// ---- env helpers ----------------------------------------------------------
// Cert/CA/key paths come from the environment (the harness fixes the positional
// server args). Falls back to the repo-relative default under tls/.
inline std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

}  // namespace tlscommon
