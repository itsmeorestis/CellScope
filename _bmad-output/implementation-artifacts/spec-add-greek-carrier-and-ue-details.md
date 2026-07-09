---
title: 'Add Greek carrier lookup and richer UE identity display'
type: 'feature'
created: '2026-07-09'
status: 'done'
review_loop_iteration: 0
baseline_commit: 'f1ce24d'
context: []
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** The operator_name() lookup in the LTE engine covers US carriers only; Greek cells show a blank operator name even though SIB1 already provides the PLMN and band. In the UEs panel, decoded device identities (TMSI/IMSI/Contention Resolution) are crammed into one raw string column with no type separation, and uplink traffic is invisible despite being collected.

**Approach:** Add MCC 202 (Greece) MNC-to-carrier mappings to the operator_name() lookup so Cosmote, Vodafone GR, and Nova/Wind are identified. Enhance the UEs table with separate identity-type and identity-value columns, expose UL volume, and show per-UE activity freshness so the user can tell which devices are currently active at a glance.

## Boundaries & Constraints

**Always:**
- Greek carrier names must be in English: "Cosmote", "Vodafone GR", "Nova"
- Identity types in the table must use short column-friendly labels ("TMSI", "IMSI", "CR" for Contention Resolution)
- UL data columns must use the same unit formatting as existing DL columns (lteFmtRate helper)
- The operator_name() function signature must not change — only add new case branches
- CellInfo::band is already decoded from SIB1 and displayed in the LTE panel — no band mapping table is needed

**Ask First:**
- None — all decisions are mechanical formatting or well-known PLMN assignments

**Never:**
- Do not add a JSON/CSV carrier database file — inline switch is sufficient for the current scope
- Do not change the UeStat struct or the LTE engine public API
- Do not remove any existing UE table column
- Do not modify the LTESniffer hook or identity extraction logic

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Greek Cosmote cell locked | MCC=202, MNC=01, band from SIB1 | oper="Cosmote", SIB1 line shows "Cosmote PLMN 202-01 ... Band 7" | N/A |
| Greek Vodafone cell locked | MCC=202, MNC=05 | oper="Vodafone GR" | N/A |
| Greek Nova/Wind cell locked | MCC=202, MNC=09 or 10 | oper="Nova" | N/A |
| Non-Greek MCC with MNC not in table | MCC=208 (France) | oper="" (unchanged behavior) | N/A |
| UE with only TMSI decoded | identity="TMSI 0x12AB" | Type col="TMSI", Value col="0x12AB" | N/A |
| UE with IMSI decoded (overrides TMSI) | identity="IMSI 202011234567890" | Type col="IMSI", Value col="202011234567890" | Long IMSI may truncate with ellipsis |
| UE with Contention Resolution only | identity="Contention Resolution 0xABCD" | Type col="CR", Value col="0xABCD" | N/A |
| UE with no identity decoded yet | identity="" | Type col empty, Value col empty | N/A |
| UE last seen > 60s ago | last_seen_ms = now - 65000 | Activity col shows "—" (grey) | N/A |
| UE last seen < 5s ago | last_seen_ms = now - 2000 | Activity col shows "●" (green) | N/A |

</frozen-after-approval>

## Code Map

- `src/lte/lte_engine.cpp:805-825` — `operator_name()` static helper; add MCC 202 branches for Cosmote/Vodafone GR/Nova
- `src/gui/gui_panels.cpp:1583-1751` — `drawLteUes()`; add identity-type/value split, UL volume column, activity indicator column

## Tasks & Acceptance

**Execution:**
- [x] `src/lte/lte_engine.cpp` — Add Greece (MCC 202) to operator_name(): MNC 01/14 → "Cosmote", 05 → "Vodafone GR", 09/10 → "Nova"
- [x] `src/gui/gui_panels.cpp` — Split the single "identity" column in the UEs table into two: "Id type" (TMSI/IMSI/CR) and "Id value" (parsed from the concatenated string)
- [x] `src/gui/gui_panels.cpp` — Add "UL total" column (ul_bytes formatted same as DL total)
- [x] `src/gui/gui_panels.cpp` — Add "Active" column showing a green dot for UEs seen in the last 5s, grey dash otherwise

**Acceptance Criteria:**
- Given CellScope is decoding a Greek LTE cell with MCC=202 MNC=01, when SIB1 is received, then the LTE panel shows "Cosmote" as the operator name
- Given CellScope is decoding a Greek LTE cell with MCC=202 MNC=05, when SIB1 is received, then the LTE panel shows "Vodafone GR" as the operator name
- Given the UEs panel is open with UEs having decoded identities, when identities are present, then the Id type column shows the identity kind (TMSI/IMSI/CR) and the Id value column shows the raw value
- Given the UEs panel is open with UEs having uplink traffic, when UL bytes > 0, then the UL total column shows the formatted byte count
- Given the UEs panel is open, when a UE was last seen within the last 5 seconds, then its Active column shows a green indicator

## Verification

**Commands:**
- Build and confirm compilation succeeds (CMake + Ninja)
- Run CellScope against a known Greek LTE signal and verify operator name appears in the SIB1 line
- Verify the UEs panel renders the new columns without layout issues at default window size

## Spec Change Log

**Review loop 1 (2026-07-09):**
- Finding: Active column (index 1) had no sort case, clicking its header silently fell through to default (d=0) with ascending-RNTI tiebreak
- Finding: UL total formatting didn't match DL total — had a raw "B" branch and rendered empty cell when ul_bytes==0, violating spec's "same unit formatting" constraint
- Finding: Identity type/value fallback for unknown types put the full string in the narrow Id type column instead of Id value
- Finding: TMSI/IMSI prefix check didn't verify a space follows, risking misidentification of extensional types like "TMSI-Ext"
- Amendment: Added sort case 1 for Active column; matched UL formatting to DL (removed "B" branch, zero shows "0.0 KB"); flipped fallback to show unknown identities in the value column; changed prefix checks from `compare(0,4,"TMSI")` to `compare(0,5,"TMSI ")` with a size guard on value offset
- KEEP: Greek carrier lookup (all MNC mappings, default "Greece" return); Active column green/grey indicator with 5s threshold; UL total column placement and WidthFixed at 90px; split identity columns (Id type + Id value)

## Suggested Review Order

**Carrier lookup — entry point**

- Straightforward MCC-MNC switch case added inline with the existing US/UK/DE/CA/AU pattern; check MNC mappings
  [`lte_engine.cpp:820`](../../src/lte/lte_engine.cpp#L820)

**UE table — column layout and sort**

- Table grew from 7 to 10 columns; verify new column indices match sort cases and rendering order
  [`gui_panels.cpp:1651`](../../src/gui/gui_panels.cpp#L1651)
- Sort switch updated for renumbered columns + new Active (1), Id type (8), Id value (9) cases
  [`gui_panels.cpp:1693`](../../src/gui/gui_panels.cpp#L1693)

**UE table — row rendering**

- Activity indicator with 5s threshold; green dot vs grey dash per review feedback
  [`gui_panels.cpp:1745`](../../src/gui/gui_panels.cpp#L1745)
- UL total column matching DL formatting (KB/MB, no raw bytes); zero shows "0.0 KB"
  [`gui_panels.cpp:1769`](../../src/gui/gui_panels.cpp#L1769)
- Identity split: space-delimited prefix match with size guard, fallback shows raw string in value column
  [`gui_panels.cpp:1783`](../../src/gui/gui_panels.cpp#L1783)
