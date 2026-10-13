#ifndef SPLC_MQTT_FMT_H
#define SPLC_MQTT_FMT_H

/*
 * splc_mqtt_fmt.h - Layer U (Utils)
 *
 * Pure MQTT payload rendering and template checking: no hardware, no heap,
 * no libc printf/float formatting, no strnlen. Builds and unit-tests on a
 * plain PC (docs/plc_mqtt_design.md section 8).
 *
 * Tags are int32. A tag that stands for a decimal quantity carries a
 * per-tag "dec" (0..3 decimal digits): the printed value is v / 10^dec,
 * done with integer arithmetic only. -5 with dec=1 prints "-0.5";
 * 2457 with dec=2 prints "24.57"; 812 with dec=0 prints "812".
 *
 * Every render function writes a NUL-terminated string and returns its
 * length, or -1 when the result would not fit (buf[0] is then '\0', nothing
 * partial is left behind).
 *
 * Custom template tokens (a '{' followed by a lower-case letter starts a
 * token; any other '{' - e.g. the '{' of a JSON object - and every '}' that
 * does not close a token are literal text):
 *     {val}  {valK}  {valK:N}   value of the K-th bound tag (K = 1..8, {val} = {val1});
 *                               N = 0..3 overrides that tag's dec
 *     {tag}  {tagK}             tag INDEX of the K-th bound tag ({tag} = {tag1})
 *     {vals}                    all bound values, comma separated
 *     {ts}                      epoch seconds, UTC (0 when no clock)
 *     {device}                  device name (truncated to SPLC_MQTT_FMT_DEVICE_MAX)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPLC_MQTT_FMT_MAX_TAGS    8U
#define SPLC_MQTT_FMT_DEC_MAX     3U
#define SPLC_MQTT_FMT_DEVICE_MAX  32U   /* wire client_id is 32 chars */

/* Worst-case printed widths, used by the template size check. */
#define SPLC_MQTT_FMT_NUM_MAX     12U   /* "-2147483.648" (INT32_MIN, dec=3) */
#define SPLC_MQTT_FMT_TAG_MAX     5U    /* "65535" */
#define SPLC_MQTT_FMT_TS_MAX      10U   /* "4294967295" */

typedef struct {
    uint8_t     count;                          /* bound tags, 1..8 */
    uint16_t    tag[SPLC_MQTT_FMT_MAX_TAGS];    /* tag index of each bound tag */
    int32_t     val[SPLC_MQTT_FMT_MAX_TAGS];    /* raw tag value */
    uint8_t     dec[SPLC_MQTT_FMT_MAX_TAGS];    /* decimal digits 0..3 (larger is clamped to 3) */
    uint32_t    ts;                             /* epoch seconds UTC, 0 = no clock */
    const char *device;                         /* NULL = "" */
} splc_mqtt_fmt_in_t;

typedef enum {
    SPLC_MQTT_FMT_OK = 0,
    SPLC_MQTT_FMT_ERR_ARG,          /* NULL pointer or tag_count outside 1..8 */
    SPLC_MQTT_FMT_ERR_UNBALANCED,   /* "{name" without a closing '}' */
    SPLC_MQTT_FMT_ERR_BAD_TOKEN,    /* unknown token, bad ":N" */
    SPLC_MQTT_FMT_ERR_TAG_RANGE,    /* K = 0, K > 8 or K > tag_count */
    SPLC_MQTT_FMT_ERR_TOO_LONG      /* worst-case output does not fit payload_max */
} splc_mqtt_fmt_err_t;

/* 10^dec for dec 0..3 (larger dec is clamped to 3). */
uint32_t splc_mqtt_fmt_pow10(uint8_t dec);

/* Prints v scaled by 10^-dec. */
int splc_mqtt_fmt_number(char *buf, size_t cap, int32_t v, uint8_t dec);

/* Compact JSON. 1 tag : {"tag":16,"val":24.5,"ts":1773228945}
 *               n tags: {"d":[24.5,101.3],"ts":1773228945}
 * Worst case is 127 characters, so it always fits SPLC_MQTT_PAYLOAD_MAX >= 128. */
int splc_mqtt_fmt_compact(char *buf, size_t cap, const splc_mqtt_fmt_in_t *in);

/* Raw: just the first tag's value ("24.5", "1"). */
int splc_mqtt_fmt_raw(char *buf, size_t cap, const splc_mqtt_fmt_in_t *in);

/* Custom: expands the tokens above. tmpl is read up to the first NUL or
 * tmpl_max bytes. A token that was not accepted by splc_mqtt_fmt_check_template()
 * (K > count, unknown name) makes the render fail with -1. */
int splc_mqtt_fmt_custom(char *buf, size_t cap, const char *tmpl, size_t tmpl_max,
                         const splc_mqtt_fmt_in_t *in);

/* COMMIT-time validation of a Custom template for a block with tag_count tags:
 * tokens well formed, K <= tag_count, and the WORST-CASE expansion (every
 * number at its widest) plus the NUL fits payload_max. */
splc_mqtt_fmt_err_t splc_mqtt_fmt_check_template(const char *tmpl, size_t tmpl_max,
                                                 uint8_t tag_count, size_t payload_max);

/* COV threshold in RAW tag units: ceil(deadband * 10^dec), at least 1, clamped
 * to INT32_MAX. Computed once at COMMIT so the scan only compares integers.
 * A tiny float representation error (0.3f * 10 = 3.0000001) does not push the
 * result up by one. Returns false (and leaves *out alone) for NaN, +-Inf or a
 * negative deadband. */
bool splc_mqtt_fmt_deadband_threshold(float deadband, uint8_t dec, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MQTT_FMT_H */