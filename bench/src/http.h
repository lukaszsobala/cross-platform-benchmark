// The smallest HTTP client that can hand a result to a hub.
//
// One call, one POST, one reply. No TLS: a plain-HTTP request is a socket and
// a hundred lines, and a TLS one is a dependency the rest of this benchmark
// does not have. An https:// URL is therefore handed to curl if the machine
// has one -- see http.c.

#ifndef CPCPUB_HTTP_H
#define CPCPUB_HTTP_H

#include <stddef.h>

typedef struct {
    int   status;           // HTTP status, or 0 when no reply was read
    char *body;             // reply body, NUL-terminated; free with http_free
    char  error[256];       // why there was no reply, when status is 0
} http_reply_t;

// POST `body` as application/json to `url`. `token`, when non-empty, goes in an
// Authorization: Bearer header. Returns 0 when the server answered (`status`
// then says what it answered), -1 when it could not be reached (`error` says
// why). Never longjmps, never exits: a failed upload must not lose the
// measurement that was just made.
int http_post_json(const char *url, const char *token,
                   const char *body, size_t len, http_reply_t *out);

void http_free(http_reply_t *r);

#endif  // CPCPUB_HTTP_H
