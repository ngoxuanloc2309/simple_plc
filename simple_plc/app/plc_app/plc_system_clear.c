#include "plc_system_clear.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "plc_tag.h"
#include "plc_rule.h"
#include "plc_rule_flash.h"
#include "plc_retain.h"
#include "plc_fb.h"
#include "splc_opt.h"
#if SPLC_MQTT_ENABLE
#include "plc_mqtt.h"
#endif
#if SPLC_MBM_ENABLE
#include "plc_mbm.h"
#endif
#if SPLC_MBW_ENABLE
#include "plc_mbw.h"
#endif
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"
#endif
#if SPLC_MQTT_SUB_ENABLE
#include "plc_mqtt_sub.h"
#endif
#include "plc_wifi.h"
#include "plc_mqtt_tls.h"
#include "splc_log.h"

/*
 * plc_system_clear.c - Layer 4 (Engine & Application entry)
 *
 * See plc_system_clear.h for the scope, the "write an empty record instead
 * of erasing sectors" decision and the failure contract.
 *
 * Only Layer 2/3 APIs are used here (no sx_* calls), so this file builds
 * on a plain PC against stub plc_rule_flash_save()/retain_snapshot_write().
 *
 * The backups below are static (no heap, no big stack frames): about
 * 3.2 KB for the rule table and 0.5 KB for the tag values. Only one
 * clear runs at a time (single-threaded scan loop), so sharing them
 * between the three entry points is safe.
 */

static const char *TAG = "PLC_SYS_CLEAR";

static SPLC_RuleRecord s_rules_backup[MAX_RULES];
static int32_t         s_retain_backup[MAX_TAGS];
static uint8_t         s_fb_backup[PLC_FB_FLASH_SIZE];
#if SPLC_MQTT_ENABLE
static uint8_t         s_mqtt_backup[PLC_MQTT_FLASH_SIZE];   /* holds the broker password: cleared right after use */
#endif
#if SPLC_MBM_ENABLE
static uint8_t         s_mb_backup[PLC_MBM_FLASH_SIZE];
#endif
#if SPLC_MBW_ENABLE
static uint8_t         s_mbw_backup[PLC_MBW_FLASH_SIZE];
#endif
#if SPLC_REAL_ENABLE
static uint8_t         s_types_backup[PLC_TAGTYPE_FLASH_SIZE];
#endif
#if SPLC_MQTT_SUB_ENABLE
static uint8_t         s_sub_backup[PLC_MQTT_SUB_FLASH_SIZE];
#endif

bool plc_clear_rules(void)
{
    /* Keep the current table so a failed Flash write can be rolled back. */
    const uint16_t old_count = g_rule_count.rule_count;
    memcpy(s_rules_backup, g_rule_table, sizeof(s_rules_backup));
    plc_fb_export(s_fb_backup);   /* FB config is part of the program being cleared */
#if SPLC_MQTT_ENABLE
    plc_mqtt_export(s_mqtt_backup);   /* so is the MQTT config (telemetry blocks + broker) */
#endif
#if SPLC_MBM_ENABLE
    plc_mbm_export(s_mb_backup);          /* and the MB_READ blocks */
#endif
#if SPLC_MBW_ENABLE
    plc_mbw_export(s_mbw_backup);         /* and the MB_WRITE blocks */
#endif
#if SPLC_REAL_ENABLE
    plc_tagtype_export(s_types_backup);   /* and the tag-type table */
#endif
#if SPLC_MQTT_SUB_ENABLE
    plc_mqtt_sub_export(s_sub_backup);    /* and the MQTT Subscribe blocks */
#endif

    /*
     * rule_table_commit() rejects raw_data == NULL, so pass a real pointer
     * with rule_count = 0 (memcpy of 0 bytes; the rest of the table is
     * zeroed and all rule runtime state is reset by the function itself).
     */
    if (!rule_table_commit((const uint8_t *)g_rule_table, 0U)) {
        log_error(TAG, "CLEAR_RULES: rule_table_commit(0) rejected, nothing changed");
        return false;
    }

    /* The Function Block config goes with the rules: left behind it would
     * show Timers/Counters that no rule uses. This also drops any FB draft.
     * It runs BEFORE the save so the empty record carries a DISABLED FB
     * image. */
    plc_fb_clear_all();
#if SPLC_MQTT_ENABLE
    plc_mqtt_clear_all();   /* also drops any MQTT draft; runs BEFORE the save like the FB clear */
#endif
#if SPLC_MBM_ENABLE
    plc_mbm_clear_all();       /* every block disabled, draft dropped; BEFORE the save like the others */
#endif
#if SPLC_MBW_ENABLE
    plc_mbw_clear_all();       /* every block disabled, draft dropped; BEFORE the save like the others */
#endif
#if SPLC_REAL_ENABLE
    plc_tagtype_clear_all();   /* every tag undeclared, draft dropped; BEFORE the save like the others */
#endif
#if SPLC_MQTT_SUB_ENABLE
    plc_mqtt_sub_clear_all();  /* every block disabled, draft and queues dropped; BEFORE the save like the others */
#endif

    if (plc_rule_flash_save()) {
        log_info(TAG, "CLEAR_RULES done: empty rule table and FB config saved to Flash (was %u rule(s))",
                 (unsigned)old_count);
#if SPLC_MQTT_ENABLE
        memset(s_mqtt_backup, 0, sizeof(s_mqtt_backup));
#endif
        return true;
    }

    /* Flash did not take the empty table: after the next reboot the OLD
     * table would come back from Flash. Put RAM back too, so RAM and
     * Flash agree and the reported ERROR really means "not cleared". */
    log_error(TAG, "CLEAR_RULES FAILED: Flash save not verified, restoring %u rule(s) and FB config in RAM",
              (unsigned)old_count);
    (void)rule_table_commit((const uint8_t *)s_rules_backup, old_count);
    (void)plc_fb_import(s_fb_backup);   /* valid before, tag layout unchanged */
#if SPLC_MQTT_ENABLE
    (void)plc_mqtt_import(s_mqtt_backup);
    memset(s_mqtt_backup, 0, sizeof(s_mqtt_backup));
#endif
#if SPLC_MBM_ENABLE
    (void)plc_mbm_import(s_mb_backup);          /* valid before, tag layout unchanged */
#endif
#if SPLC_MBW_ENABLE
    (void)plc_mbw_import(s_mbw_backup);         /* valid before, tag layout unchanged */
#endif
#if SPLC_REAL_ENABLE
    (void)plc_tagtype_import(s_types_backup);   /* valid before, tag layout unchanged */
#endif
#if SPLC_MQTT_SUB_ENABLE
    (void)plc_mqtt_sub_import(s_sub_backup);    /* valid before, tag layout unchanged */
#endif
    return false;
}

bool plc_clear_retain(void)
{
    uint16_t n = 0U;

    for (uint16_t idx = 0U; idx < MAX_TAGS; idx++) {
        if (tag_get_kind(idx) == TAG_VREG_RETAIN) {
            s_retain_backup[idx] = tag_read(idx);
            tag_write(idx, 0);
            n++;
        }
    }

    /* retain_snapshot_write() reads the live tag values, so the zeros
     * written above are what gets saved. It reads the record back and
     * checks CRC + seq_num itself. */
    if (retain_snapshot_write()) {
        log_info(TAG, "CLEAR_RETAIN done: %u retain tag(s) set to 0 and saved to Flash", (unsigned)n);
        return true;
    }

    /* The zero record is not valid in Flash; the previous newest record
     * is still the one a reboot would restore. Restore the live values to
     * match it instead of leaving zeros that exist only in RAM. */
    log_error(TAG, "CLEAR_RETAIN FAILED: Flash write not verified, restoring %u retain tag(s) in RAM",
              (unsigned)n);
    for (uint16_t idx = 0U; idx < MAX_TAGS; idx++) {
        if (tag_get_kind(idx) == TAG_VREG_RETAIN) {
            tag_write(idx, s_retain_backup[idx]);
        }
    }
    return false;
}

bool plc_factory_reset(void)
{
    /* Both are always attempted (no early return) so one failing Flash
     * region does not prevent clearing the other. */
    const bool retain_ok = plc_clear_retain();
    const bool rules_ok  = plc_clear_rules();
    plc_wifi_factory_reset();   /* stored Wi-Fi credentials only; CLEAR_RULES alone never touches them */
    plc_mqtt_tls_factory_reset();/* stored TLS certificates (host clear()); CLEAR_RULES alone never touches them */

    if (retain_ok && rules_ok) {
        log_info(TAG, "FACTORY_RESET done: retain and rules cleared");
    } else {
        log_error(TAG, "FACTORY_RESET incomplete: retain=%s rules=%s",
                  retain_ok ? "ok" : "FAILED", rules_ok ? "ok" : "FAILED");
    }
    return retain_ok && rules_ok;
}