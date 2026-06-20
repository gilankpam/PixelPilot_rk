#include "pp_slider_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int32_t pp_slider_step(int32_t raw, const pp_slider_cfg_t *cfg, int dir) {
    if (cfg->fine_step <= 0) return cfg->step;
    if (dir > 0) return (raw >= cfg->fine_threshold) ? cfg->step : cfg->fine_step;
    return (raw > cfg->fine_threshold) ? cfg->step : cfg->fine_step;
}

void pp_slider_fmt(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n) {
    int32_t div = cfg->disp_div > 0 ? cfg->disp_div : 1;
    if (div == 1 && cfg->decimals == 0) { snprintf(buf, n, "%d", (int)raw); return; }
    snprintf(buf, n, "%.*f", cfg->decimals, (double)raw / (double)div);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';            /* drop a bare trailing '.' */
    }
}

int32_t pp_slider_parse(const char *s, const pp_slider_cfg_t *cfg) {
    if (!s || !*s) return cfg->raw_min;
    int32_t raw;
    if (cfg->serialize == PP_SER_FLOAT_DIV) {
        int32_t div = cfg->disp_div > 0 ? cfg->disp_div : 1;
        raw = (int32_t)lround(atof(s) * (double)div);
    } else {
        raw = (int32_t)strtol(s, NULL, 10);
    }
    return clampi(raw, cfg->raw_min, cfg->raw_max);
}

void pp_slider_ser(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n) {
    if (cfg->serialize == PP_SER_FLOAT_DIV) pp_slider_fmt(raw, cfg, buf, n);
    else snprintf(buf, n, "%d", (int)raw);
}
