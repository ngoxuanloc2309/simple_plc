#ifndef PLC_TAG_DEF_H
#define PLC_TAG_DEF_H

/*
 * plc_tag_def.h - Layer 2 (PLC Core)
 *
 * Concrete tag index assignment for the Remote I/O 8DI/8DO/4AI SKU. This
 * file turns the abstract allocation table in docs/architecture.md section
 * 2.1 / docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 3.2 into named
 * constants so Layer 3/4 code (plc_io.c, board init, rule authoring/tests)
 * never has to hardcode a bare index and hope it is right.
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
 * Total: 69 tags used by this SKU (index 0 sentinel + 68 real tags),
 * indices 69-127 (59 slots) reserved, unused by Remote I/O, earmarked for
 * a future Gateway SKU's remote Modbus tags (TAG_MB_COIL / TAG_MB_HOLDING).
 * This file does not define anything in that reserved range -- a future
 * Gateway SKU gets its own tag_def header when that SKU is designed, it
 * does not extend this one.
 *
 * See docs/architecture.md, section 2.1 and section 7.
 */

/* Index 0: TAG_NONE sentinel. Never assign a real tag here -- this is the
 * "no reference" default, e.g. for an unused Rule.guard_tag. Defined for
 * completeness/self-documentation; matches plc_tag.h's TAG_NONE enumerator
 * value, not to be confused with it (that one names the *kind*, this one
 * names the *index*). */
#define TAG_INDEX_NONE  0

/* 1-8: DI0-DI7 (digital inputs), kind = TAG_DI */
#define TAG_DI0  1
#define TAG_DI1  2
#define TAG_DI2  3
#define TAG_DI3  4
#define TAG_DI4  5
#define TAG_DI5  6
#define TAG_DI6  7
#define TAG_DI7  8

/* 9-16: DO0-DO7 (digital outputs), kind = TAG_DO */
#define TAG_DO0  9
#define TAG_DO1  10
#define TAG_DO2  11
#define TAG_DO3  12
#define TAG_DO4  13
#define TAG_DO5  14
#define TAG_DO6  15
#define TAG_DO7  16

/* 17-20: AI0-AI3 (analog inputs), kind = TAG_AI */
#define TAG_AI0  17
#define TAG_AI1  18
#define TAG_AI2  19
#define TAG_AI3  20

/* 21-36: VFLAG0-VFLAG15 (internal virtual flags, 1-bit-ish, relay-like),
 * kind = TAG_VFLAG. Not retained across power loss. */
#define TAG_VFLAG0   21
#define TAG_VFLAG1   22
#define TAG_VFLAG2   23
#define TAG_VFLAG3   24
#define TAG_VFLAG4   25
#define TAG_VFLAG5   26
#define TAG_VFLAG6   27
#define TAG_VFLAG7   28
#define TAG_VFLAG8   29
#define TAG_VFLAG9   30
#define TAG_VFLAG10  31
#define TAG_VFLAG11  32
#define TAG_VFLAG12  33
#define TAG_VFLAG13  34
#define TAG_VFLAG14  35
#define TAG_VFLAG15  36

/* 37-52: VREG0-VREG15 (internal virtual registers, i32; timers, scratch
 * results), kind = TAG_VREG. Lost on power loss/reset -- use the
 * TAG_VREG_RETAIN range below for anything that must survive a reset
 * (e.g. production counters). */
#define TAG_VREG0   37
#define TAG_VREG1   38
#define TAG_VREG2   39
#define TAG_VREG3   40
#define TAG_VREG4   41
#define TAG_VREG5   42
#define TAG_VREG6   43
#define TAG_VREG7   44
#define TAG_VREG8   45
#define TAG_VREG9   46
#define TAG_VREG10  47
#define TAG_VREG11  48
#define TAG_VREG12  49
#define TAG_VREG13  50
#define TAG_VREG14  51
#define TAG_VREG15  52

/* 53-68: VREG_R0-VREG_R15 (retained virtual registers; production counts,
 * accumulators), kind = TAG_VREG_RETAIN. Survives power loss/reset via the
 * Flash retain snapshot (see docs/architecture.md, plc_retain.c, not yet
 * written). */
#define TAG_VREG_R0   53
#define TAG_VREG_R1   54
#define TAG_VREG_R2   55
#define TAG_VREG_R3   56
#define TAG_VREG_R4   57
#define TAG_VREG_R5   58
#define TAG_VREG_R6   59
#define TAG_VREG_R7   60
#define TAG_VREG_R8   61
#define TAG_VREG_R9   62
#define TAG_VREG_R10  63
#define TAG_VREG_R11  64
#define TAG_VREG_R12  65
#define TAG_VREG_R13  66
#define TAG_VREG_R14  67
#define TAG_VREG_R15  68

/*
 * 69-127 (59 slots): reserved for a future Gateway SKU's remote Modbus
 * tags (TAG_MB_COIL / TAG_MB_HOLDING). Unused by this SKU -- no #define
 * here on purpose. g_tag_table[69..127] stays TAG_NONE (all-zero) after
 * tag_table_load_from_flash() on a Remote I/O board; do not read/write
 * these indices from Remote I/O code.
 */

#endif /* PLC_TAG_DEF_H */