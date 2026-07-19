# Experiment pre-registration template

Copy this file to `docs/<EXPERIMENT>.md` before writing any shader or harness
code for a new research bet. `docs/WORLDGI.md` is the reference example: its
acceptance criteria were written before the prototype, which is why a negative
result there was a clean one-day kill instead of an unbounded tuning project.

Small ladder-rung experiments (a knob sweep on an existing flag) do not need a
full document — the Phase 3 ladder in `docs/PLAN.md` covers them. Anything
that adds a flag, a pass, a buffer, or a new estimator term does.

Rules that apply to every experiment regardless of scale:

- Measurement discipline comes from `docs/RESEARCH_LOOP.md`; nothing here may
  weaken it.
- Fill in sections 1–6 before implementation. Sections 7–8 are filled in from
  measured results only.
- Acceptance thresholds, scenarios, and kill conditions may not be edited
  after the first measurement run. If a gate turns out to be wrong, say so in
  the results section and re-register — do not silently move it.
- The experiment ends in one of four states (PLAN §8): **promoted**, **parked
  with a specific missing test**, **killed with evidence**, or **invalid
  because a gate failed**. "Interesting" is not a terminal state.

---

# <Experiment name> (`?<flag>=1`) — design and result

One-paragraph summary: what is being tested, and the current terminal state
once known.

## 1. Hypothesis

The falsifiable claim, in one or two sentences. State *why* this renderer's
structure should make it true — a hypothesis that would be equally true in any
renderer is probably not a voxel-native contribution (PLAN §2.5).

## 2. Denominator

Exactly which comparison decides the result: equal GPU time or equal error,
against which control configuration, on which scenarios. The control must do
identical non-experimental work (cf. `ours` as the screen-only control for
`worldgi`). Equal frame count is not a performance claim.

## 3. Design and invariants

The estimator/merge math, written before code. Name the invariants that keep
the estimator correct — e.g. null outcomes stay in the MIS domain with
confidence, empty state is inert, determinism is preserved with the flag off —
and how each will be verified (bit-identical off-path, byte-identical
first-frame, converged bias check).

## 4. Cost budget

Memory (buffers, worst case), bandwidth, and expected GPU time. If the design
cannot state its budget it is not ready to implement.

## 5. Acceptance criteria

Numbered, machine-checkable where possible, with the harness command for each.
Include, at minimum:

1. Inertness/determinism checks (flag off ⇒ unchanged output).
2. Correctness: converged unbiased-variant agreement with `base` within the
   frozen tolerances (`npm run bench:correctness` path or a dedicated probe).
3. The thesis test itself: the metric, scenarios, frame budgets, seed/repeat
   counts, and the threshold that counts as a win (Phase 3 default: ≥5% better
   HDR-FLIP at matched GPU time, or ≥5% less GPU time at matched HDR-FLIP, on
   ≥2 core scenarios, with no core scenario regressing >2%).

## 6. Kill condition and budget

What result kills the idea, and how much effort it gets before stopping
(e.g. "two materially different designs"). Write the kill condition as
concretely as the win condition.

## 7. Results

Measured only. Tables with the exact commands, hardware, and manifest hashes
behind them. Record failures and surprising observations — a killed
experiment's writeup is evidence, not waste.

## 8. Terminal state and redirect

Promoted / parked (name the missing test) / killed / invalid — plus what the
result says about where the remaining quality or cost actually lives, and the
corresponding `STATUS.md` entry date.
