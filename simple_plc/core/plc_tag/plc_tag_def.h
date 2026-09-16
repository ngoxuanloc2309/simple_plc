#ifndef PLC_TAG_DEF_H
#define PLC_TAG_DEF_H

/*
 * plc_tag_def.h - Layer 2 (PLC Core)
 *
 * Concrete tag index assignment for the Remote I/O 8DI/8DO/4AI SKU. This
 * file turns the fixed wire layout in
 * docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md, section
 * 5.1 ("FIXED TAG LAYOUT -- WIRE PROFILE V1") into named constants so
 * Layer 3/4 code (plc_io.c, board init, rule authoring/tests) never has to
 * hardcode a bare index and hope it is right.
 *
 * V1.9 CHANGE from the earlier v1.7-based layout: there is no more
 * TAG_INDEX_NONE sentinel occupying index 0. DI0 is TagIndex 0 directly
 * (v1.7 had DI0 at index 1, with index 0 reserved as a "no reference"
 * sentinel for things like an unused Rule.guard_tag). Every index below
 * shifts down by 1 compared to the old v1.7-based file. V1.9 also adds a
 * new COUNTER range (116-123, 8 slots) that did not exist in v1.7 at all.
 * If any code elsewhere stored a v1.7 TAG_DI0..TAG_VREG_R15 value
 * (persisted config, a written test, a hand-authored rule), that value is
 * now off by one class of one slot and must be regenerated, not reused.
 *
 * Deliberately plain #define ("Cach A" per docs/architecture.md section 7),
 * NOT a self-numbering enum. Rationale: MAX_TAGS is a fixed, spec-owned
 * layout -- every index below is pinned by the official allocation table,
 * not by declaration order in this file. An enum would silently renumber
 * everything if an entry were ever added/removed/reordered; #define makes
 * each index an explicit, independently-checkable fact.
 *
 * Same porting boundary as plc_tag.h: no Layer 0/1 include here. This file
 * only names indices -- it does not say which GPIO/ADC channel a given
 * index maps to on real hardware. That mapping is plc_io.c's job (Layer 3,
 * not yet written); plc_io.c is expected to #include this header rather
 * than repeat the raw indices.
 *
 * Total: 124 tags used by this SKU (DI+DO+AI+VFLAG+VREG+VREG_RETAIN+
 * COUNTER), indices 124-127 (4 slots) reserved -- unused by Remote I/O,
 * earmarked for future Gateway SKU remote Modbus tags (TAG_MB_COIL /
 * TAG_MB_HOLDING). This file does not define anything in that reserved
 * range -- a future Gateway SKU gets its own tag_def header when that SKU
 * is designed, it does not extend this one.
 *
 * See docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md,
 * section 5.1, and docs/architecture.md, section 2.1 and section 7.
 */

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

#endif /* PLC_TAG_DEF_H */