# SimplePLC

A portable, layered firmware library implementing a lightweight PLC-style
Rule Engine for industrial IoT devices — starting with a Remote I/O SKU
(8DI/8DO/4AI) on STM32H523CCU6, designed from day one to be reused across a
future product family (Datalogger, Gateway, Controller) with minimal per-SKU
code.

## What this is

SimplePLC lets a device run a table of simple `IF trigger THEN action` rules
against a flat table of "tags" (digital inputs/outputs, analog inputs,
internal flags/registers). Rules are configured externally by a companion
App and pushed to the device over Modbus RTU; the device itself only
executes them — it never needs to understand what a tag or rule "means" in
the real world.

```
Trigger (edge / compare / dwell) -> Guard (optional) -> Action
```

A fixed-cycle scan loop (10 ms) drives everything:

```
input_scan() -> rule_scan() -> output_scan() -> modbus_config_service() -> retain_service()
```

## Design principles

- **No dynamic heap.** Every struct has a fixed, compile-time size — required
  to stay portable down to smaller MCUs (STM32G0, STM32F1) later.
- **Layered, one-directional includes.** Higher layers call lower layers,
  never the reverse. See [`docs/architecture.md`](simple_plc/docs/architecture.md)
  for the full 7-layer breakdown (Utils -> Platform -> SX Driver Core ->
  PLC Core -> Protocol Porting -> PLC Application Services -> Engine).
- **Pull, not push.** Writing a value never triggers a cascade of side
  effects — a separate, independently-scheduled step always polls for
  changes later. This keeps behavior deterministic and easy to reason about.
- **Layer 2 (`core/`) is the build/port boundary.** The Tag Table and Rule
  Engine include nothing from the hardware layers, so they compile and unit
  test on a plain PC — no target hardware required.
- **Adding a new chip means adding a new file, not editing existing ones.**
  Platform selection happens once, via a single macro in `sx_config.h`.

## Repository layout

```
simple_plc/
├── app/          Layer 4 — engine entry point, scan loop wiring
├── services/     Layer 3 — PLC application services (I/O scan, retain
│                 storage, Modbus config service)
├── port/         Layer 3.5 — thin adapters between an external library
│                 (e.g. nanoMODBUS) and the hardware driver layer
├── core/         Layer 2 — Tag Table + Rule Engine (the portable core)
├── components/   Layer 1 — SX Driver Core: chip-agnostic driver contracts
├── platforms/    Layer 0 — the ONLY layer allowed to include vendor HAL
├── utils/        Layer U — pure algorithms with no hardware dependency
│                 (ring buffer, filters, CRC-16/MODBUS)
├── libs/         Third-party libraries (nanoMODBUS as a git submodule)
└── docs/
    └── architecture.md   Full layered architecture reference — read this
                            first before touching any code
```

## Core concepts

**Tag** — a single addressable value (0–127, `MAX_TAGS=128`), tagged with a
kind (`TAG_DI`, `TAG_DO`, `TAG_AI`, `TAG_VFLAG`, `TAG_VREG`,
`TAG_VREG_RETAIN`, `TAG_MB_COIL`, `TAG_MB_HOLDING`). All tags live in one
flat `int32_t[128]` array — the Rule Engine never needs to know whether a
tag is a physical pin or a purely internal flag.

**Rule** — a 32-byte record (`trigger_tag`, `compare_op`, `for_ms` dwell,
`guard_tag`, `action_type`, `action_param`, plus a reserved tail for future
protocol extensions) describing one `IF ... THEN ...` statement. Up to 100
rules (`MAX_RULES=100`) run every scan cycle via an explicit state machine
(`IDLE -> TRIGGERED -> COMPARED -> DWELLING -> GUARD_CHECK -> FIRE`).

**Modbus over USB** — the App configures the device over Modbus RTU carried
on USB-CDC (TinyUSB), using a staged-upload + CRC-16 verify + atomic-commit
protocol so a dropped connection mid-upload never corrupts the rule table
that's currently running. See `docs/architecture.md` §2.6 for the full
official register map.

## Status

Early-stage. Layer 2 (Tag/Rule core) is implemented and unit-tested
independently of hardware. Layers 0/1/3/3.5/4 are in progress — see the
"still open" checklist in `docs/architecture.md` §10 for exactly what's
missing (Rule Table Flash location, Gateway Modbus Master role, Event Log,
Alarm mechanism, RTC source, system command handling).

## Getting started

This is a library, not a standalone firmware image — it's meant to be
consumed by a per-product application that supplies its own Tag Table
definition (which tag index means what for that specific board) and pin
mapping, then links against `core/`, `components/`, and the appropriate
`platforms/<family>/<chip>/` implementation.

Read `docs/architecture.md` in full before writing any code against this
repo — it documents every layer boundary, the exact wire format, and a
running list of decisions already made (so you don't re-litigate them) and
decisions still open (so you don't assume something is settled that isn't).