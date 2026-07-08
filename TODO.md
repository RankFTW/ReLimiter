# ReLimiter TODO

## Bugs
- [ ] **Focus Lock doesn't activate until ReLimiter panel is opened** — The lazy-install logic is inside `DrawSettings()` which only runs when the user opens the ReLimiter tab. Move auto-install to `on_present` or the OSD overlay callback so it fires every frame regardless of UI state. Affects all APIs (64-bit and 32-bit).

## 32-bit
- [ ] **DX9 VSync override requires restart** — Currently modifies `sync_interval` at swapchain creation (`on_create_swapchain`). Could potentially force a `Reset` call to apply immediately, but this is risky for game stability.
- [ ] **Monitor selector doesn't work with DX9 exclusive fullscreen** — `SetWindowPos` forces loss of exclusive mode → minimize loop. Works fine with Fake Fullscreen enabled. Low priority — document as known limitation.
