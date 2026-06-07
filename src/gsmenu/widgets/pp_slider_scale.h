#ifndef PP_SLIDER_SCALE_H
#define PP_SLIDER_SCALE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PP_SER_INT = 0, PP_SER_FLOAT_DIV = 1 } pp_slider_ser_t;

/* Slider value model: an integer "raw" value. Display value = raw/disp_div
 * with `decimals` places (trailing zeros trimmed). Stepping is `step`, or
 * `fine_step` when raw is below `fine_threshold` (fine_step=0 => uniform). */
typedef struct {
    int32_t raw_min, raw_max;
    int32_t step;
    int32_t fine_step;        /* 0 => uniform stepping */
    int32_t fine_threshold;
    int32_t disp_div;         /* >=1; display = raw/disp_div */
    int8_t  decimals;
    const char *unit;         /* NULL/"" => none (used by the widget, not here) */
    pp_slider_ser_t serialize;
} pp_slider_cfg_t;

/* Step magnitude for one press: dir>0 up, dir<0 down. */
int32_t pp_slider_step(int32_t raw, const pp_slider_cfg_t *cfg, int dir);
/* Numeric display of raw (no unit), trailing zeros + bare '.' trimmed. */
void    pp_slider_fmt(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n);
/* Parse a backend string to a clamped raw value. */
int32_t pp_slider_parse(const char *s, const pp_slider_cfg_t *cfg);
/* Wire string: PP_SER_INT => "%d" of raw; PP_SER_FLOAT_DIV => pp_slider_fmt. */
void    pp_slider_ser(int32_t raw, const pp_slider_cfg_t *cfg, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
