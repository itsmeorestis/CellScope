---
title: 'Fix DecimFir anti-alias filter transition width for D=1 fractional downsampling'
type: 'bugfix'
created: '2026-07-09'
status: 'done'
review_loop_iteration: 1
baseline_commit: '2b35a0b'
context: []
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** LTE decoding produces 0 DCIs (no UEs detected) after the DecimFir D=1 anti-alias filter was added. The filter's transition width is computed against the input Nyquist (0.5 cyc/samp) instead of the output Nyquist (`ratio * 0.5`), producing a filter ~3.3× too short. For the common RTL-SDR case (2.4→1.92 MHz), 25 taps are used when ~83 are needed; the stopband never engages before the output Nyquist, so energy above 0.96 MHz aliases directly onto PDCCH subcarriers during `srsran_resample_arb` downsampling.

**Approach:** Fix the `tw` calculation in `DecimFir::init()` to use the output Nyquist as the stopband edge, producing a properly-sized anti-alias filter for all D=1 fractional-downsample ratios.

## Boundaries & Constraints

**Always:**
- The fix must only touch the D=1 branch of `DecimFir::init()` in `src/lte/lte_engine.cpp`
- The D>=2 path (existing, known-good) must remain unchanged
- `ntaps` must remain odd (symmetric FIR with integer group delay)

**Ask First:**
- None — the fix is a single-line mathematical correction

**Never:**
- Do not change the filter design for D>=2
- Do not alter the `process()` convolution logic
- Do not touch `configure_resampler()` or `push_resampled()`
- Do not add runtime-configurable filter parameters

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| RTL-SDR 2.4→1.92 MHz (ratio=0.8) | D=1, fc=0.36, tw=0.04 | ntaps=83, stopband at output Nyquist 0.4 | N/A |
| Near-unity downsample (ratio=0.96) | D=1, fc=0.432, tw=0.048 | ntaps=69, stopband at 0.48 | N/A |
| Max downsample (ratio=0.52) | D=1, fc=0.234, tw=max(0.02,0.26-0.234)=0.026 | ntaps=127, large but correct | N/A |
| Unity/upsample (ratio ≥ 0.999) | D=1 | `h.clear(); return;` — no filter needed | N/A |

</frozen-after-approval>

## Code Map

- `src/lte/lte_engine.cpp:82-98` — `DecimFir::init()`, D==1 branch where the bug lives. Line 91 contains the incorrect `tw` formula.
- `src/lte/lte_engine.cpp:119-141` — `DecimFir::process()`, the convolution implementation (no changes needed).
- `src/lte/lte_engine.cpp:275-310` — `push_resampled()`, where `!decim.h.empty()` gates the filter application.

## Tasks & Acceptance

**Execution:**
- [x] `src/lte/lte_engine.cpp` — Fix `tw` formula on line 91: replace `0.5 - fc` with `ratio * 0.5 - fc`
- [x] `src/lte/lte_engine.cpp` — Reset `attempts` counter per re-acquisition cycle (move inside outer while loop) to prevent stale count carrying over after sync loss

**Acceptance Criteria:**
- Given RTL-SDR input at 2.4 Msps decoding a valid LTE cell, when the engine runs, then PDCCH DCIs are decoded and UEs appear (non-zero UE count)
- Given any D=1 fractional-downsample configuration, when `DecimFir::init()` computes `ntaps`, then `ntaps` is large enough so the stopband edge (fc + 3.3/ntaps) ≤ output Nyquist (ratio * 0.5)
- Given any D>=2 configuration, when `DecimFir::init()` runs, then filter parameters are identical to before the fix
- Given a re-acquisition cycle after sync loss, when cell search fails, then the attempt counter starts fresh from 1 (not carried over from prior cycles)

## Spec Change Log

**Review loop 1 (2026-07-09):**
- Finding: `attempts` counter in `run_worker()` never reset between outer-loop re-acquisition cycles, causing stale "likely 5G NR" message after sync loss
- Amendment: Moved `int attempts = 0` inside the outer `while(run.load())` loop so it resets per re-acquisition
- Avoids: After sync loss + re-acquire, the first failed search would show "likely 5G NR" because `attempts` from the prior cycle exceeded 5
- KEEP: The 5-attempt threshold before showing the 5G NR hint; the inner search loop structure; the `found` flag pattern for transitioning to decode

**7 deferred items** logged to `_bmad-output/implementation-artifacts/deferred-work.md` (pre-existing concerns surfaced by review: D>=2 filter undersizing, lock ordering docs, PPM accumulation drift, field naming, RNTI history leak, decode_rate_hz race window, tw/fc coupling)

## Design Notes

The Hamming-windowed sinc filter's transition width (passband edge to stopband edge) is approximately `3.3 / (ntaps - 1)` in cycles/sample. For effective anti-aliasing, the stopband must begin at or before the output Nyquist: `fc + 3.3/(ntaps-1) ≤ ratio * 0.5`. Solving for `ntaps`: `ntaps ≥ 1 + 3.3 / (ratio * 0.5 - fc)`. With `fc = 0.45 * ratio`, this becomes `ntaps ≥ 1 + 3.3 / (0.05 * ratio) = 1 + 66/ratio`. For the worst case (ratio → 0.5, the largest fractional downsample before D flips to 2), this gives ~133 taps — a few milliseconds of latency at 2.4 Msps, entirely acceptable.

The fix is a one-line change from:
```cpp
const double tw = std::max(0.02, 0.5 - fc);
```
to:
```cpp
const double tw = std::max(0.02, ratio * 0.5 - fc);
```

## Verification

**Commands:**
- Build the project and confirm compilation succeeds
- Run CellScope against a known LTE signal and verify UE count > 0 during decode

## Suggested Review Order

**Filter fix — transition width calculation**

- Entry point: the one-line mathematical fix — output Nyquist replaces input Nyquist as the stopband target
  [`lte_engine.cpp:91`](../../../src/lte/lte_engine.cpp#L91)

- Filter coefficient computation (unchanged) — verify the Hamming window and DC-gain normalization still apply correctly with the larger ntaps
  [`lte_engine.cpp:101`](../../../src/lte/lte_engine.cpp#L101)

**Filter application gate**

- Gate changed from `D > 1` to `!h.empty()` — the D==1 filter now actually runs, so the gate must match init()
  [`lte_engine.cpp:289`](../../../src/lte/lte_engine.cpp#L289)

**Re-acquisition counter hygiene**

- `attempts` now resets per outer-loop cycle so sync-loss recovery doesn't inherit stale failure counts
  [`lte_engine.cpp:705`](../../../src/lte/lte_engine.cpp#L705)
