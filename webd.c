/* webd -- an HTTP driver.
 *
 * Not "the application". A driver: the one process thick layer that knows how
 * to speak the outside world's protocol. Behind it there is no protocol.
 *
 *   GET /       -> show the count
 *   GET /bump   -> increment, show the count
 */
#include "counter.h"
#include <stdlib.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static counter C;

static void reply(int fd, const char *status, const char *body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", status, strlen(body));
    write(fd, hdr, n);
    write(fd, body, strlen(body));
}

int main(int argc, char **argv) {
    int join = 0, port = 8080;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--join")) join = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) {
            if (counter_open(&C, join, "webd", "http") < 0) return 2;
            uint64_t v = counter_bump(&C);
            printf("webd selftest: mode=%s count=%llu\n", counter_mode(&C),
                   (unsigned long long)v);
            counter_close(&C);
            return 0;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    if (counter_open(&C, join, "webd", "http") < 0) return 2;   /* <- the only mode-aware line */

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    printf("webd: listening on http://127.0.0.1:%d  mode=%s\n", port, counter_mode(&C));
    fflush(stdout);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;

        char buf[2048];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) { close(fd); continue; }
        buf[n] = 0;

        char method[8] = {0}, path[256] = {0};
        sscanf(buf, "%7s %255s", method, path);

        char body[256];
        if (!strcmp(path, "/bump")) {
            uint64_t v = counter_bump(&C);                /* identical call site */
            snprintf(body, sizeof body, "bumped\ncount = %llu\nmode  = %s\n",
                     (unsigned long long)v, counter_mode(&C));
            reply(fd, "200 OK", body);
        } else if (!strcmp(path, "/")) {
            uint64_t v = counter_read(&C);                /* identical call site */
            snprintf(body, sizeof body, "count = %llu\nmode  = %s\n\nGET /bump to increment\n",
                     (unsigned long long)v, counter_mode(&C));
            reply(fd, "200 OK", body);
        } else {
            reply(fd, "404 Not Found", "no\n");
        }
        close(fd);
    }
}
