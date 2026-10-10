#ifndef SERVER_H
#define SERVER_H

#include <signal.h>
#include <stddef.h>

#include "telegram.h"

/* Maximum size of the HTTP request header block (request line + headers). */
#define MAX_HEADER_SIZE 8192

/* Maximum size of an HTTP request body we are willing to buffer. */
#define MAX_HTTP_BODY 16384

/* SO_RCVTIMEO / SO_SNDTIMEO applied to accepted sockets, in seconds. */
#define SOCKET_TIMEOUT_SEC 5

/* listen() backlog depth. */
#define LISTEN_BACKLOG 16

/* The route this service exposes. */
#define NOTIFICATION_PATH "/notifications"

typedef struct {
    unsigned short port;
    telegram_config_t telegram_config;
} server_config_t;

typedef struct {
    int status_code;
    char body[256];
    size_t body_length;
} http_response_t;

/*
 * Set to 0 by the SIGINT/SIGTERM handler in main.c so that accept() returning
 * EINTR unwinds the loop in request_handler_start() and control flows back to
 * main() for curl_global_cleanup().
 */
extern volatile sig_atomic_t g_keep_running;

/*
 * Socket setup and accept loop. Blocks until g_keep_running is cleared.
 * Returns 0 on graceful shutdown, -1 on setup failure.
 */
int request_handler_start(const server_config_t *config);

/*
 * Route a single request and produce the response in `response`.
 *
 * `headers` must be a NUL-terminated header block (the bytes preceding the
 * "\r\n\r\n" terminator). `body` holds the already-framed body bytes.
 */
void request_handler_dispatch(const server_config_t *config,
                              const char *headers, const char *body,
                              size_t body_length, http_response_t *response);

#endif
