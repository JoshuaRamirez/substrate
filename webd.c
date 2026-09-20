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
 *   GET /reserve?n= -> reserve n ids, plain text. THE verb that waits.
 *   GET /lookup?k=  -> the table linked into this binary. No owner needed.
 *   PUT /blob?k=N   -> store a payload in the shared arena
 *   GET /blob?k=N   -> serve it straight out of the arena. Never copied here.
 *
 * One process, two databases: a shared one it JOINS for writes, and a private
 * read-only one it CARRIES. The second needs no owner at all -- it is already
 * mapped, and every copy of this binary shares one physical image of it.
 *   POST /admin/dbfile?path=...  -> move the database, live. ADMIN ONLY.
 *
 * The admin route is gated because webd's callers are the only things in this
 * system OUTSIDE the trust boundary: a browser tab can reach port 8080 but
 * cannot map the segment. Peers are inside by construction, so `top` moves the
 * database with no token at all. Callers must present X-Admin-Token, which the
 * owner minted into the segment and webd reads from there to compare.
 *
 * Three properties do the work, and all three are needed:
 *   POST    -- a GET can be fired by <img src>; a POST cannot.
 *   a custom header -- forces a CORS preflight that this server never answers,
 *                      so a cross-origin page cannot even send the request.
 *   an unguessable token -- read only from shm, or from the same-origin page.
 *
 * Moving the database is not an RPC to dbd. webd writes the path into the
 * shared page and returns; dbd reads it on its next tick. There is no request,
 * no reply, and no one to be down.
 *
 * It holds no state. Kill it and start a new one and nothing is lost, because
 * the state was never here -- it is in the owner's page.
 */
#include "substrate.h"
#include "emdb.h"
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>

static substrate C;
static emdb    DB;      /* in-process, read-only, mapped from our own image */

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><title>substrate</title>"
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
"const TOK=%TOKEN%;"
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
" const r=await fetch('/admin/dbfile?path='+encodeURIComponent(v),"
"  {method:'POST',cache:'no-store',headers:{'X-Admin-Token':TOK}});"
" const d=await r.json();"
" m.textContent=d.ok?'asked. dbd moves within 100ms.':('refused: '+d.detail);"
" m.style.color=d.ok?'#34cc66':'#d46a6a';"
" document.activeElement.blur();setTimeout(tick,200);"
"}"
"tick();setInterval(tick,300);"
"</script>";


/* Pull one header value out of a raw request. Case-insensitive name match. */
static int header(const char *req, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");
    while (p) {
        p += 2;
        if (p[0] == '\r') break;                      /* end of headers */
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && k < cap - 1) k++;
            memcpy(out, v, k); out[k] = 0;
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    if (cap) out[0] = 0;
    return 0;
}

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
    if (!C.seg || !sub_path_read(C.seg, out, cap) || !out[0])
        snprintf(out, cap, "(none)");
}

/* write(2) may write less than asked; a partial header is a corrupt response. */
static void writeall(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void reply(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", status, ctype, strlen(body));
    writeall(fd, hdr, (size_t)n);
    writeall(fd, body, strlen(body));
}

/* The peer table, rendered for the browser. Same rows `top` draws. */
static void json_state(char *out, size_t cap) {
    size_t k = 0;
    char db[SUB_PATHLEN], dbj[SUB_PATHLEN * 2];
    dbfile_now(db, sizeof db);
    jsonesc(db, dbj, sizeof dbj);
    k += snprintf(out + k, cap - k,
        "{\"count\":%llu,\"mode\":\"%s\",\"shm\":\"%s\",\"version\":%u,"
        "\"dbfile\":\"%s\",\"peers\":[",
        (unsigned long long)sub_read(&C), sub_mode(&C),
        C.joined ? SUB_SHM_NAME : "(none)", SUB_VERSION, dbj);

    int first = 1;
    if (C.seg) {
        for (int i = 0; i < SUB_MAX_PEERS && k < cap - 128; i++) {
            sub_peer *p = &C.seg->peers[i];
            if (!sub_peer_alive(p)) continue;
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
            if (sub_open(&C, join, "webd", "http") < 0) return 2;
            uint64_t v = sub_bump(&C);
            printf("webd selftest: mode=%s count=%llu\n", sub_mode(&C),
                   (unsigned long long)v);
            sub_close(&C);
            return 0;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    emdb_open(&DB);                        /* no I/O: dyld already mapped it */
    if (sub_open(&C, join, "webd", "http") < 0) return 2;  /* only mode-aware line */

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
           port, sub_mode(&C), getpid());
    fflush(stdout);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;

        /* One read() could split the headers across TCP segments and drop the
         * auth header on the floor -- which fails closed, but for the wrong
         * reason. Read until the blank line, or give up with a real status. */
        char buf[8192];
        size_t have = 0;
        int too_big = 0;
        for (;;) {
            if (have >= sizeof buf - 1) { too_big = 1; break; }
            ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = 0;
            if (strstr(buf, "\r\n\r\n")) break;
        }
        if (too_big) {
            reply(fd, "431 Request Header Fields Too Large",
                  "text/plain; charset=utf-8", "too big\n");
            close(fd); continue;
        }
        if (have == 0) { close(fd); continue; }
        buf[have] = 0;

        /* Did our owner go away, or come back? Cheap, once per request. */
        sub_revalidate(&C);

        char method[8] = {0}, path[256] = {0};
        sscanf(buf, "%7s %255s", method, path);

        char body[4096];
        if (!strcmp(path, "/")) {
            /* The token goes into the page. A cross-origin script may fetch "/"
             * but the same-origin policy forbids it reading the response, so
             * this hands the credential to your tab and to nobody else. */
            static char page[sizeof PAGE + 64];
            char tokj[SUB_TOKLEN + 8];
            snprintf(tokj, sizeof tokj, "'%s'", (C.seg && C.seg->admin[0]) ? C.seg->admin : "");
            const char *mark = strstr(PAGE, "%TOKEN%");
            if (!mark) {                       /* template broken at build time */
                reply(fd, "500 Internal Server Error",
                      "text/plain; charset=utf-8", "page template missing %TOKEN%\n");
            } else {
                size_t pre = (size_t)(mark - PAGE);
                snprintf(page, sizeof page, "%.*s%s%s", (int)pre, PAGE, tokj, mark + 7);
                reply(fd, "200 OK", "text/html; charset=utf-8", page);
            }
        } else if (!strcmp(path, "/api")) {
            json_state(body, sizeof body);
            reply(fd, "200 OK", "application/json", body);
        } else if (!strcmp(path, "/api/bump")) {
            sub_bump(&C);                             /* identical call site */
            json_state(body, sizeof body);
            reply(fd, "200 OK", "application/json", body);
        } else if (!strcmp(path, "/bump")) {
            uint64_t v = sub_bump(&C);                /* identical call site */
            snprintf(body, sizeof body, "bumped\ncount = %llu\nmode  = %s\n",
                     (unsigned long long)v, sub_mode(&C));
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else if (!strncmp(path, "/admin/dbfile", 13)) {
            /* Admin. Gated three ways: method, custom header, unguessable token. */
            char tok[SUB_TOKLEN * 2] = {0};
            header(buf, "X-Admin-Token", tok, sizeof tok);

            if (strcmp(method, "POST") != 0) {
                reply(fd, "405 Method Not Allowed", "application/json",
                      "{\"ok\":false,\"detail\":\"POST only\"}");
            } else if (!C.seg) {
                reply(fd, "409 Conflict", "application/json",
                      "{\"ok\":false,\"detail\":\"not joined -- this webd owns no segment\"}");
            } else if (!sub_admin_ok(C.seg, tok)) {
                reply(fd, "401 Unauthorized", "application/json",
                      "{\"ok\":false,\"detail\":\"X-Admin-Token missing or wrong\"}");
            } else {
                const char *q = strstr(path, "?path=");
                char want[SUB_PATHLEN] = {0};
                int rc = q ? (urldec(q + 6, want, sizeof want), sub_path_write(C.seg, want))
                           : -8;
                const char *msg = rc == -8 ? "usage: POST /admin/dbfile?path=/absolute/path"
                                           : sub_path_error(rc);
                char wj[SUB_PATHLEN * 2];
                jsonesc(want, wj, sizeof wj);
                snprintf(body, sizeof body,
                         "{\"ok\":%s,\"requested\":\"%s\",\"detail\":\"%s\"}",
                         rc == 0 ? "true" : "false", wj, msg);
                reply(fd, rc == 0 ? "200 OK" : "400 Bad Request", "application/json", body);
            }
        } else if (!strncmp(path, "/reserve", 8)) {
            /* The one call in this program that waits for an answer. Alone it
             * is an add; joined it is a ring round trip. Same call site. */
            const char *q = strstr(path, "?n=");
            uint64_t n = q ? strtoull(q + 3, NULL, 10) : 1;
            if (n == 0 || n > 1000000) n = 1;
            uint64_t base = sub_reserve(&C, n);        /* identical call site */
            if (base)
                snprintf(body, sizeof body,
                         "reserved %llu ids\nbase  = %llu\nlast  = %llu\nmode  = %s\n",
                         (unsigned long long)n, (unsigned long long)base,
                         (unsigned long long)(base + n - 1), sub_mode(&C));
            else
                snprintf(body, sizeof body,
                         "reserve failed (ring full, or the owner went away)\nmode  = %s\n",
                         sub_mode(&C));
            reply(fd, base ? "200 OK" : "503 Service Unavailable",
                  "text/plain; charset=utf-8", body);
        } else if (!strncmp(path, "/lookup", 7)) {
            const char *q = strstr(path, "?k=");
            char k[EMDB_KEYLEN] = {0};
            if (q) urldec(q + 3, k, sizeof k);
            uint64_t v = 0;
            int hit = k[0] && emdb_get(&DB, k, &v);
            snprintf(body, sizeof body,
                     "%s\nkey     = %s\nvalue   = %llu\nrecords = %zu\nsource  = this binary\n",
                     hit ? "hit" : "miss", k[0] ? k : "(none)",
                     (unsigned long long)v, DB.count);
            reply(fd, hit ? "200 OK" : "404 Not Found",
                  "text/plain; charset=utf-8", body);
        } else if (!strncmp(path, "/blob", 5)) {
            const char *q = strstr(path, "?k=");
            uint64_t key = q ? strtoull(q + 3, NULL, 10) : 0;
            if (!C.seg || !key) {
                reply(fd, "400 Bad Request", "text/plain; charset=utf-8",
                      "need ?k=N and a joined webd\n");
            } else if (!strcmp(method, "PUT") || !strcmp(method, "POST")) {
                /* Read the body STRAIGHT INTO an arena block. The bytes go
                 * kernel -> shared page and are never copied again, by anyone.
                 * Landing them in a local buffer first would double the cost
                 * of the one path that still has to copy at all. */
                char clh[32] = {0};
                header(buf, "Content-Length", clh, sizeof clh);
                unsigned long long want = strtoull(clh, NULL, 10);

                const char *b = strstr(buf, "\r\n\r\n");
                size_t got = b ? have - (size_t)((b + 4) - buf) : 0;

                int rc;
                if (!want || want > SUB_BLOCK_SIZE) {
                    rc = -1;
                } else {
                    int blk = sub_block_alloc(C.seg);
                    if (blk < 0) { rc = -2; }            /* arena full: real backpressure */
                    else {
                        uint8_t *dst = sub_block_ptr(C.seg, blk);
                        if (got) memcpy(dst, b + 4, got > want ? (size_t)want : got);
                        size_t off = got > want ? (size_t)want : got;
                        while (off < want) {
                            ssize_t r = read(fd, dst + off, (size_t)want - off);
                            if (r <= 0) break;
                            off += (size_t)r;
                        }
                        rc = (off == want) ? sub_publish(&C, key, blk, want) : -4;
                        if (rc != 0 && off != want) sub_block_free(C.seg, blk);
                        got = off;
                    }
                }
                snprintf(body, sizeof body,
                         "{\"ok\":%s,\"key\":%llu,\"len\":%zu,\"blocks_free\":%d}",
                         rc == 0 ? "true" : "false", (unsigned long long)key, got,
                         sub_blocks_free(C.seg));
                reply(fd, rc == 0 ? "200 OK" : "507 Insufficient Storage",
                      "application/json", body);
            } else {
                uint64_t len = 0;
                const uint8_t *p = sub_get(&C, key, &len);
                if (!p || !len) {
                    reply(fd, "404 Not Found", "text/plain; charset=utf-8", "no such blob\n");
                } else {
                    /* Straight from the arena to the socket. webd never copies
                     * it into a buffer of its own; the bytes go from the page
                     * the owner put them in to the wire. */
                    char hdr[256];
                    int hn = snprintf(hdr, sizeof hdr,
                        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        "Content-Length: %llu\r\nConnection: close\r\n\r\n",
                        (unsigned long long)len);
                    writeall(fd, hdr, (size_t)hn);
                    writeall(fd, (const char *)p, (size_t)len);
                }
            }
        } else if (!strcmp(path, "/status")) {
            uint64_t v = sub_read(&C);                /* identical call site */
            char db[SUB_PATHLEN];
            dbfile_now(db, sizeof db);
            snprintf(body, sizeof body, "count = %llu\nmode  = %s\ndb    = %s\n",
                     (unsigned long long)v, sub_mode(&C), db);
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else {
            reply(fd, "404 Not Found", "text/plain; charset=utf-8", "no\n");
        }
        close(fd);
    }
}
