# AGENTS: ReLimiter dependencies

This directory contains external git submodules: ReShade, NVAPI, Vulkan-Headers, and PresentMon.

- Do not edit, reformat, regenerate, or update dependency contents or revisions for a first-party ReLimiter fix unless dependency work is explicitly requested.
- Prefer first-party adapters, compatibility shims, compile definitions, or narrowly scoped CMake integration outside the submodule over patching upstream code.
- Preserve upstream ABI, headers, licenses, and local conventions. Do not copy dependency implementation code into `src/`.
- If dependency work is requested, identify the owning upstream project, keep the patch isolated, and validate both the affected dependency boundary and ReLimiter targets that consume it.