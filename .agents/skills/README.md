# ReLimiter workflow skills

Project skills provide on-demand procedures that are too specialized for always-on `AGENTS.md` guidance.

| Skill | Use for | Handoff boundary |
|---|---|---|
| [`pacing-capture-analysis`](./pacing-capture-analysis/SKILL.md) | Safely archiving, inspecting, or comparing offline ReLimiter joined pacing rows (`relimiter_pacing_*`), pacing events (`relimiter_pacing_events_*`), and FrameView CSV data; displayed-frame pacing, jitter, animation error, latency, PC Latency, GPU residence and CPU holds, stutter, FG/DMFG; the game's render pipeline from Reflex markers (`render_pipeline.py`: simulation, render submission, Present wait, release-rule replays); the regression capture set for release-point changes (`references/capture-protocol.md`) | Provides a refusal-safe immutable archiver plus capture-backed evidence, warnings, and conclusions. Replays only reject release rules; production changes then follow `src/AGENTS.md` and need a live capture. |

Read the matching `SKILL.md` before planning or acting. Historical scripts under `debug_data/scripts/` are investigation evidence, not workflow specifications.
