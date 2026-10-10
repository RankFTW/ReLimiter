# AGENTS: ReLimiter workflow skills

This directory contains on-demand procedures for specialized repository tasks. Follow root `AGENTS.md` as the shared baseline.

- Use lowercase kebab-case skill folders and keep the frontmatter `name` identical to the folder name.
- Make `description` the discovery surface: state what the skill does and include concrete trigger phrases.
- Keep `SKILL.md` procedural and under 500 lines. Move detailed semantics, policies, and templates into one-level-deep `references/` or `assets/` files.
- Keep scripts deterministic, non-destructive, and usable from the repository root. Prefer the Python standard library unless a dependency is already established and materially improves the workflow.
- Never modify raw captures. Write derived artifacts to an explicit output location and disclose every filter, threshold, assumption, and excluded sample count.
- Treat current source and CMake as authoritative. Update skill references and bundled tools when production telemetry schemas or semantics change.
- Test bundled scripts against small synthetic inputs and at least one representative repository artifact when available. Do not use historical captures as golden behavioral truth.
- Catalog every skill and its handoff boundaries in `.agents/skills/README.md`.