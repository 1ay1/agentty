// SSRF resolved-IP guard — unit tests for agentty::http::ssrf_ip_blocked.
//
// The web tools (web_fetch / web_search) dial arbitrary model-supplied URLs.
// mcp-cpp's name-based host_blocked() rejects obvious internal *names*, but a
// public hostname can DNS-rebind to an internal IP the name check never sees.
// ssrf_ip_blocked() is the authoritative backstop: it runs on the RESOLVED
// address the dialer is about to connect() to. This pins the ranges it must
// refuse (and the public ones it must allow) so a future edit can't quietly
// open a hole.
#undef NDEBUG
#include "agtest.hpp"

#include "agentty/io/http.hpp"

using agentty::http::ssrf_ip_blocked;

TEST_CASE("ssrf: loopback + unspecified are blocked") {
    CHECK(ssrf_ip_blocked("127.0.0.1"));
    CHECK(ssrf_ip_blocked("127.1.2.3"));     // whole 127/8
    CHECK(ssrf_ip_blocked("0.0.0.0"));       // 0/8 "this host"
    CHECK(ssrf_ip_blocked("::1"));           // v6 loopback
    CHECK(ssrf_ip_blocked("::"));            // v6 unspecified
    CHECK(ssrf_ip_blocked("[::1]"));         // bracketed literal
}

TEST_CASE("ssrf: cloud metadata + link-local are blocked") {
    CHECK(ssrf_ip_blocked("169.254.169.254"));   // the AWS/GCP/Azure metadata IP
    CHECK(ssrf_ip_blocked("169.254.0.1"));       // whole 169.254/16 link-local
    CHECK(ssrf_ip_blocked("fe80::1"));           // v6 link-local
    CHECK(ssrf_ip_blocked("[fe80::abcd]"));
}

TEST_CASE("ssrf: RFC1918 private ranges are blocked") {
    CHECK(ssrf_ip_blocked("10.0.0.1"));          // 10/8
    CHECK(ssrf_ip_blocked("10.255.255.255"));
    CHECK(ssrf_ip_blocked("172.16.0.1"));        // 172.16/12 lower edge
    CHECK(ssrf_ip_blocked("172.31.255.254"));    // 172.16/12 upper edge
    CHECK(ssrf_ip_blocked("192.168.1.1"));       // 192.168/16
}

TEST_CASE("ssrf: CGNAT, ULA, multicast are blocked") {
    CHECK(ssrf_ip_blocked("100.64.0.1"));        // 100.64/10 CGNAT
    CHECK(ssrf_ip_blocked("100.127.255.255"));
    CHECK(ssrf_ip_blocked("fc00::1"));           // fc00::/7 ULA
    CHECK(ssrf_ip_blocked("fd12:3456::1"));
    CHECK(ssrf_ip_blocked("224.0.0.1"));         // multicast / reserved
    CHECK(ssrf_ip_blocked("ff02::1"));           // v6 multicast
}

TEST_CASE("ssrf: IPv4-mapped IPv6 is re-checked as its v4 address") {
    // The bypass a naive "starts with fc/fe80/::1" v6 filter misses.
    CHECK(ssrf_ip_blocked("::ffff:127.0.0.1"));
    CHECK(ssrf_ip_blocked("::ffff:169.254.169.254"));
    CHECK(ssrf_ip_blocked("::ffff:10.0.0.1"));
    CHECK(ssrf_ip_blocked("[::ffff:192.168.0.1]"));
    // ...and a mapped PUBLIC address must still be allowed.
    CHECK(!ssrf_ip_blocked("::ffff:8.8.8.8"));
}

TEST_CASE("ssrf: public addresses are allowed") {
    CHECK(!ssrf_ip_blocked("8.8.8.8"));          // Google DNS
    CHECK(!ssrf_ip_blocked("1.1.1.1"));          // Cloudflare
    CHECK(!ssrf_ip_blocked("140.82.121.4"));     // github.com-ish
    CHECK(!ssrf_ip_blocked("172.15.0.1"));       // just BELOW 172.16/12
    CHECK(!ssrf_ip_blocked("172.32.0.1"));       // just ABOVE 172.16/12
    CHECK(!ssrf_ip_blocked("100.63.255.255"));   // just below CGNAT
    CHECK(!ssrf_ip_blocked("100.128.0.0"));      // just above CGNAT
    CHECK(!ssrf_ip_blocked("2606:4700:4700::1111")); // Cloudflare v6
}

TEST_CASE("ssrf: unparseable / non-numeric input fails closed") {
    CHECK(ssrf_ip_blocked(""));                  // empty
    CHECK(ssrf_ip_blocked("example.com"));       // a NAME, not an IP
    CHECK(ssrf_ip_blocked("not-an-ip"));
    CHECK(ssrf_ip_blocked("999.999.999.999"));   // out of range octets
    CHECK(ssrf_ip_blocked("[]"));
}
