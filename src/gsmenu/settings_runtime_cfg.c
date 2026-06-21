#include "settings_runtime_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- module state ---- */
static char g_path[512] = "/etc/pixelpilot/runtime.json";
static pp_runtime_cfg_t g_state;
static bool g_state_primed = false;
static const pp_runtime_cfg_ops_t *g_ops = NULL;   /* set in Task 3 */

/* ---- enum <-> string ---- */
static int mode_str_to_int(const char *s, int fallback) {
    if (!s) return fallback;
    if (!strcmp(s, "raw"))      return 0;
    if (!strcmp(s, "reencode")) return 1;
    if (!strcmp(s, "both"))     return 2;
    return fallback;
}
static const char *mode_int_to_str(int m) {
    return m == 1 ? "reencode" : m == 2 ? "both" : "raw";
}

void pp_runtime_cfg_set_path(const char *path) {
    snprintf(g_path, sizeof g_path, "%s", path);
    g_state_primed = false;   /* path changed — re-read on next access */
}

void pp_runtime_cfg_defaults(pp_runtime_cfg_t *out) {
    out->dvr_mode        = 0;
    out->dvr_max_size_mb = 4000;
    out->dvr_reenc_kbps  = 8000;
    out->cc_enabled      = 0;
    out->cc_gain         = 25;
    out->cc_offset       = -15;
}

/* Parse the file at g_path into *out. Returns true on a successful read+parse. */
static bool read_file(pp_runtime_cfg_t *out) {
    pp_runtime_cfg_defaults(out);

    FILE *f = fopen(g_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 16)) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *dvr = cJSON_GetObjectItemCaseSensitive(root, "dvr");
    if (dvr) {
        cJSON *m  = cJSON_GetObjectItemCaseSensitive(dvr, "mode");
        if (cJSON_IsString(m)) out->dvr_mode = mode_str_to_int(m->valuestring, out->dvr_mode);
        cJSON *ms = cJSON_GetObjectItemCaseSensitive(dvr, "maxSizeMb");
        if (cJSON_IsNumber(ms)) out->dvr_max_size_mb = (int)ms->valuedouble;
        cJSON *rb = cJSON_GetObjectItemCaseSensitive(dvr, "reencBitrateKbps");
        if (cJSON_IsNumber(rb)) out->dvr_reenc_kbps = (int)rb->valuedouble;
    }
    cJSON *cc = cJSON_GetObjectItemCaseSensitive(root, "colorCorrection");
    if (cc) {
        cJSON *en = cJSON_GetObjectItemCaseSensitive(cc, "enabled");
        if (cJSON_IsBool(en)) out->cc_enabled = cJSON_IsTrue(en) ? 1 : 0;
        cJSON *g  = cJSON_GetObjectItemCaseSensitive(cc, "gain");
        if (cJSON_IsNumber(g)) out->cc_gain = (int)g->valuedouble;
        cJSON *o  = cJSON_GetObjectItemCaseSensitive(cc, "offset");
        if (cJSON_IsNumber(o)) out->cc_offset = (int)o->valuedouble;
    }
    cJSON_Delete(root);
    return true;
}

bool pp_runtime_cfg_load(pp_runtime_cfg_t *out) {
    bool ok = read_file(&g_state);
    g_state_primed = true;
    *out = g_state;
    return ok;
}

static void ensure_primed(void) {
    if (!g_state_primed) { read_file(&g_state); g_state_primed = true; }
}

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

bool pp_runtime_cfg_owns(const char *domain, const char *page, const char *key) {
    if (!eq(domain, "gs")) return false;
    if (eq(page, "dvr"))
        return eq(key, "dvr_mode") || eq(key, "dvr_max_size") || eq(key, "dvr_reenc_bitrate");
    if (eq(page, "display"))
        return eq(key, "color_correction") || eq(key, "cc_gain") || eq(key, "cc_offset");
    return false;
}

char *pp_runtime_cfg_get(const char *domain, const char *page, const char *key) {
    if (!pp_runtime_cfg_owns(domain, page, key)) return NULL;
    ensure_primed();

    char buf[32];
    if (eq(page, "dvr")) {
        if (eq(key, "dvr_mode"))          return strdup(mode_int_to_str(g_state.dvr_mode));
        if (eq(key, "dvr_max_size"))      { snprintf(buf, sizeof buf, "%d", g_state.dvr_max_size_mb); return strdup(buf); }
        if (eq(key, "dvr_reenc_bitrate")) { snprintf(buf, sizeof buf, "%d", g_state.dvr_reenc_kbps);  return strdup(buf); }
    } else { /* display */
        if (eq(key, "color_correction"))  return strdup(g_state.cc_enabled ? "on" : "off");
        if (eq(key, "cc_gain"))           { snprintf(buf, sizeof buf, "%d", g_state.cc_gain);   return strdup(buf); }
        if (eq(key, "cc_offset"))         { snprintf(buf, sizeof buf, "%d", g_state.cc_offset); return strdup(buf); }
    }
    return NULL;
}

void pp_runtime_cfg_set_ops(const pp_runtime_cfg_ops_t *ops) { g_ops = ops; }

bool pp_runtime_cfg_is_recording(void) {
    return (g_ops && g_ops->is_recording) ? (g_ops->is_recording() != 0) : false;
}

/* Atomic write of g_state to g_path via "<path>.tmp" + rename(). */
static void persist(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *dvr  = cJSON_AddObjectToObject(root, "dvr");
    cJSON_AddStringToObject(dvr, "mode", mode_int_to_str(g_state.dvr_mode));
    cJSON_AddNumberToObject(dvr, "maxSizeMb", g_state.dvr_max_size_mb);
    cJSON_AddNumberToObject(dvr, "reencBitrateKbps", g_state.dvr_reenc_kbps);
    cJSON *cc = cJSON_AddObjectToObject(root, "colorCorrection");
    cJSON_AddBoolToObject(cc, "enabled", g_state.cc_enabled ? 1 : 0);
    cJSON_AddNumberToObject(cc, "gain", g_state.cc_gain);
    cJSON_AddNumberToObject(cc, "offset", g_state.cc_offset);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;

    char tmp[540];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fflush(f);
        fclose(f);
        rename(tmp, g_path);   /* atomic replace */
    }
    free(txt);
}

static void apply_colortrans(void) {
    if (g_ops && g_ops->colortrans_apply)
        g_ops->colortrans_apply(g_state.cc_enabled,
                                g_state.cc_gain   / 10.0f,
                                g_state.cc_offset / 100.0f);
}

void pp_runtime_cfg_set(const char *domain, const char *page,
                        const char *key, const char *value) {
    if (!pp_runtime_cfg_owns(domain, page, key)) return;
    ensure_primed();

    if (eq(page, "dvr")) {
        if (eq(key, "dvr_mode")) {
            g_state.dvr_mode = mode_str_to_int(value, g_state.dvr_mode);
            if (g_ops && g_ops->dvr_set_mode) g_ops->dvr_set_mode(g_state.dvr_mode);
        } else if (eq(key, "dvr_max_size")) {
            g_state.dvr_max_size_mb = atoi(value);
            if (g_ops && g_ops->dvr_set_max_size) g_ops->dvr_set_max_size(g_state.dvr_max_size_mb);
        } else if (eq(key, "dvr_reenc_bitrate")) {
            g_state.dvr_reenc_kbps = atoi(value);
            if (g_ops && g_ops->dvr_reenc_set_bitrate) g_ops->dvr_reenc_set_bitrate(g_state.dvr_reenc_kbps);
        }
    } else { /* display */
        if (eq(key, "color_correction")) {
            g_state.cc_enabled = (strcmp(value, "on") == 0) ? 1 : 0;
            apply_colortrans();
        } else if (eq(key, "cc_gain")) {
            g_state.cc_gain = atoi(value);
            apply_colortrans();
        } else if (eq(key, "cc_offset")) {
            g_state.cc_offset = atoi(value);
            apply_colortrans();
        }
    }
    persist();
}
