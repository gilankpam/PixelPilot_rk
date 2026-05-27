#include "settings_gs_writers.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void pp_gs_write_result_free(pp_gs_write_result_t *r) {
    if (r && r->err) { free(r->err); r->err = NULL; }
}

static pp_gs_write_result_t err_result(const char *msg) {
    pp_gs_write_result_t r = { -1, NULL };
    r.err = strdup(msg ? msg : "unknown");
    return r;
}

static char *slurp_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int atomic_write(const char *path, const char *contents, size_t len) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    ssize_t w = write(fd, contents, len);
    if (w != (ssize_t)len) { close(fd); unlink(tmp); return -1; }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Replace lines whose lhs (before '=') equals `key`, ignoring whitespace.
 * If no such line, insert after the [common] header. If no header, append.
 * Returns a heap-allocated new content string. */
static char *cfg_upsert(const char *src, const char *key, const char *value_line) {
    size_t cap = strlen(src) + strlen(value_line) + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    bool replaced = false;
    bool injected = false;
    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        size_t cpy = llen < sizeof line - 1 ? llen : sizeof line - 1;
        memcpy(line, p, cpy); line[cpy] = '\0';

        char lhs[64] = {0};
        const char *eq = strchr(line, '=');
        if (eq) {
            size_t k = (size_t)(eq - line);
            while (k > 0 && isspace((unsigned char)line[k-1])) k--;
            size_t i = 0;
            while (i < k && isspace((unsigned char)line[i])) i++;
            size_t klen = k - i;
            if (klen < sizeof lhs) { memcpy(lhs, line + i, klen); lhs[klen] = '\0'; }
        }

        bool skip_newline = false;
        if (strcmp(lhs, key) == 0 && !replaced) {
            strcat(out, value_line);
            replaced = true;
        } else if (strcmp(lhs, key) == 0 && replaced) {
            /* Already injected after [common]; skip the old key line entirely. */
            skip_newline = true;
        } else {
            strncat(out, p, llen);
        }
        if (eol) { if (!skip_newline) strcat(out, "\n"); p = eol + 1; } else { break; }

        if (!replaced && !injected && strncmp(line, "[common]", 8) == 0) {
            strcat(out, value_line);
            strcat(out, "\n");
            injected = true;
            replaced = true;
        }
    }
    if (!replaced) {
        if (out[0] && out[strlen(out)-1] != '\n') strcat(out, "\n");
        strcat(out, value_line);
        strcat(out, "\n");
    }
    return out;
}

static pp_gs_write_result_t write_kv_line(const char *cfg_path,
                                          const char *key, const char *value) {
    size_t src_len = 0;
    char *src = slurp_file(cfg_path, &src_len);
    if (!src) src = strdup("[common]\n");
    char line[256];
    snprintf(line, sizeof line, "%s = %s", key, value);
    char *out = cfg_upsert(src, key, line);
    free(src);
    if (!out) return err_result("alloc failed");
    int rc = atomic_write(cfg_path, out, strlen(out));
    free(out);
    if (rc != 0) return err_result(strerror(errno));
    return (pp_gs_write_result_t){ 0, NULL };
}

pp_gs_write_result_t pp_gs_wfbcfg_set_channel(const char *cfg_path, const char *value) {
    return write_kv_line(cfg_path, "wifi_channel", value);
}

pp_gs_write_result_t pp_gs_wfbcfg_set_bandwidth(const char *cfg_path, const char *value) {
    return write_kv_line(cfg_path, "bandwidth", value);
}

pp_gs_write_result_t pp_gs_wfbcfg_set_txpower(const char *cfg_path, const char *json) {
    return write_kv_line(cfg_path, "wifi_txpower", json);
}

pp_gs_write_result_t pp_gs_env_set(const char *env_path, const char *key, const char *value) {
    size_t src_len = 0;
    char *src = slurp_file(env_path, &src_len);
    if (!src) src = strdup("");
    /* Decide if value needs quoting. */
    bool needs_quote = false;
    for (const char *c = value; *c; c++) {
        if (isspace((unsigned char)*c) || *c == '"' || *c == '\'' ||
            *c == '$' || *c == '`' || *c == '\\') { needs_quote = true; break; }
    }
    char vbuf[512];
    if (needs_quote) {
        /* Single-quote the value; escape embedded single quotes via '\''. */
        char *o = vbuf;
        char *end = vbuf + sizeof vbuf - 1;
        *o++ = '\'';
        for (const char *c = value; *c && o < end - 3; c++) {
            if (*c == '\'') {
                /* Close quote, escaped single quote, reopen quote. */
                if (o + 4 > end) break;
                *o++ = '\'';
                *o++ = '\\';
                *o++ = '\'';
                *o++ = '\'';
            } else {
                *o++ = *c;
            }
        }
        if (o < end) *o++ = '\'';
        *o = '\0';
    } else {
        snprintf(vbuf, sizeof vbuf, "%s", value);
    }
    char line[768];
    snprintf(line, sizeof line, "%s=%s", key, vbuf);

    /* Same upsert routine but match on KEY= (no spaces, no [section]). */
    size_t cap = strlen(src) + strlen(line) + 8;
    char *out = (char *)malloc(cap);
    if (!out) { free(src); return err_result("alloc failed"); }
    out[0] = '\0';
    bool replaced = false;
    const char *p = src;
    size_t klen = strlen(key);
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (!replaced && llen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            strcat(out, line);
            replaced = true;
        } else {
            strncat(out, p, llen);
        }
        if (eol) { strcat(out, "\n"); p = eol + 1; } else { break; }
    }
    if (!replaced) {
        if (out[0] && out[strlen(out)-1] != '\n') strcat(out, "\n");
        strcat(out, line);
        strcat(out, "\n");
    }
    free(src);
    int rc = atomic_write(env_path, out, strlen(out));
    free(out);
    if (rc != 0) return err_result(strerror(errno));
    return (pp_gs_write_result_t){ 0, NULL };
}
