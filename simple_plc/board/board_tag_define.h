#ifndef BOARD_TAG_DEFINE_H
#define BOARD_TAG_DEFINE_H

/*
 * board_tag_define.h - Layer 4 (board/)
 *
 * NOTE (current status): as of the SPLC_TagLayout / tag_table_load_from_
 * flash(&layout) change (core/plc_tag/plc_tag.h), NO board .c file
 * includes this header anymore. board_zigbee_io.c now gets DI/DO/AI/...
 * base indices at runtime via tag_di_base_index()/tag_do_base_index()/etc.
 * (plc_tag.h) instead of a compile-time TAG_DI0/TAG_DO0 macro from here --
 * that avoids having two hand-maintained sources of the same offset (this
 * file's macros, and board_<sku>.c's SPLC_TagLayout) that could silently
 * drift apart if only one were updated after a layout change.
 *
 * Kept in the repo (not deleted) as a reference table of each board's
 * full tag map (useful when writing rules/tests by hand, or documenting a
 * board's wire layout) and as a starting point for a future board that
 * prefers compile-time constants for its own reasons -- but no code path
 * currently depends on it compiling correctly. If it drifts from a
 * board's real SPLC_TagLayout, nothing will catch that automatically;
 * do not treat it as authoritative without cross-checking the relevant
 * board_<sku>.c's s_tag_layout.
 */

#include "board_config.h"

/*===========BOARD_ZIGBEE_IO===========*/
#if BOARD_ZIGBEE_IO_4DI_4DO
/* 0-3: DI0-DI3 (digital inputs), kind = TAG_DI */
#define TAG_DI0  0
#define TAG_DI1  1
#define TAG_DI2  2
#define TAG_DI3  3

/* 4-7: DO0-DO3 (digital outputs), kind = TAG_DO */
#define TAG_DO0  4
#define TAG_DO1  5
#define TAG_DO2  6
#define TAG_DO3  7

/* NONE TAG AI */

/* 8-39: VFLAG0-VFLAG31 (internal virtual flags, 1-bit-ish, relay-like),
 * kind = TAG_VFLAG. Not retained across power loss.
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VFLAG0   8
#define TAG_VFLAG1   9
#define TAG_VFLAG2   10
#define TAG_VFLAG3   11
#define TAG_VFLAG4   12
#define TAG_VFLAG5   13
#define TAG_VFLAG6   14
#define TAG_VFLAG7   15
#define TAG_VFLAG8   16
#define TAG_VFLAG9   17
#define TAG_VFLAG10  18
#define TAG_VFLAG11  19
#define TAG_VFLAG12  20
#define TAG_VFLAG13  21
#define TAG_VFLAG14  22
#define TAG_VFLAG15  23
#define TAG_VFLAG16  24
#define TAG_VFLAG17  25
#define TAG_VFLAG18  26
#define TAG_VFLAG19  27
#define TAG_VFLAG20  28
#define TAG_VFLAG21  29
#define TAG_VFLAG22  30
#define TAG_VFLAG23  31
#define TAG_VFLAG24  32
#define TAG_VFLAG25  33
#define TAG_VFLAG26  34
#define TAG_VFLAG27  35
#define TAG_VFLAG28  36
#define TAG_VFLAG29  37
#define TAG_VFLAG30  38
#define TAG_VFLAG31  39

/* 40-71: VREG0-VREG31 (internal virtual registers, i32; timers, scratch
 * results), kind = TAG_VREG. Lost on power loss/reset -- use the
 * TAG_VREG_RETAIN range below for anything that must survive a reset
 * (e.g. production counters).
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VREG0   40
#define TAG_VREG1   41
#define TAG_VREG2   42
#define TAG_VREG3   43
#define TAG_VREG4   44
#define TAG_VREG5   45
#define TAG_VREG6   46
#define TAG_VREG7   47
#define TAG_VREG8   48
#define TAG_VREG9   49
#define TAG_VREG10  50
#define TAG_VREG11  51
#define TAG_VREG12  52
#define TAG_VREG13  53
#define TAG_VREG14  54
#define TAG_VREG15  55
#define TAG_VREG16  56
#define TAG_VREG17  57
#define TAG_VREG18  58
#define TAG_VREG19  59
#define TAG_VREG20  60
#define TAG_VREG21  61
#define TAG_VREG22  62
#define TAG_VREG23  63
#define TAG_VREG24  64
#define TAG_VREG25  65
#define TAG_VREG26  66
#define TAG_VREG27  67
#define TAG_VREG28  68
#define TAG_VREG29  69
#define TAG_VREG30  70
#define TAG_VREG31  71

/* 72-103: VREG_R0-VREG_R31 (retained virtual registers; production counts,
 * accumulators), kind = TAG_VREG_RETAIN. Survives power loss/reset via the
 * Flash retain snapshot (see docs/architecture.md, plc_retain.c, not yet
 * written).
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VREG_R0   72
#define TAG_VREG_R1   73
#define TAG_VREG_R2   74
#define TAG_VREG_R3   75
#define TAG_VREG_R4   76
#define TAG_VREG_R5   77
#define TAG_VREG_R6   78
#define TAG_VREG_R7   79
#define TAG_VREG_R8   80
#define TAG_VREG_R9   81
#define TAG_VREG_R10  82
#define TAG_VREG_R11  83
#define TAG_VREG_R12  84
#define TAG_VREG_R13  85
#define TAG_VREG_R14  86
#define TAG_VREG_R15  87
#define TAG_VREG_R16  88
#define TAG_VREG_R17  89
#define TAG_VREG_R18  90
#define TAG_VREG_R19  91
#define TAG_VREG_R20  92
#define TAG_VREG_R21  93
#define TAG_VREG_R22  94
#define TAG_VREG_R23  95
#define TAG_VREG_R24  96
#define TAG_VREG_R25  97
#define TAG_VREG_R26  98
#define TAG_VREG_R27  99
#define TAG_VREG_R28  100
#define TAG_VREG_R29  101
#define TAG_VREG_R30  102
#define TAG_VREG_R31  103

/* 104-111: COUNTER0-COUNTER7 (dedicated counter tags, incremented by
 * SPLC_ACT_INC_COUNTER), kind = TAG_COUNTER.
 * V1.9: entirely new range, did not exist in the earlier v1.7-based file. */
#define TAG_COUNTER0  104
#define TAG_COUNTER1  105
#define TAG_COUNTER2  106
#define TAG_COUNTER3  107
#define TAG_COUNTER4  108
#define TAG_COUNTER5  109
#define TAG_COUNTER6  110
#define TAG_COUNTER7  111

/*
 * 112-115 (4 slots): reserved for a future Gateway SKU's remote Modbus
 * tags (TAG_MB_COIL / TAG_MB_HOLDING). Unused by this SKU -- no #define
 * here on purpose. g_tag_table[112..115] stays TAG_NONE (all-zero) after
 * tag_table_load_from_flash() on a Remote I/O board; do not read/write
 * these indices from Remote I/O code.
 */

#endif
/*===========BOARD_ZIGBEE_IO===========*/

/*===========BOARD_REMOTE_IO===========*/
#if BOARD_REMOTE_IO_8DI_8DO
/* 0-7: DI0-DI7 (digital inputs), kind = TAG_DI */
#define TAG_DI0  0
#define TAG_DI1  1
#define TAG_DI2  2
#define TAG_DI3  3
#define TAG_DI4  4
#define TAG_DI5  5
#define TAG_DI6  6
#define TAG_DI7  7

/* 8-15: DO0-DO7 (digital outputs), kind = TAG_DO */
#define TAG_DO0  8
#define TAG_DO1  9
#define TAG_DO2  10
#define TAG_DO3  11
#define TAG_DO4  12
#define TAG_DO5  13
#define TAG_DO6  14
#define TAG_DO7  15

/* 16-19: AI0-AI3 (analog inputs), kind = TAG_AI */
#define TAG_AI0  16
#define TAG_AI1  17
#define TAG_AI2  18
#define TAG_AI3  19

/* 20-51: VFLAG0-VFLAG31 (internal virtual flags, 1-bit-ish, relay-like),
 * kind = TAG_VFLAG. Not retained across power loss.
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VFLAG0   20
#define TAG_VFLAG1   21
#define TAG_VFLAG2   22
#define TAG_VFLAG3   23
#define TAG_VFLAG4   24
#define TAG_VFLAG5   25
#define TAG_VFLAG6   26
#define TAG_VFLAG7   27
#define TAG_VFLAG8   28
#define TAG_VFLAG9   29
#define TAG_VFLAG10  30
#define TAG_VFLAG11  31
#define TAG_VFLAG12  32
#define TAG_VFLAG13  33
#define TAG_VFLAG14  34
#define TAG_VFLAG15  35
#define TAG_VFLAG16  36
#define TAG_VFLAG17  37
#define TAG_VFLAG18  38
#define TAG_VFLAG19  39
#define TAG_VFLAG20  40
#define TAG_VFLAG21  41
#define TAG_VFLAG22  42
#define TAG_VFLAG23  43
#define TAG_VFLAG24  44
#define TAG_VFLAG25  45
#define TAG_VFLAG26  46
#define TAG_VFLAG27  47
#define TAG_VFLAG28  48
#define TAG_VFLAG29  49
#define TAG_VFLAG30  50
#define TAG_VFLAG31  51

/* 52-83: VREG0-VREG31 (internal virtual registers, i32; timers, scratch
 * results), kind = TAG_VREG. Lost on power loss/reset -- use the
 * TAG_VREG_RETAIN range below for anything that must survive a reset
 * (e.g. production counters).
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VREG0   52
#define TAG_VREG1   53
#define TAG_VREG2   54
#define TAG_VREG3   55
#define TAG_VREG4   56
#define TAG_VREG5   57
#define TAG_VREG6   58
#define TAG_VREG7   59
#define TAG_VREG8   60
#define TAG_VREG9   61
#define TAG_VREG10  62
#define TAG_VREG11  63
#define TAG_VREG12  64
#define TAG_VREG13  65
#define TAG_VREG14  66
#define TAG_VREG15  67
#define TAG_VREG16  68
#define TAG_VREG17  69
#define TAG_VREG18  70
#define TAG_VREG19  71
#define TAG_VREG20  72
#define TAG_VREG21  73
#define TAG_VREG22  74
#define TAG_VREG23  75
#define TAG_VREG24  76
#define TAG_VREG25  77
#define TAG_VREG26  78
#define TAG_VREG27  79
#define TAG_VREG28  80
#define TAG_VREG29  81
#define TAG_VREG30  82
#define TAG_VREG31  83

/* 84-115: VREG_R0-VREG_R31 (retained virtual registers; production counts,
 * accumulators), kind = TAG_VREG_RETAIN. Survives power loss/reset via the
 * Flash retain snapshot (see docs/architecture.md, plc_retain.c, not yet
 * written).
 * V1.9 widens this range from 16 to 32 slots versus the earlier
 * v1.7-based file. */
#define TAG_VREG_R0   84
#define TAG_VREG_R1   85
#define TAG_VREG_R2   86
#define TAG_VREG_R3   87
#define TAG_VREG_R4   88
#define TAG_VREG_R5   89
#define TAG_VREG_R6   90
#define TAG_VREG_R7   91
#define TAG_VREG_R8   92
#define TAG_VREG_R9   93
#define TAG_VREG_R10  94
#define TAG_VREG_R11  95
#define TAG_VREG_R12  96
#define TAG_VREG_R13  97
#define TAG_VREG_R14  98
#define TAG_VREG_R15  99
#define TAG_VREG_R16  100
#define TAG_VREG_R17  101
#define TAG_VREG_R18  102
#define TAG_VREG_R19  103
#define TAG_VREG_R20  104
#define TAG_VREG_R21  105
#define TAG_VREG_R22  106
#define TAG_VREG_R23  107
#define TAG_VREG_R24  108
#define TAG_VREG_R25  109
#define TAG_VREG_R26  110
#define TAG_VREG_R27  111
#define TAG_VREG_R28  112
#define TAG_VREG_R29  113
#define TAG_VREG_R30  114
#define TAG_VREG_R31  115

/* 116-123: COUNTER0-COUNTER7 (dedicated counter tags, incremented by
 * SPLC_ACT_INC_COUNTER), kind = TAG_COUNTER.
 * V1.9: entirely new range, did not exist in the earlier v1.7-based file. */
#define TAG_COUNTER0  116
#define TAG_COUNTER1  117
#define TAG_COUNTER2  118
#define TAG_COUNTER3  119
#define TAG_COUNTER4  120
#define TAG_COUNTER5  121
#define TAG_COUNTER6  122
#define TAG_COUNTER7  123

/*
 * 124-127 (4 slots): reserved for a future Gateway SKU's remote Modbus
 * tags (TAG_MB_COIL / TAG_MB_HOLDING). Unused by this SKU -- no #define
 * here on purpose. g_tag_table[124..127] stays TAG_NONE (all-zero) after
 * tag_table_load_from_flash() on a Remote I/O board; do not read/write
 * these indices from Remote I/O code.
 */

#endif
/*===========BOARD_REMOTE_IO===========*/


#endif