# AGENTS: ReLimiter documentation

This file adds documentation rules under `docs/`.

- Treat `USER_GUIDE.md` as the comprehensive current user guide and root `README.md` as the concise product/build overview.
- Keep feature names, defaults, ranges, INI keys, OSD labels, API support, and 32/64-bit availability synchronized with current source and CMake guards.
- Write for end users: state prerequisites, restart requirements, risks, fallbacks, and known limitations without exposing unnecessary implementation detail.
- Files named as reports, plans, or prior analyses are historical technical records unless a task explicitly promotes or updates them. Preserve their original context and dates; do not silently rewrite them as current specifications.
- Distinguish confirmed behavior from hypotheses, planned work, and unperformed validation. Do not claim a build, game, API, or GPU path was tested unless it was actually tested.
- When behavior changes, update the smallest relevant current document and `CHANGELOG.md` when release notes are part of the task; do not duplicate the full user guide across multiple files.