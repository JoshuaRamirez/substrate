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
 *   GET /dbfile?path=/abs/path  -> move the database, live. Plain text.
 *   GET /api/dbfile?path=...    -> the same, JSON
 *
 * Moving the database is not an RPC to dbd. webd writes the path into the
 * shared page and returns; dbd reads it on its next tick. There is no request,
 * no reply, and no one to be down.
 *
 * It holds no state. Kill it and start a new one and nothing is lost, because
 * the state was never here -- it is in the owner's page.
 */
#include "counter.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>

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
"#db{margin-top:30px;max-width:560px}"
"#db label{display:block;color:#616161;font:400 10px ui-monospace,Menlo,monospace;"
"padding-bottom:7px;border-bottom:1px solid #333;margin-bottom:11px}"
"#p{background:#111;color:#ddd;border:1px solid #333;border-radius:6px;"
"padding:8px 10px;font:12px ui-monospace,Menlo,monospace;width:398px}"
"#mv{background:#2f2f2f;padding:9px 16px;font-size:12px;margin-left:7px}"
"#mv:active{background:#252525}"
"#msg{color:#666;font-size:10px;margin-top:9px;min-height:12px}"
"</style>"
"<h1>PIPELINE</h1><div id=sub></div>"
"<div id=n>-</div><div id=lbl>count</div>"
"<button onclick='bump()'>bump</button>"
"<table><thead><tr><th></th><th>NAME</th><th>ROLE</th><th>PID</th>"
"<th>OPS</th></tr></thead><tbody id=rows></tbody></table>"
"<div id=db><label>DATABASE  (absolute path; the owner moves on its next tick)</label>"
"<input id=p spellcheck=false><button id=mv onclick='move()'>move</button>"
"<div id=msg></div></div>"
"<div id=ft>this page polls /api. the native windows poll nothing -- "
"they read the same page of memory.</div>"
"<script>"
"async function tick(){"
" try{const r=await fetch('/api',{cache:'no-store'});const d=await r.json();"
"  document.getElementById('n').textContent=d.count;"
"  document.getElementById('sub').textContent=d.shm+'  format v'+d.version+"
"   '  mode '+d.mode;"
"  const p=document.getElementById('p');"
"  if(document.activeElement!==p) p.value=d.dbfile;"
"  document.getElementById('rows').innerHTML=d.peers.map(p=>"
"   '<tr><td class=dot>&bull;</td><td class=nm>'+p.name+'</td><td>'+p.role+"
"   '</td><td>'+p.pid+'</td><td>'+p.ops+'</td></tr>').join('');"
" }catch(e){document.getElementById('sub').textContent='webd unreachable';}"
"}"
"async function bump(){await fetch('/api/bump',{cache:'no-store'});tick();}"
"async function move(){"
" const v=document.getElementById('p').value.trim();const m=document.getElementById('msg');"
" const r=await fetch('/api/dbfile?path='+encodeURIComponent(v),{cache:'no-store'});"
" const d=await r.json();"
" m.textContent=d.ok?'asked. dbd moves within 100ms.':('refused: '+d.detail);"
" m.style.color=d.ok?'#34cc66':'#d46a6a';"
" document.activeElement.blur();setTimeout(tick,200);"
"}"
"tick();setInterval(tick,300);"
"</script>";

/* Paths arrive percent-encoded and leave inside JSON. Neither is interesting;
 * both are necessary. */
static void urldec(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; in[i] && k + 1 < cap; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i+1])
                         && isxdigit((unsigned char)in[i+2])) {
            char h[3] = { in[i+1], in[i+2], 0 };
            out[k++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (in[i] == '+') out[k++] = ' ';
        else out[k++] = in[i];
    }
    out[k] = 0;
}

static void jsonesc(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; in[i] && k + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20)           { k += snprintf(out + k, cap - k, "\\u%04x", c); }
        else                          out[k++] = (char)c;
    }
    out[k] = 0;
}

/* Where the owner is persisting, as this process sees it. */
static void dbfile_now(char *out, size_t cap) {
    if (!C.seg || !cnt_path_read(C.seg, out, cap) || !out[0])
        snprintf(out, cap, "(none)");
}

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
    char db[CNT_PATHLEN], dbj[CNT_PATHLEN * 2];
    dbfile_now(db, sizeof db);
    jsonesc(db, dbj, sizeof dbj);
    k += snprintf(out + k, cap - k,
        "{\"count\":%llu,\"mode\":\"%s\",\"shm\":\"%s\",\"version\":%u,"
        "\"dbfile\":\"%s\",\"peers\":[",
        (unsigned long long)counter_read(&C), counter_mode(&C),
        C.joined ? CNT_SHM_NAME : "(none)", CNT_VERSION, dbj);

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
        } else if (!strncmp(path, "/dbfile", 7) || !strncmp(path, "/api/dbfile", 11)) {
            int as_json = !strncmp(path, "/api/", 5);
            const char *q = strstr(path, "?path=");
            char want[CNT_PATHLEN] = {0};
            int rc = -1;
            if (!C.seg)      rc = -9;                   /* alone: no one to tell */
            else if (!q)     rc = -8;
            else { urldec(q + 6, want, sizeof want); rc = cnt_path_write(C.seg, want); }

            const char *msg = rc == -9 ? "not joined -- this webd owns no segment"
                            : rc == -8 ? "usage: /dbfile?path=/absolute/path"
                                       : cnt_path_error(rc);
            if (as_json) {
                char wj[CNT_PATHLEN * 2];
                jsonesc(want, wj, sizeof wj);
                snprintf(body, sizeof body,
                         "{\"ok\":%s,\"requested\":\"%s\",\"detail\":\"%s\"}",
                         rc == 0 ? "true" : "false", wj, msg);
                reply(fd, rc == 0 ? "200 OK" : "400 Bad Request", "application/json", body);
            } else {
                snprintf(body, sizeof body, "%s\nrequested = %s\ndetail    = %s\n",
                         rc == 0 ? "asked" : "refused", want[0] ? want : "(none)", msg);
                reply(fd, rc == 0 ? "200 OK" : "400 Bad Request",
                      "text/plain; charset=utf-8", body);
            }
        } else if (!strcmp(path, "/status")) {
            uint64_t v = counter_read(&C);                /* identical call site */
            char db[CNT_PATHLEN];
            dbfile_now(db, sizeof db);
            snprintf(body, sizeof body, "count = %llu\nmode  = %s\ndb    = %s\n",
                     (unsigned long long)v, counter_mode(&C), db);
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else {
            reply(fd, "404 Not Found", "text/plain; charset=utf-8", "no\n");
        }
        close(fd);
    }
}
