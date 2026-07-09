- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: Document implicit coupling between fc and tw in DecimFir::init D==1 branch (both derive from ratio, algebraic simplification obscured)
  evidence: Blind Hunter review: "A future maintainer tweaking fc without realizing they have simultaneously shrunk the transition band could produce an accidentally undersized filter again."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: D>=2 branch anti-alias filters are undersized for small D (same aliasing class as fixed D==1 bug)
  evidence: Blind Hunter review: "At D=2 (ntaps=17), the output Nyquist falls inside the filter's transition band, getting at most 10-15 dB of suppression."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: Document resamp_mtx -> ring_mtx lock ordering to prevent future deadlocks
  evidence: Blind Hunter review: "Any future code path that acquires ring_mtx before resamp_mtx would deadlock."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: Auto-center PPM correction accumulates unbounded from CFO estimator noise, risking frequency drift over time
  evidence: Blind Hunter review: "Over an hour, accumulated PPM value could drift by hundreds of ppm purely from estimation noise."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: CellInfo::decode_rate_hz is misnamed — it holds occupied bandwidth (PRB*180kHz), not the sample rate needed for decode
  evidence: Blind Hunter review: "For a 100-PRB cell, decode_rate_hz=18 MHz but srsRAN needs 30.72 MHz. The misleading name is a trap for future development."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: Watched RNTI history not cleared on re-acquire, causing stale data from previous cell incarnation to persist
  evidence: Blind Hunter review: "setWatched() only prunes history for RNTIs no longer watched, not for RNTIs persisting across re-acquires."

- source_spec: `_bmad-output/implementation-artifacts/spec-fix-decimfir-antialias-filter.md`
  summary: Race window where have_cell=true but decode_rate_hz is still 0.0 before run_decode writes it
  evidence: Blind Hunter review: "decode_rate_hz is set inside run_decode() which runs on the worker thread after have_cell is already true; the GUI could read the stale zero."
