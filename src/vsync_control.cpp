#include "vsync_control.h"
#include "config.h"
#include "hooks.h"
#include "swapchain_manager.h"
#include "logger.h"
#include <dxgi.h>
#include <atomic>

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

// ═══════════════════════════════════════════
// OpenGL VSync hook (via wglSwapBuffers)
// ═══════════════════════════════════════════
//
// Strategy: hook wglSwapBuffers (a standard opengl32.dll export, always
// hookable via GetProcAddress). On the first call we're guaranteed to be
// on the GL thread with an active context, so wglGetProcAddress can
// resolve wglSwapIntervalEXT. We resolve it once, then call it to apply
// the VSync override. On subsequent calls we just check if the mode changed.

typedef BOOL(WINAPI* PFN_wglSwapBuffers)(HDC hdc);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int interval);
typedef int(WINAPI* PFN_wglGetSwapIntervalEXT)();
typedef void*(WINAPI* PFN_wglGetProcAddress)(const char*);

static PFN_wglSwapBuffers s_orig_wglSwapBuffers = nullptr;
static PFN_wglSwapIntervalEXT s_real_wglSwapIntervalEXT = nullptr;
static bool s_gl_hooks_installed = false;
static bool s_gl_extension_resolved = false;
static std::atomic<int> s_game_requested_interval{1};
static std::string s_last_applied_mode;

static void ResolveAndApplyGLVSync() {
    if (!s_gl_extension_resolved) {
        s_gl_extension_resolved = true;

        HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
        if (!opengl32) return;

        auto wglGetProc = reinterpret_cast<PFN_wglGetProcAddress>(
            GetProcAddress(opengl32, "wglGetProcAddress"));
        if (wglGetProc) {
            s_real_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                wglGetProc("wglSwapIntervalEXT"));
        }

        if (s_real_wglSwapIntervalEXT) {
            LOG_INFO("VSync: resolved wglSwapIntervalEXT at %p (from wglSwapBuffers hook)",
                     s_real_wglSwapIntervalEXT);
        } else {
            LOG_WARN("VSync: wglSwapIntervalEXT not available on this GL context");
        }
    }

    if (!s_real_wglSwapIntervalEXT) return;

    // Re-apply VSync override periodically. Some games (especially SDL-based)
    // call wglSwapIntervalEXT on focus regain, overriding our setting.
    // Re-enforce every 60 frames (~1 second) to counteract this.
    static int s_reapply_counter = 0;
    bool force_reapply = (++s_reapply_counter >= 60);
    if (force_reapply) s_reapply_counter = 0;

    if (g_config.vsync_mode == s_last_applied_mode && !force_reapply) return;
    s_last_applied_mode = g_config.vsync_mode;

    if (g_config.vsync_mode == "off") {
        s_real_wglSwapIntervalEXT(0);
        if (!force_reapply) LOG_INFO("VSync: applied GL interval=0 (off)");
    } else if (g_config.vsync_mode == "on") {
        s_real_wglSwapIntervalEXT(1);
        if (!force_reapply) LOG_INFO("VSync: applied GL interval=1 (on)");
    } else {
        int game_val = s_game_requested_interval.load(std::memory_order_relaxed);
        s_real_wglSwapIntervalEXT(game_val);
        if (!force_reapply) LOG_INFO("VSync: restored GL interval=%d (game)", game_val);
    }
}

static BOOL WINAPI Hooked_wglSwapBuffers(HDC hdc) {
    // Resolve extension and apply VSync on first call (and on mode changes)
    ResolveAndApplyGLVSync();

    // Call the real wglSwapBuffers
    return s_orig_wglSwapBuffers(hdc);
}

void VSync_InstallOpenGLHooks() {
    if (s_gl_hooks_installed) return;

    LOG_INFO("VSync: attempting gdi32!SwapBuffers hook...");

    // Hook SwapBuffers in gdi32.dll — this is the actual Win32 function
    // every OpenGL app calls to present frames. opengl32.dll's wglSwapBuffers
    // is an internal function that may not be exported, especially when
    // ReShade acts as an opengl32.dll proxy.
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32) {
        LOG_WARN("VSync: gdi32.dll not loaded, skipping GL hooks");
        return;
    }

    void* swap_buffers_addr = GetProcAddress(gdi32, "SwapBuffers");
    LOG_INFO("VSync: gdi32=%p, SwapBuffers=%p", gdi32, swap_buffers_addr);

    if (swap_buffers_addr) {
        MH_STATUS st = InstallHook(swap_buffers_addr,
                                   reinterpret_cast<void*>(Hooked_wglSwapBuffers),
                                   reinterpret_cast<void**>(&s_orig_wglSwapBuffers));
        if (st == MH_OK) {
            LOG_INFO("VSync: hooked gdi32!SwapBuffers at %p", swap_buffers_addr);
            s_gl_hooks_installed = true;
        } else {
            LOG_WARN("VSync: failed to hook SwapBuffers (status=%d)", st);
        }
    } else {
        LOG_WARN("VSync: SwapBuffers not found in gdi32.dll");
    }
}

void VSync_ApplyOpenGL() {
    // Force re-apply on next wglSwapBuffers call by clearing the last mode
    s_last_applied_mode.clear();
}

// ═══════════════════════════════════════════
// DXGI VSync hook (IDXGISwapChain::Present)
// ═══════════════════════════════════════════

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain* self, UINT SyncInterval, UINT Flags);
static PFN_Present s_orig_Present = nullptr;
static bool s_dxgi_hooks_installed = false;
static std::atomic<UINT> s_game_sync_interval{1};

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    s_game_sync_interval.store(SyncInterval, std::memory_order_relaxed);

    UINT override_interval = SyncInterval;
    UINT override_flags = Flags;

    if (g_config.vsync_mode == "off") {
        override_interval = 0;
        // Add ALLOW_TEARING if the swapchain supports it
        override_flags |= DXGI_PRESENT_ALLOW_TEARING;
        static bool s_off_logged = false;
        if (!s_off_logged) {
            s_off_logged = true;
            LOG_INFO("VSync: first present with mode=off (interval=0, ALLOW_TEARING added, "
                     "game_interval=%u, game_flags=0x%X)", SyncInterval, Flags);
        }
    } else if (g_config.vsync_mode == "on") {
        override_interval = 1;
        // Strip ALLOW_TEARING — incompatible with sync_interval > 0
        override_flags &= ~DXGI_PRESENT_ALLOW_TEARING;
    }

    HRESULT hr = s_orig_Present(self, override_interval, override_flags);

    // If ALLOW_TEARING failed (swapchain doesn't support it), retry without
    if (hr == DXGI_ERROR_INVALID_CALL && g_config.vsync_mode == "off" &&
        (override_flags & DXGI_PRESENT_ALLOW_TEARING)) {
        static bool s_tearing_fail_logged = false;
        if (!s_tearing_fail_logged) {
            s_tearing_fail_logged = true;
            LOG_WARN("VSync: DXGI_PRESENT_ALLOW_TEARING rejected (swapchain missing flag?), "
                     "falling back to SyncInterval=0 without tearing");

            // Log the swapchain's actual flags for diagnosis
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(self->GetDesc(&desc))) {
                LOG_WARN("VSync: swapchain SwapEffect=%d, Flags=0x%X, BufferCount=%u, Windowed=%d",
                         desc.SwapEffect, desc.Flags, desc.BufferCount, desc.Windowed);
            }
        }
        hr = s_orig_Present(self, 0, Flags);
    }

    return hr;
}

void VSync_InstallDXGIHooks(void* native_swapchain) {
    if (s_dxgi_hooks_installed || !native_swapchain) return;

    auto* sc = static_cast<IDXGISwapChain*>(native_swapchain);

    // Get Present from vtable (index 8 in IDXGISwapChain)
    void** vtable = *reinterpret_cast<void***>(sc);
    void* present_addr = vtable[8];

    MH_STATUS st = InstallHook(present_addr,
                               reinterpret_cast<void*>(Hooked_Present),
                               reinterpret_cast<void**>(&s_orig_Present));
    if (st == MH_OK) {
        LOG_INFO("VSync: hooked IDXGISwapChain::Present at %p", present_addr);
        s_dxgi_hooks_installed = true;
        EnableAllHooks();
    } else {
        LOG_WARN("VSync: failed to hook Present (status=%d)", st);
    }
}

int VSync_GetDXGISyncInterval() {
    if (g_config.vsync_mode == "off") return 0;
    if (g_config.vsync_mode == "on") return 1;
    return -1;
}
