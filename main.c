/*
 * m7mddz File Manager for PS4 (homebrew)
 *
 * The PS4 runs a tiny HTTP server (port 8080). You open the address from a
 * phone / PC browser on the same Wi-Fi and manage the console's files from a
 * full web UI (browse, upload, download, rename, copy, move, delete, mkdir).
 * Access is protected by a random 4-digit PIN shown in an on-screen notification.
 *
 * Plain POSIX code: builds with OpenOrbis (-DM7_PS4) and also on Linux for testing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "index_html.h"

#define PORT      8080
#define HDR_MAX   16384
#define PATH_MAX_ 1024
#define IO_BUF    (256 * 1024)

static char g_pin[8];

#ifdef M7_PS4
typedef struct {
    char unk[45];
    char msg[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *text) {
    notify_request_t r;
    memset(&r, 0, sizeof(r));
    strncpy(r.msg, text, sizeof(r.msg) - 1);
    sceKernelSendNotificationRequest(0, &r, sizeof(r), 0);
}
#else
static void notify(const char *text) { printf("[notify] %s\n", text); }
#endif

/* ---------- small helpers ---------- */

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_head(int fd, int code, const char *ctype, long long len, const char *extra) {
    char h[768];
    const char *msg = code == 200 ? "OK" : code == 401 ? "Unauthorized" :
                      code == 404 ? "Not Found" : code == 400 ? "Bad Request" : "Error";
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n%s\r\n",
        code, msg, ctype, len, extra ? extra : "");
    send_all(fd, h, (size_t)n);
}

static void send_text(int fd, int code, const char *ctype, const char *body) {
    send_head(fd, code, ctype, (long long)strlen(body), NULL);
    send_all(fd, body, strlen(body));
}

static void send_json_err(int fd, int code, const char *err) {
    char b[512];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", err);
    send_text(fd, code, "application/json", b);
}

static void send_ok(int fd) { send_text(fd, 200, "application/json", "{\"ok\":true}"); }

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dsz) {
    size_t o = 0;
    while (*src && o + 1 < dsz) {
        if (*src == '%' && hexv(src[1]) >= 0 && hexv(src[2]) >= 0) {
            dst[o++] = (char)(hexv(src[1]) * 16 + hexv(src[2]));
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

/* get query param `key` from "a=1&b=2" into out (decoded) */
static int qget(const char *query, const char *key, char *out, size_t osz) {
    size_t kl = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '&');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            char tmp[PATH_MAX_ * 3];
            if (l >= sizeof(tmp)) return 0;
            memcpy(tmp, v, l);
            tmp[l] = 0;
            url_decode(out, tmp, osz);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = 0;
    return 0;
}

/* path must be absolute and must not contain ".." components */
static int path_ok(const char *p) {
    if (p[0] != '/') return 0;
    const char *s = p;
    while (*s) {
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || s[3] == 0)) return 0;
        s++;
    }
    return 1;
}

static void json_escape(FILE *f, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

/* ---------- file operations ---------- */

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) { close(in); return -1; }
    char *buf = (char *)malloc(IO_BUF);
    if (!buf) { close(in); close(out); return -1; }
    int rc = 0;
    ssize_t n;
    while ((n = read(in, buf, IO_BUF)) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w <= 0) { rc = -1; goto done; }
            p += w;
            n -= w;
        }
    }
    if (n < 0) rc = -1;
done:
    free(buf);
    close(in);
    close(out);
    return rc;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);

    mkdir(dst, 0777);
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s2[PATH_MAX_ + 256], d2[PATH_MAX_ + 256];
        snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
        snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
        if (copy_tree(s2, d2) != 0) rc = -1;
    }
    closedir(d);
    return rc;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[PATH_MAX_ + 256];
        snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
        if (remove_tree(c) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

static int move_path(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    /* cross-device: copy then delete */
    if (copy_tree(src, dst) != 0) return -1;
    return remove_tree(src);
}

/* ---------- request handlers ---------- */

static void h_list(int fd, const char *query) {
    char path[PATH_MAX_];
    qget(query, "path", path, sizeof(path));
    if (!path_ok(path)) { send_json_err(fd, 400, "bad path"); return; }

    DIR *d = opendir(path);
    if (!d) { send_json_err(fd, 404, "cannot open folder"); return; }

    FILE *f = tmpfile();
    if (!f) { closedir(d); send_json_err(fd, 500, "tmpfile failed"); return; }

    fputs("{\"ok\":true,\"path\":\"", f);
    json_escape(f, path);
    fputs("\",\"items\":[", f);

    struct dirent *e;
    int first = 1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[PATH_MAX_ + 256];
        snprintf(full, sizeof(full), "%s%s%s", path, strcmp(path, "/") ? "/" : "", e->d_name);
        struct stat st;
        int isdir = 0;
        long long sz = 0, mt = 0;
        if (stat(full, &st) == 0) {
            isdir = S_ISDIR(st.st_mode);
            sz = (long long)st.st_size;
            mt = (long long)st.st_mtime;
        }
        if (!first) fputc(',', f);
        first = 0;
        fputs("{\"n\":\"", f);
        json_escape(f, e->d_name);
        fprintf(f, "\",\"d\":%s,\"s\":%lld,\"t\":%lld}", isdir ? "true" : "false", sz, mt);
    }
    closedir(d);
    fputs("]}", f);

    long len = ftell(f);
    rewind(f);
    send_head(fd, 200, "application/json; charset=utf-8", len, NULL);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) send_all(fd, buf, n);
    fclose(f);
}

static void h_download(int fd, const char *query) {
    char path[PATH_MAX_];
    qget(query, "path", path, sizeof(path));
    struct stat st;
    if (!path_ok(path) || stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        send_json_err(fd, 404, "file not found");
        return;
    }
    int in = open(path, O_RDONLY);
    if (in < 0) { send_json_err(fd, 404, "cannot open file"); return; }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char extra[PATH_MAX_ + 128];
    snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n", base);
    send_head(fd, 200, "application/octet-stream", (long long)st.st_size, extra);

    char *buf = (char *)malloc(IO_BUF);
    if (buf) {
        ssize_t n;
        while ((n = read(in, buf, IO_BUF)) > 0)
            if (send_all(fd, buf, (size_t)n) != 0) break;
        free(buf);
    }
    close(in);
}

/* `pre` = body bytes that were already read together with the headers */
static void h_upload(int fd, const char *query, const char *pre, size_t prelen, long long clen) {
    char path[PATH_MAX_];
    qget(query, "path", path, sizeof(path));
    if (!path_ok(path)) { send_json_err(fd, 400, "bad path"); return; }

    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) { send_json_err(fd, 500, "cannot create file"); return; }

    long long left = clen;
    int rc = 0;
    if (prelen > (size_t)left) prelen = (size_t)left;
    if (prelen && write(out, pre, prelen) != (ssize_t)prelen) rc = -1;
    left -= (long long)prelen;

    char *buf = (char *)malloc(IO_BUF);
    if (!buf) rc = -1;
    while (rc == 0 && left > 0) {
        size_t want = left > IO_BUF ? IO_BUF : (size_t)left;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) { rc = -1; break; }
        if (write(out, buf, (size_t)n) != n) { rc = -1; break; }
        left -= n;
    }
    free(buf);
    close(out);
    if (rc) { unlink(path); send_json_err(fd, 500, "upload failed"); }
    else send_ok(fd);
}

static void handle_request(int fd) {
    char hdr[HDR_MAX + 1];
    size_t got = 0;
    char *body = NULL;

    while (got < HDR_MAX) {
        ssize_t n = recv(fd, hdr + got, HDR_MAX - got, 0);
        if (n <= 0) return;
        got += (size_t)n;
        hdr[got] = 0;
        body = strstr(hdr, "\r\n\r\n");
        if (body) break;
    }
    if (!body) { send_json_err(fd, 400, "bad request"); return; }
    *body = 0;
    body += 4;
    size_t prelen = got - (size_t)(body - hdr);

    char method[8] = {0}, target[PATH_MAX_ * 3] = {0};
    if (sscanf(hdr, "%7s %3071s", method, target) != 2) { send_json_err(fd, 400, "bad request"); return; }

    char *query = strchr(target, '?');
    if (query) *query++ = 0; else query = target + strlen(target);

    /* header values (case-insensitive lookup) */
    char low[HDR_MAX + 1];
    size_t i;
    for (i = 0; hdr[i]; i++) low[i] = (char)tolower((unsigned char)hdr[i]);
    low[i] = 0;

    long long clen = 0;
    char *p = strstr(low, "content-length:");
    if (p) clen = atoll(p + 15);

    char pin[16] = {0};
    p = strstr(low, "x-pin:");
    if (p) {
        const char *v = hdr + (p - low) + 6;
        while (*v == ' ') v++;
        size_t k = 0;
        while (*v && *v != '\r' && *v != '\n' && k < sizeof(pin) - 1) pin[k++] = *v++;
        pin[k] = 0;
    }

    /* static page is public; everything under /api needs the PIN */
    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) {
        send_head(fd, 200, "text/html; charset=utf-8", (long long)INDEX_HTML_LEN, NULL);
        send_all(fd, INDEX_HTML, INDEX_HTML_LEN);
        return;
    }
    if (strncmp(target, "/api/", 5) != 0) { send_json_err(fd, 404, "not found"); return; }

    char qpin[16] = {0};
    qget(query, "pin", qpin, sizeof(qpin));
    if (strcmp(pin, g_pin) != 0 && strcmp(qpin, g_pin) != 0) {
        send_json_err(fd, 401, "wrong pin");
        return;
    }

    const char *api = target + 5;
    char a[PATH_MAX_], b[PATH_MAX_];

    if (!strcmp(api, "ping")) { send_ok(fd); }
    else if (!strcmp(api, "list")) { h_list(fd, query); }
    else if (!strcmp(api, "download")) { h_download(fd, query); }
    else if (!strcmp(api, "upload")) { h_upload(fd, query, body, prelen, clen); }
    else if (!strcmp(api, "delete")) {
        qget(query, "path", a, sizeof(a));
        if (!path_ok(a) || !strcmp(a, "/")) send_json_err(fd, 400, "bad path");
        else if (remove_tree(a) == 0) send_ok(fd);
        else send_json_err(fd, 500, "delete failed");
    }
    else if (!strcmp(api, "mkdir")) {
        qget(query, "path", a, sizeof(a));
        if (!path_ok(a)) send_json_err(fd, 400, "bad path");
        else if (mkdir(a, 0777) == 0) send_ok(fd);
        else send_json_err(fd, 500, "mkdir failed");
    }
    else if (!strcmp(api, "rename") || !strcmp(api, "move") || !strcmp(api, "copy")) {
        qget(query, "from", a, sizeof(a));
        qget(query, "to", b, sizeof(b));
        if (!path_ok(a) || !path_ok(b)) { send_json_err(fd, 400, "bad path"); }
        else {
            int rc = !strcmp(api, "copy") ? copy_tree(a, b) : move_path(a, b);
            if (rc == 0) send_ok(fd); else send_json_err(fd, 500, "operation failed");
        }
    }
    else { send_json_err(fd, 404, "unknown api"); }
}

/* ---------- main ---------- */

int main(void) {
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(g_pin, sizeof(g_pin), "%04d", 1000 + rand() % 9000);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { notify("m7mddz: socket() failed"); return 1; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 8) != 0) {
        notify("m7mddz: cannot open port 8080");
        return 1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "m7mddz File Manager\nPIN: %s | Port: %d\nOpen http://<PS4-IP>:%d in your browser",
             g_pin, PORT, PORT);
    notify(msg);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        handle_request(c);
        close(c);
    }
    return 0;
}
