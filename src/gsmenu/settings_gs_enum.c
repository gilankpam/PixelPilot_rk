/* src/gsmenu/settings_gs_enum.c */
#include "settings_gs_enum.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* iw list line of interest:
 *   "      * 5180 MHz [36] (20.0 dBm)"
 * Filter out lines containing "disabled" or "radar detection".
 * Emit "<chan> (<freq> MHz)\n" per line, sorted by channel number,
 * deduplicated. Trailing newline trimmed. */

typedef struct { int chan; int freq; } chan_entry_t;

static int cmp_chan(const void *a, const void *b) {
    int ca = ((const chan_entry_t *)a)->chan;
    int cb = ((const chan_entry_t *)b)->chan;
    return ca - cb;
}

char *pp_gs_parse_iw_list_channels(const char *in) {
    if (!in || !*in) return NULL;
    chan_entry_t list[512];
    size_t n = 0;
    const char *p = in;
    while (*p && n < 512) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        size_t cpy = llen < sizeof line - 1 ? llen : sizeof line - 1;
        memcpy(line, p, cpy); line[cpy] = '\0';
        if (!strstr(line, "MHz") || strstr(line, "disabled") ||
            strstr(line, "radar detection")) {
            p = eol ? eol + 1 : p + llen;
            continue;
        }
        int freq = 0, chan = 0;
        const char *lb = strchr(line, '[');
        const char *rb = lb ? strchr(lb, ']') : NULL;
        if (lb && rb) {
            sscanf(lb + 1, "%d", &chan);
        }
        sscanf(line, " * %d MHz", &freq);
        if (freq <= 0) { /* fall back: scan for first number before MHz */
            const char *mhz = strstr(line, "MHz");
            if (mhz) for (const char *q = mhz - 1; q >= line; q--)
                if (isdigit((unsigned char)*q)) { while (q > line && isdigit((unsigned char)q[-1])) q--; freq = atoi(q); break; }
        }
        if (chan > 0 && freq > 0) {
            list[n].chan = chan; list[n].freq = freq;
            n++;
        }
        p = eol ? eol + 1 : p + llen;
    }
    if (n == 0) return NULL;
    qsort(list, n, sizeof list[0], cmp_chan);
    /* Dedup by channel. */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m == 0 || list[m-1].chan != list[i].chan) list[m++] = list[i];
    }
    /* Build output. */
    char buf[4096]; buf[0] = '\0';
    for (size_t i = 0; i < m; i++) {
        char line[64];
        snprintf(line, sizeof line, "%s%d (%d MHz)",
                 i == 0 ? "" : "\n", list[i].chan, list[i].freq);
        if (strlen(buf) + strlen(line) + 1 >= sizeof buf) break;
        strcat(buf, line);
    }
    return strdup(buf);
}

char *pp_gs_parse_drm_info_modes(const char *in) {
    if (!in) return NULL;
    cJSON *root = cJSON_Parse(in);
    if (!root) return NULL;
    cJSON *card = cJSON_GetObjectItemCaseSensitive(root, "/dev/dri/card0");
    if (!card) { cJSON_Delete(root); return NULL; }
    cJSON *conns = cJSON_GetObjectItemCaseSensitive(card, "connectors");
    if (!cJSON_IsArray(conns) || cJSON_GetArraySize(conns) < 2) {
        cJSON_Delete(root); return NULL;
    }
    cJSON *conn = cJSON_GetArrayItem(conns, 1);
    cJSON *modes = cJSON_GetObjectItemCaseSensitive(conn, "modes");
    if (!cJSON_IsArray(modes)) { cJSON_Delete(root); return NULL; }

    /* Collect "<name>@<vrefresh>" strings, skipping names containing 'i'. */
    char *items[256]; size_t n = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, modes) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(m, "name");
        cJSON *vref = cJSON_GetObjectItemCaseSensitive(m, "vrefresh");
        if (!cJSON_IsString(name) || !cJSON_IsNumber(vref)) continue;
        if (strchr(name->valuestring, 'i')) continue;
        char buf[64];
        snprintf(buf, sizeof buf, "%s@%d", name->valuestring, (int)vref->valuedouble);
        /* Dedup. */
        int dup = 0;
        for (size_t i = 0; i < n; i++) if (strcmp(items[i], buf) == 0) { dup = 1; break; }
        if (!dup && n < 256) items[n++] = strdup(buf);
    }
    cJSON_Delete(root);
    if (n == 0) return NULL;

    /* Sort by string (stable enough for display). */
    for (size_t i = 1; i < n; i++) for (size_t j = i; j > 0; j--) {
        if (strcmp(items[j-1], items[j]) > 0) {
            char *t = items[j-1]; items[j-1] = items[j]; items[j] = t;
        } else break;
    }
    char buf[4096]; buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i) strcat(buf, "\n");
        if (strlen(buf) + strlen(items[i]) + 1 >= sizeof buf) break;
        strcat(buf, items[i]);
    }
    for (size_t i = 0; i < n; i++) free(items[i]);
    return strdup(buf);
}

/* popen variants — overridable binary paths. */
static char *popen_slurp(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char *out = (char *)malloc(65536); if (!out) { pclose(p); return NULL; }
    size_t off = 0;
    size_t got;
    while ((got = fread(out + off, 1, 65535 - off, p)) > 0) {
        off += got;
        if (off >= 65535) break;
    }
    out[off] = '\0';
    pclose(p);
    return out;
}

char *pp_gs_enum_channels(void) {
    const char *bin = getenv("PP_GS_IW_BIN");
    if (!bin) bin = "iw";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s list 2>/dev/null", bin);
    char *raw = popen_slurp(cmd);
    if (!raw) return NULL;
    char *r = pp_gs_parse_iw_list_channels(raw);
    free(raw);
    return r;
}

char *pp_gs_enum_hdmi_modes(void) {
    const char *bin = getenv("PP_GS_DRM_INFO_BIN");
    if (!bin) bin = "drm_info";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s -j /dev/dri/card0 2>/dev/null", bin);
    char *raw = popen_slurp(cmd);
    if (!raw) return NULL;
    char *r = pp_gs_parse_drm_info_modes(raw);
    free(raw);
    return r;
}
