#include "agentty/io/tls.hpp"
#include "agentty/util/env.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if defined(__APPLE__)
#  include <Security/Security.h>
#  include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
// Declared before windows.h to quiet min/max + cert cruft.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "crypt32.lib")
#  endif
#endif

namespace agentty::tls {

namespace {

// ALPN advertisement: h2 first, http/1.1 as fallback. Anthropic's edge
// always negotiates h2, but advertising 1.1 keeps us compatible with any
// proxy a user might stand up (corporate TLS-intercepting middleboxes).
// Wire format: length-prefixed protocol IDs per RFC 7301.
constexpr unsigned char kAlpn[] = {
    2, 'h', '2',
    8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

// --------------------------------------------------------------------------
// Client-side TLS session resumption.
//
// SSL_SESS_CACHE_CLIENT enables OpenSSL's internal client cache, but on the
// client side OpenSSL does NOT auto-attach a cached session on SSL_new — you
// must call SSL_set_session() yourself before the handshake, and you must
// CAPTURE the session OpenSSL hands you (TLS 1.3 tickets arrive in
// post-handshake NewSessionTicket frames) via the new-session callback.
// Without both halves, every dial pays a full handshake — resumption never
// fires. We keep one most-recent SSL_SESSION per SNI host in a tiny
// process-wide map; a single-endpoint client (api.anthropic.com) only ever
// has a couple of distinct hosts, so a flat map under a mutex is ample.
//
// Resumption to api.anthropic.com is a 1-RTT (TLS 1.3) abbreviated handshake
// instead of the full 2-RTT exchange — ~30-150 ms saved on every fresh dial
// after the connection-pool idle TTL evicts the live socket.
// --------------------------------------------------------------------------
std::mutex&                                          sess_mu() {
    static std::mutex m; return m;
}
std::unordered_map<std::string, SSL_SESSION*>&       sess_map() {
    static auto* m = new std::unordered_map<std::string, SSL_SESSION*>{};
    return *m;
}

// ex_data slot carrying the SNI host string pointer through to new_session_cb
// (which only gets the SSL*, and SSL_get_servername is server-side only).
int sni_ex_index() {
    static int idx = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return idx;
}

// Called by OpenSSL when a usable session/ticket is established. Takes
// ownership of `sess` (return 1) and stashes it as the latest for this host,
// freeing any prior one. Thread-safe.
int new_session_cb(SSL* ssl, SSL_SESSION* sess) {
    const auto* host = static_cast<const std::string*>(
        SSL_get_ex_data(ssl, sni_ex_index()));
    if (!host) return 0;   // not ours to keep — let OpenSSL free it
    std::lock_guard<std::mutex> lk(sess_mu());
    auto& m = sess_map();
    auto it = m.find(*host);
    if (it != m.end() && it->second) SSL_SESSION_free(it->second);
    m[*host] = sess;       // we now own one ref (cb gives us a ref to keep)
    return 1;
}

// --------------------------------------------------------------------------
// Platform root store loaders. We load on top of OpenSSL's default paths
// (which already cover the common Linux cases) so the final X509_STORE is
// the union — belt and suspenders.
// --------------------------------------------------------------------------

#if defined(__APPLE__)
// macOS: enumerate the admin + system keychain trust settings.  Any cert
// marked with kSecTrustSettingsResultTrustRoot or TrustAsRoot goes into
// OpenSSL's store.  Not all roots have explicit trust settings (the system
// ones use implicit trust from /System/Library/Keychains/SystemRootCertificates),
// so we also pull that keychain directly.
bool load_system_roots(SSL_CTX* ctx) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;

    auto add_cfdata = [&](CFDataRef data) {
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(CFDataGetBytePtr(data));
        long len = static_cast<long>(CFDataGetLength(data));
        X509* x = d2i_X509(nullptr, &bytes, len);
        if (x) {
            // Ignore the success/failure — duplicates are benign and a single
            // bad cert shouldn't nuke the whole store.
            X509_STORE_add_cert(store, x);
            X509_free(x);
        }
    };

    auto copy_from_domain = [&](SecTrustSettingsDomain domain) {
        CFArrayRef certs = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certs) != errSecSuccess || !certs)
            return;
        CFIndex n = CFArrayGetCount(certs);
        for (CFIndex i = 0; i < n; ++i) {
            SecCertificateRef cert =
                static_cast<SecCertificateRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
            if (CFDataRef data = SecCertificateCopyData(cert)) {
                add_cfdata(data);
                CFRelease(data);
            }
        }
        CFRelease(certs);
    };

    copy_from_domain(kSecTrustSettingsDomainSystem);
    copy_from_domain(kSecTrustSettingsDomainAdmin);
    copy_from_domain(kSecTrustSettingsDomainUser);
    return true;
}

#elif defined(_WIN32)
// Windows: pull every cert out of the system ROOT store and add to OpenSSL.
// We go through CertEnumCertificatesInStore instead of the legacy A-variant
// so unicode cert names don't trip us up.
bool load_system_roots(SSL_CTX* ctx) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;

    HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
    if (!sys) return false;

    PCCERT_CONTEXT ctxt = nullptr;
    while ((ctxt = CertEnumCertificatesInStore(sys, ctxt)) != nullptr) {
        const unsigned char* enc = ctxt->pbCertEncoded;
        long n = static_cast<long>(ctxt->cbCertEncoded);
        X509* x = d2i_X509(nullptr, &enc, n);
        if (x) {
            X509_STORE_add_cert(store, x);
            X509_free(x);
        }
    }
    CertCloseStore(sys, 0);
    return true;
}

#else
// Linux / BSD: OpenSSL's default paths already cover this.  Some distros
// ship the trusted CA bundle under odd paths (Alpine: /etc/ssl/cert.pem,
// some container bases: /etc/pki/tls/certs/ca-bundle.crt); SSL_CTX_set_default_verify_paths
// handles the usual ones.  We additionally honor the two env vars that
// curl conventionally reads, so a user with a corporate proxy cert can
// point us at it without code changes.
bool load_system_roots(SSL_CTX* ctx) {
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        // Not fatal: env overrides still get a shot below.
    }
    if (const char* f = std::getenv("SSL_CERT_FILE"); f && *f) {
        SSL_CTX_load_verify_locations(ctx, f, nullptr);
    }
    if (const char* d = std::getenv("SSL_CERT_DIR"); d && *d) {
        SSL_CTX_load_verify_locations(ctx, nullptr, d);
    }
    if (const char* f = std::getenv("CURL_CA_BUNDLE"); f && *f) {
        SSL_CTX_load_verify_locations(ctx, f, nullptr);
    }
    return true;
}
#endif

// --------------------------------------------------------------------------
// One-time SSL_CTX build. Two contexts: one with peer verification, one
// insecure (for AGENTTY_INSECURE=1 / --insecure).  Lazy-initialized under a
// mutex; the first caller pays the setup cost, the rest see the cached ptr.
// Leaked at process exit — SSL_CTX_free would race any in-flight SSL that
// outlives main's cleanup.
// --------------------------------------------------------------------------
SSL_CTX* build_ctx(bool insecure) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                     nullptr);
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;

    // TLS 1.2 minimum — anything older is a compliance red flag and Anthropic
    // won't negotiate it anyway.  TLS 1.3 is preferred (the default) and
    // gives us 1-RTT handshakes, 0-RTT resumption via session tickets.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // Session cache + tickets. This is the fat perf win: a fresh dial after
    // the connection-pool TTL (90 s) skips the full TLS handshake and does
    // a 1-RTT resume — saving one RTT to api.anthropic.com (~30-150 ms).
    //
    // SSL_SESS_CACHE_CLIENT enables the cache mode, but on the CLIENT side
    // OpenSSL neither auto-stores the ticket nor auto-attaches it on the next
    // SSL_new. Both halves are wired explicitly: new_session_cb captures each
    // NewSessionTicket per SNI host (below), and wrap_client calls
    // SSL_set_session() with the saved one before connecting. Relying on the
    // cache mode alone — as the prior comment here claimed — silently does a
    // full handshake every time.
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    // Capture TLS 1.3 NewSessionTicket sessions ourselves — the internal
    // cache alone never gets consulted on the client side (see new_session_cb).
    SSL_CTX_sess_set_new_cb(ctx, new_session_cb);
    // I/O mode flags, essential for our pump-send loop:
    //   ENABLE_PARTIAL_WRITE    — SSL_write may return a positive value less
    //                             than the request; callers handle partial
    //                             progress (we already do via `off += put`).
    //   ACCEPT_MOVING_WRITE_BUF — after WANT_READ/WANT_WRITE, caller may
    //                             retry with a *different* buffer pointer and
    //                             length. Required because nghttp2's
    //                             `mem_send` returns a fresh pointer/len on
    //                             every call — without this flag OpenSSL
    //                             raises SSL_R_BAD_LENGTH (0x0A00010F) on
    //                             the retry, which is exactly the failure
    //                             the Windows build hit at SSL_write on the
    //                             first POST after a TLS resume.
    //   AUTO_RETRY              — transparent handling of peer-initiated
    //                             renegotiation / post-handshake tickets;
    //                             on by default in OpenSSL 1.1.1+, set
    //                             explicitly for clarity and portability.
    SSL_CTX_set_mode(ctx,
                     SSL_MODE_ENABLE_PARTIAL_WRITE
                     | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
                     | SSL_MODE_AUTO_RETRY);
    // Advertise ALPN so the server negotiates h2 on our behalf.
    SSL_CTX_set_alpn_protos(ctx, kAlpn, sizeof(kAlpn));

    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        load_system_roots(ctx);
    }
    return ctx;
}

struct SharedCtx {
    SSL_CTX* verifying = nullptr;
    SSL_CTX* insecure  = nullptr;
};

SharedCtx& shared() {
    // Deliberate leak — process lifetime, avoids destruction order races.
    static SharedCtx* s = new SharedCtx{};
    static std::once_flag once;
    std::call_once(once, [] {
        s->verifying = build_ctx(false);
        // Env gate: AGENTTY_INSECURE=1 collapses both handles into the insecure
        // one, so a code path that asks for the "verified" handle still
        // follows the user's explicit opt-out.
        //
        // ── AGENTTY_INSECURE lifecycle (read this before exporting the var) ──
        //
        // The env var is sampled exactly ONCE — here, on the first call to
        // shared() — and cached for the rest of the process via call_once.
        // Effects of that:
        //
        //   • In a CLI run (the only intended use): set AGENTTY_INSECURE=1
        //     before launching `agentty`, the first TLS handshake reads it
        //     and both contexts become insecure. Working as intended.
        //
        //   • A mid-process setenv("AGENTTY_INSECURE", "1", 1) is silently
        //     ignored — we already built the verifying context. Same for
        //     unsetenv(). This breaks the obvious test pattern:
        //
        //       setenv("AGENTTY_INSECURE", "1", 1);
        //       run_request();           // verifies (already cached)
        //       unsetenv("AGENTTY_INSECURE");
        //
        //     If you need that flow, set the var BEFORE the first http
        //     request of the process — typically in main() or a test
        //     fixture's SetUp(), not between assertions.
        //
        //   • If agentty is ever embedded as a library, callers can't
        //     change verification posture after the first SSL handshake
        //     short of restarting the process. Document this at the
        //     embedding seam if you go that route.
        //
        // Why one-shot rather than re-reading per shared_context() call:
        // SSL_CTX is a heavyweight object (loads the system CA bundle,
        // verifies the trust store) and connections already established
        // against the cached pointer wouldn't migrate to a freshly-built
        // one anyway, so a "live" knob would lie about its scope.
        if (const char* e = util::env::get_or_null<util::env::Var::Insecure>();
            e && *e == '1') {
            s->insecure  = build_ctx(true);
            s->verifying = s->insecure;
        } else {
            s->insecure = build_ctx(true);
        }
    });
    return *s;
}

} // namespace

SSL_CTX* shared_context(bool insecure) {
    auto& s = shared();
    return insecure ? s.insecure : s.verifying;
}

SSL* wrap_client(int fd, std::string_view sni_host) {
    SSL_CTX* ctx = shared_context(false);
    if (!ctx) return nullptr;
    SSL* ssl = SSL_new(ctx);
    if (!ssl) return nullptr;

    // RAII scaffold for the partially-built SSL: until we commit() at the
    // end, any early return frees both the SSL and the owned SNI host key
    // we stash in ex_data. Previously the SSL_set_fd failure path had to
    // hand-unwind `delete host_key; SSL_free(ssl)`; a future early-return
    // added above it would have leaked one or both. The guard makes the
    // half-built handle a move-only resource that cleans itself up.
    struct Building {
        SSL*         ssl  = nullptr;
        std::string* host = nullptr;   // owned ex_data key, freed unless committed
        int          ex   = -1;
        bool         live = true;
        Building(SSL* s, int ex_idx) : ssl(s), ex(ex_idx) {}
        Building(const Building&)            = delete;
        Building& operator=(const Building&) = delete;
        ~Building() {
            if (!live) return;
            if (host) { if (ex >= 0) SSL_set_ex_data(ssl, ex, nullptr); delete host; }
            if (ssl)  SSL_free(ssl);
        }
        SSL* commit() noexcept { live = false; return ssl; }   // ownership escapes
    } building{ssl, sni_ex_index()};

    // SNI: required for virtual-hosted edges (Anthropic's Cloudflare front).
    // null-terminate via a local string — string_view has no guarantee.
    std::string host{sni_host};
    SSL_set_tlsext_host_name(ssl, host.c_str());
    // Hostname verification — checks CN/SAN against the cert, on top of
    // chain verification that SSL_CTX already configured.
    SSL_set1_host(ssl, host.c_str());
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

    // Resumption: attach the most recent ticket for this host (if any) so the
    // upcoming handshake is a 1-RTT resume. Also stash an owned copy of the
    // host in ex_data so new_session_cb knows which key to file the fresh
    // ticket under (it fires post-handshake, only sees the SSL*). The string
    // is freed in free_ssl on the happy path, or by ~Building on failure.
    building.host = new std::string{host};
    SSL_set_ex_data(ssl, sni_ex_index(), building.host);
    {
        std::lock_guard<std::mutex> lk(sess_mu());
        auto& m = sess_map();
        if (auto it = m.find(host); it != m.end() && it->second)
            SSL_set_session(ssl, it->second);
    }

    if (SSL_set_fd(ssl, fd) != 1) {
        return nullptr;   // ~Building frees host key + SSL
    }
    SSL_set_connect_state(ssl);
    return building.commit();
}

void free_ssl(SSL* ssl) noexcept {
    if (!ssl) return;
    // Free the owned SNI host key we stashed in ex_data (wrap_client).
    if (auto* host_key = static_cast<std::string*>(
            SSL_get_ex_data(ssl, sni_ex_index()))) {
        SSL_set_ex_data(ssl, sni_ex_index(), nullptr);
        delete host_key;
    }
    // Best-effort bidirectional shutdown.  If the peer hasn't ACK'd we don't
    // block on it — the fd is about to close anyway.
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
}

std::string last_error(SSL* ssl) {
    std::string out;
    if (ssl) {
        int code = SSL_get_error(ssl, -1);
        if (code == SSL_ERROR_SYSCALL) {
            // The actual reason is often on the err queue; fall through.
            out = "syscall";
        } else if (code == SSL_ERROR_ZERO_RETURN) {
            out = "peer closed (SSL_ERROR_ZERO_RETURN)";
        }
    }
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) out += "; ";
        out += buf;
    }
    if (out.empty()) out = "unknown openssl error";
    return out;
}

} // namespace agentty::tls
