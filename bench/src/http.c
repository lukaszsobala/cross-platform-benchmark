// A one-shot HTTP POST, so the benchmark can hand its own result to a hub.
//
// Plain HTTP is a socket, a request line and a reply to read back, which is
// small enough to carry here and keeps `cpcpub --submit` working on a machine
// with nothing installed on it. TLS is not: an https:// URL is handed to curl,
// which every system this benchmark targets either ships or can install, and
// which is the only outside program this tree will ever run. When there is no
// curl the failure says so rather than pretending the upload went out.
//
// Everything here is best-effort by construction. The measurement has already
// been made by the time any of it runs, so a refused connection is a message
// to the operator, never a reason to lose a result.

#define _GNU_SOURCE 1

#include "http.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD    INVALID_SOCKET
#define sock_close  closesocket
#define popen       _popen
#define pclose      _pclose
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD    (-1)
#define sock_close  close
#endif

#define CONNECT_SECONDS 10
#define IO_SECONDS      30
#define MAX_REPLY       (256u << 10)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int fail(http_reply_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, ap);
    va_end(ap);
    r->status = 0;
    return -1;
}

// strdup by hand: it is POSIX rather than C, and this file is compiled -std=c2x
// on toolchains that then decline to declare it (mingw-w64 among them).
static char *dup_str(const char *s) {
    const size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static const char *sock_error(void) {
#ifdef _WIN32
    static char buf[64];
    snprintf(buf, sizeof(buf), "winsock error %d", WSAGetLastError());
    return buf;
#else
    return strerror(errno);
#endif
}

void http_free(http_reply_t *r) {
    free(r->body);
    r->body = NULL;
}

// ---------------------------------------------------------------------------
// URL
// ---------------------------------------------------------------------------

typedef struct {
    int  tls;                   // https, which this file cannot speak itself
    char host[256];
    char port[8];
    char target[8192];          // path and query, as the request line wants it
} url_t;

// Split a URL into what a request needs. Deliberately strict: this is fed from
// a command line and its failure mode should be a sentence, not a connection to
// somewhere unintended.
static int url_split(const char *url, url_t *u, http_reply_t *r) {
    memset(u, 0, sizeof(*u));
    const char *p = url;
    // Nothing outside printable ASCII, so a URL cannot carry the CR LF that
    // would end the request line and start a header of the caller's choosing.
    for (const unsigned char *c = (const unsigned char *)url; *c; ++c) {
        if (*c <= 0x20 || *c >= 0x7f)
            return fail(r, "hub URL has a space or a control character in it");
    }
    if (!strncmp(p, "https://", 8)) { u->tls = 1; p += 8; }
    else if (!strncmp(p, "http://", 7)) { p += 7; }
    else return fail(r, "hub URL must start with http:// or https://");

    if (strchr(p, '@')) return fail(r, "hub URL must not carry credentials");

    // Host: either [v6:address] or everything up to :, / or end.
    const char *host_end;
    if (*p == '[') {
        host_end = strchr(p, ']');
        if (!host_end) return fail(r, "unterminated [address] in hub URL");
        ++p;                                    // the brackets are not the host
        if ((size_t)(host_end - p) >= sizeof(u->host))
            return fail(r, "hub address is too long");
        memcpy(u->host, p, (size_t)(host_end - p));
        p = host_end + 1;
    } else {
        host_end = p + strcspn(p, ":/?");
        if ((size_t)(host_end - p) >= sizeof(u->host))
            return fail(r, "hub host name is too long");
        memcpy(u->host, p, (size_t)(host_end - p));
        p = host_end;
    }
    if (!u->host[0]) return fail(r, "hub URL names no host");

    if (*p == ':') {
        ++p;
        const size_t n = strspn(p, "0123456789");
        if (n == 0 || n >= sizeof(u->port)) return fail(r, "bad port in hub URL");
        memcpy(u->port, p, n);
        p += n;
        if (*p && *p != '/' && *p != '?') return fail(r, "bad port in hub URL");
    } else {
        snprintf(u->port, sizeof(u->port), "%s", u->tls ? "443" : "80");
    }

    if (!*p) snprintf(u->target, sizeof(u->target), "/");
    else if (snprintf(u->target, sizeof(u->target), "%s%s",
                      *p == '/' ? "" : "/", p) >= (int)sizeof(u->target))
        return fail(r, "hub URL is too long");
    return 0;
}

// ---------------------------------------------------------------------------
// The socket
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Winsock is not there until it is asked for, and only the first ask does any
// work. Never cleaned up: the process is about to end, and a WSACleanup racing
// another thread's socket would be a bug traded for nothing.
static int winsock_up(http_reply_t *r) {
    static int started = 0;
    if (started) return 0;
    WSADATA wsa;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) return fail(r, "winsock startup failed (%d)", rc);
    started = 1;
    return 0;
}
#endif

static void set_blocking(sock_t s, int on) {
#ifdef _WIN32
    u_long mode = on ? 0 : 1;
    ioctlsocket(s, (long)FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, on ? (flags & ~O_NONBLOCK)
                                         : (flags | O_NONBLOCK));
#endif
}

static void set_timeouts(sock_t s) {
#ifdef _WIN32
    DWORD ms = IO_SECONDS * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv = { .tv_sec = IO_SECONDS, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Connect with a deadline. A blocking connect() to a host that drops packets
// waits out the kernel's own timeout, which is minutes; someone who has just
// mistyped a port should hear about it in ten seconds.
static int connect_deadline(sock_t s, const struct sockaddr *addr,
                            socklen_t len) {
    set_blocking(s, 0);
    int rc = connect(s, addr, len);
    if (rc == 0) { set_blocking(s, 1); return 0; }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;
#else
    if (errno != EINPROGRESS) return -1;
#endif
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = { .tv_sec = CONNECT_SECONDS, .tv_usec = 0 };
#ifdef _WIN32
    rc = select(0, NULL, &w, NULL, &tv);      // Winsock ignores the first argument
#else
    rc = select(s + 1, NULL, &w, NULL, &tv);
#endif
    if (rc <= 0) {
#ifndef _WIN32
        errno = ETIMEDOUT;
#endif
        return -1;
    }
    // Writable can also mean "failed": ask the socket which it was.
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0) {
#ifndef _WIN32
        errno = err;
#endif
        return -1;
    }
    set_blocking(s, 1);
    return 0;
}

static sock_t dial(const url_t *u, http_reply_t *r) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *list = NULL;
    const int rc = getaddrinfo(u->host, u->port, &hints, &list);
    if (rc != 0 || !list) {
        fail(r, "cannot resolve %s: %s", u->host, gai_strerror(rc));
        return SOCK_BAD;
    }
    sock_t s = SOCK_BAD;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_BAD) continue;
        if (connect_deadline(s, a->ai_addr, (socklen_t)a->ai_addrlen) == 0) break;
        sock_close(s);
        s = SOCK_BAD;
    }
    freeaddrinfo(list);
    if (s == SOCK_BAD) fail(r, "cannot reach %s:%s: %s", u->host, u->port,
                            sock_error());
    else set_timeouts(s);
    return s;
}

static int send_all(sock_t s, const char *p, size_t len) {
    while (len > 0) {
        const size_t chunk = len > (1u << 20) ? (1u << 20) : len;
#ifdef _WIN32
        const int n = send(s, p, (int)chunk, 0);
#else
        const ssize_t n = send(s, p, chunk, 0);
#endif
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

// Everything the peer sends until it hangs up, capped. Small by construction:
// a hub's reply to an upload is one line of JSON.
static char *read_all(sock_t s, size_t *len_out, http_reply_t *r) {
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fail(r, "out of memory reading the reply"); return NULL; }
    for (;;) {
        if (len + 1024 > cap) {
            if (cap >= MAX_REPLY) break;        // enough to see what went wrong
            char *bigger = (char *)realloc(buf, cap * 2);
            if (!bigger) { free(buf); fail(r, "out of memory"); return NULL; }
            buf = bigger;
            cap *= 2;
        }
#ifdef _WIN32
        const int n = recv(s, buf + len, (int)(cap - len - 1), 0);
#else
        const ssize_t n = recv(s, buf + len, cap - len - 1, 0);
#endif
        if (n < 0) {
            free(buf);
            fail(r, "reading the reply failed: %s", sock_error());
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

// ---------------------------------------------------------------------------
// The reply
// ---------------------------------------------------------------------------

// A proxy in front of a hub may chunk a reply the hub itself did not. Decoded
// in place: the result is only ever shorter than what arrived.
static void dechunk(char *body) {
    char *in = body, *out = body;
    for (;;) {
        char *end = NULL;
        const unsigned long n = strtoul(in, &end, 16);
        if (!end || end == in) break;
        const char *nl = strstr(end, "\r\n");
        if (!nl) break;
        in = (char *)nl + 2;
        if (n == 0) break;
        if (strlen(in) < n) break;
        memmove(out, in, n);
        out += n;
        in += n;
        if (!strncmp(in, "\r\n", 2)) in += 2;
    }
    *out = '\0';
}

static int parse_reply(char *raw, size_t len, http_reply_t *r) {
    (void)len;
    if (strncmp(raw, "HTTP/1.", 7) != 0)
        return fail(r, "the server did not answer with HTTP");
    const char *sp = strchr(raw, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
    if (r->status == 0) return fail(r, "the server sent no status code");

    char *split = strstr(raw, "\r\n\r\n");
    char *body = split ? split + 4 : raw + strlen(raw);
    if (split) *split = '\0';                   // headers alone, for the scan
    // Case-insensitive enough: the only header we act on is this one, and a
    // proxy spells it one of two ways.
    const int chunked = strstr(raw, "Transfer-Encoding: chunked") != NULL ||
                        strstr(raw, "transfer-encoding: chunked") != NULL;
    if (chunked) dechunk(body);
    r->body = dup_str(body);
    if (!r->body) return fail(r, "out of memory");
    return 0;
}

// ---------------------------------------------------------------------------
// https, by way of curl
// ---------------------------------------------------------------------------

// What may appear in a URL or a token before either is put in a command line.
// Both are checked rather than escaped: quoting rules differ between the shell
// and cmd.exe, and a character set both agree is inert needs no rules at all.
// `$`, backtick, backslash, quotes and whitespace are the ones that are not.
static int shell_safe(const char *s, const char *extra) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        const int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || strchr(extra, (int)*p) != NULL;
        if (!ok) return 0;
    }
    return 1;
}

static int post_via_curl(const char *url, const char *token, const char *body,
                         size_t len, http_reply_t *r) {
    if (!shell_safe(url, "-._~:/?#[]@!&()*+,;=%"))
        return fail(r, "this https URL has characters the curl fallback will "
                       "not pass on; use an http:// hub, or pipe the JSON to "
                       "curl yourself");

    char cmd[9000];
    // Everything curl says goes to stderr, so a --json run's stdout stays one
    // clean document even while an upload is happening on the side.
    const int n = snprintf(cmd, sizeof(cmd),
        "curl -fsS --max-time %d -X POST \"%s\" "
        "-H \"Content-Type: application/json\" %s%s%s--data-binary @- 1>&2",
        IO_SECONDS, url,
        (token && *token) ? "-H \"Authorization: Bearer " : "",
        (token && *token) ? token : "",
        (token && *token) ? "\" " : "");
    if (n < 0 || n >= (int)sizeof(cmd))
        return fail(r, "hub URL is too long for the curl fallback");

    FILE *pipe = popen(cmd, "w");
    if (!pipe) return fail(r, "https needs curl, and it could not be started");
    const size_t wrote = fwrite(body, 1, len, pipe);
    int rc = pclose(pipe);
#ifndef _WIN32
    // POSIX hands back a wait status, not an exit code; Windows hands back the
    // code itself. Reported as curl documents its own numbers -- 7 is "could
    // not connect", 22 is "the server said no" -- so the number can be looked
    // up rather than puzzled over.
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    if (wrote != len || rc != 0) {
        return fail(r, "curl could not upload to %s (curl exit %d; it says why "
                       "on stderr)", url, rc);
    }
    // curl -f has already refused anything that was not a success, and its
    // reply went to stderr where the operator can see it, so there is nothing
    // left to hand back but the fact that it worked.
    r->status = 201;
    r->body = dup_str("");
    return r->body ? 0 : fail(r, "out of memory");
}

// ---------------------------------------------------------------------------
// The one entry point
// ---------------------------------------------------------------------------

int http_post_json(const char *url, const char *token, const char *body,
                   size_t len, http_reply_t *out) {
    memset(out, 0, sizeof(*out));
    // Checked before either transport sees it: in a header a stray CR LF is a
    // second header, and on a command line a space is a second argument.
    if (token && *token && !shell_safe(token, "-._~"))
        return fail(out, "an upload token is letters, digits, '-', '_', '.' "
                         "and '~'; this one is not");

    url_t u;
    if (url_split(url, &u, out) != 0) return -1;
    if (u.tls) return post_via_curl(url, token, body, len, out);

#ifdef _WIN32
    if (winsock_up(out) != 0) return -1;
#endif
    const sock_t s = dial(&u, out);
    if (s == SOCK_BAD) return -1;

    char head[9000];
    const int n = snprintf(head, sizeof(head),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: cpcpub/1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s%s%s"
        "\r\n",
        u.target, u.host, len,
        (token && *token) ? "Authorization: Bearer " : "",
        (token && *token) ? token : "",
        (token && *token) ? "\r\n" : "");
    if (n < 0 || n >= (int)sizeof(head)) {
        sock_close(s);
        return fail(out, "request headers are too long");
    }
    if (send_all(s, head, (size_t)n) != 0 || send_all(s, body, len) != 0) {
        sock_close(s);
        return fail(out, "sending the result failed: %s", sock_error());
    }

    size_t got = 0;
    char *raw = read_all(s, &got, out);
    sock_close(s);
    if (!raw) return -1;
    const int rc = parse_reply(raw, got, out);
    free(raw);
    return rc;
}
