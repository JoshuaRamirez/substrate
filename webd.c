/* webd -- an HTTP driver.
 *
 * Not "the application". A driver: the one process thick layer that knows how
 * to speak the outside world's protocol. Behind it there is no protocol.
 *
 *   GET /           -> an HTML page that polls /api
 *   GET /api        -> JSON: the count, this peer's mode, the peer table
 *   GET /api/bump   -> increment, return JSON
 *   GET /bump       -> increment, plain text (curl-friendly)
 *   GET /status     -> plain text
 *
 * It holds no state. Kill it and start a new one and nothing is lost, because
 * the state was never here -- it is in the owner's page.
 */
#include "counter.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

static counter C;

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><title>counter</title>"
"<style>"
"body{background:#171717;color:#eee;font:13px ui-monospace,Menlo,monospace;"
"margin:0;padding:34px 30px;-webkit-font-smoothing:antialiased}"
"h1{font:600 13px system-ui;color:#9e9e9e;letter-spacing:.09em;margin:0}"
"#sub{color:#666;font-size:10px;margin:5px 0 26px}"
"#n{font:700 62px ui-monospace,Menlo,monospace;line-height:1;margin:0}"
"#lbl{color:#666;font-size:10px;margin:6px 0 26px}"
"button{background:#3485eb;color:#fff;border:0;border-radius:7px;"
"padding:11px 26px;font:600 14px system-ui;cursor:pointer}"
"button:active{background:#2a6ec4}"
"table{border-collapse:collapse;margin-top:30px;width:100%;max-width:560px}"
"th{text-align:left;color:#616161;font:400 10px ui-monospace,Menlo,monospace;"
"padding:0 16px 7px 0;border-bottom:1px solid #333}"
"td{padding:9px 16px 9px 0;color:#8d8d8d;border-bottom:1px solid #242424}"
"td.nm{color:#e8e8e8}"
".dot{color:#34cc66}"
"#ft{color:#484848;font-size:10px;margin-top:26px}"
"</style>"
"<h1>PIPELINE</h1><div id=sub></div>"
"<div id=n>-</div><div id=lbl>count</div>"
"<button onclick='bump()'>bump</button>"
"<table><thead><tr><th></th><th>NAME</th><th>ROLE</th><th>PID</th>"
"<th>OPS</th></tr></thead><tbody id=rows></tbody></table>"
"<div id=ft>this page polls /api. the native windows poll nothing -- "
"they read the same page of memory.</div>"
"<script>"
"async function tick(){"
" try{const r=await fetch('/api',{cache:'no-store'});const d=await r.json();"
"  document.getElementById('n').textContent=d.count;"
"  document.getElementById('sub').textContent=d.shm+'  format v'+d.version+"
"   '  mode '+d.mode;"
"  document.getElementById('rows').innerHTML=d.peers.map(p=>"
"   '<tr><td class=dot>&bull;</td><td class=nm>'+p.name+'</td><td>'+p.role+"
"   '</td><td>'+p.pid+'</td><td>'+p.ops+'</td></tr>').join('');"
" }catch(e){document.getElementById('sub').textContent='webd unreachable';}"
"}"
"async function bump(){await fetch('/api/bump',{cache:'no-store'});tick();}"
"tick();setInterval(tick,300);"
"</script>";

static void reply(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", status, ctype, strlen(body));
    write(fd, hdr, n);
    write(fd, body, strlen(body));
}

/* The peer table, rendered for the browser. Same rows `top` draws. */
static void json_state(char *out, size_t cap) {
    size_t k = 0;
    k += snprintf(out + k, cap - k,
        "{\"count\":%llu,\"mode\":\"%s\",\"shm\":\"%s\",\"version\":%u,\"peers\":[",
        (unsigned long long)counter_read(&C), counter_mode(&C),
        C.joined ? CNT_SHM_NAME : "(none)", CNT_VERSION);

    int first = 1;
    if (C.seg) {
        for (int i = 0; i < CNT_MAX_PEERS && k < cap - 128; i++) {
            cnt_peer *p = &C.seg->peers[i];
            if (!cnt_peer_alive(p)) continue;
            k += snprintf(out + k, cap - k,
                "%s{\"name\":\"%s\",\"role\":\"%s\",\"pid\":%llu,\"ops\":%llu}",
                first ? "" : ",", p->name, p->role,
                (unsigned long long)atomic_load(&p->pid),
                (unsigned long long)atomic_load(&p->ops));
            first = 0;
        }
    }
    snprintf(out + k, cap - k, "]}");
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

    if (counter_open(&C, join, "webd", "http") < 0) return 2;  /* only mode-aware line */

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 32) < 0) { perror("listen"); return 1; }

    printf("webd: listening on http://127.0.0.1:%d  mode=%s  pid=%d\n",
           port, counter_mode(&C), getpid());
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

        char body[4096];
        if (!strcmp(path, "/")) {
            reply(fd, "200 OK", "text/html; charset=utf-8", PAGE);
        } else if (!strcmp(path, "/api")) {
            json_state(body, sizeof body);
            reply(fd, "200 OK", "application/json", body);
        } else if (!strcmp(path, "/api/bump")) {
            counter_bump(&C);                             /* identical call site */
            json_state(body, sizeof body);
            reply(fd, "200 OK", "application/json", body);
        } else if (!strcmp(path, "/bump")) {
            uint64_t v = counter_bump(&C);                /* identical call site */
            snprintf(body, sizeof body, "bumped\ncount = %llu\nmode  = %s\n",
                     (unsigned long long)v, counter_mode(&C));
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else if (!strcmp(path, "/status")) {
            uint64_t v = counter_read(&C);                /* identical call site */
            snprintf(body, sizeof body, "count = %llu\nmode  = %s\n",
                     (unsigned long long)v, counter_mode(&C));
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else {
            reply(fd, "404 Not Found", "text/plain; charset=utf-8", "no\n");
        }
        close(fd);
    }
}
