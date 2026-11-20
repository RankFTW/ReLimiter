#pragma once

#include <algorithm>
#include <cstdint>

// Repeated options must not overwrite a later GetState status. A new
// configuration (including first publication after reset) may infer activity,
// but never proves presentation or supplies frame identity.
inline bool FrameGeneration_ConfiguredActivity(
    bool previous_active, bool configuration_changed,
    uint32_t mode, int generated_frames) noexcept {
    if (!configuration_changed) return previous_active;
    return (mode == 1 || mode == 2) && generated_frames > 0;
}

// ── Streamline's in-band statement (NvAPI_D3D_SetReflexSync, fgMultiplier) ──
// DLSS-G's own module states on every real frame the multiplier it tells the driver to use: 1 while nothing is
// generated, N with N-times generation; 0 means "the driver auto-detects" and states nothing, and so does a disable
// call. The fold keeps the newest stated value, how many calls in a row stated it (saturating) and the Streamline
// lifetime it belongs to in ONE 64-bit word, so a reader never combines two publications. A value is accepted once
// kFgStatementAgreement calls in a row stated it: one call cannot change the state (a one-frame value, as GetState
// shows in a game that creates the feature every frame).
struct FgStatementWord {
    uint32_t value = 0;    // the newest stated multiplier, 1 to kFgStatementMaxValue; 0 = none in this lifetime
    uint32_t run = 0;      // calls in a row that stated it, saturating at 255
    uint64_t lifetime = 0; // the Streamline lifetime generation, low 40 bits
};
inline constexpr uint32_t kFgStatementAgreement = 2;
inline constexpr uint32_t kFgStatementMaxValue = 15;
inline constexpr uint64_t kFgStatementLifetimeMask = (uint64_t{1} << 40) - 1;

inline uint64_t FgStatement_Pack(const FgStatementWord& word) noexcept {
    return (uint64_t{word.value} & 0xFFu) | ((uint64_t{std::min<uint32_t>(word.run, 255u)} & 0xFFu) << 8) |
        ((word.lifetime & kFgStatementLifetimeMask) << 16);
}
inline FgStatementWord FgStatement_Unpack(uint64_t raw) noexcept {
    FgStatementWord word;
    word.value = static_cast<uint32_t>(raw & 0xFFu);
    word.run = static_cast<uint32_t>((raw >> 8) & 0xFFu);
    word.lifetime = (raw >> 16) & kFgStatementLifetimeMask;
    return word;
}
// Whether one call states a multiplier at all.
inline bool FgStatement_States(uint32_t stated, bool disable) noexcept {
    return !disable && stated >= 1 && stated <= kFgStatementMaxValue;
}
// One call that states a multiplier, folded into the word (callers skip calls that state nothing). A call of another
// lifetime starts that lifetime's run.
inline uint64_t FgStatement_Fold(uint64_t raw, uint32_t stated, uint64_t lifetime) noexcept {
    FgStatementWord word = FgStatement_Unpack(raw);
    lifetime &= kFgStatementLifetimeMask;
    if (word.lifetime != lifetime || word.value != stated) {
        word.value = stated;
        word.run = 1;
        word.lifetime = lifetime;
    } else if (word.run < 255) {
        ++word.run;
    }
    return FgStatement_Pack(word);
}
// The accepted value after reading `raw` in `lifetime`: the word's value once its run reaches the agreement, otherwise
// the value accepted before in the same lifetime (0 = none).
inline uint32_t FgStatement_Accept(uint64_t raw, uint64_t lifetime, uint32_t previous_accepted,
    uint64_t previous_lifetime) noexcept {
    const FgStatementWord word = FgStatement_Unpack(raw);
    lifetime &= kFgStatementLifetimeMask;
    if (word.lifetime != lifetime || word.value == 0) return 0;
    if (word.run >= kFgStatementAgreement) return word.value;
    return (previous_lifetime & kFgStatementLifetimeMask) == lifetime ? previous_accepted : 0;
}

// ── One authority for frame generation's state ──
// The facts of each source, kept apart: Streamline's options (SetOptions: mode, configured generated frames), the
// legacy evidence (GetState, the deferred inference: presenting, actual multiplier), the in-band statement while it
// is fresh, and Dynamic MFG (DRS override or the passthrough setting). The resolver decides; consumers read only its
// result (the published globals in streamline_hooks.h).
struct FgFacts {
    bool mode_known = false;          // SetOptions was seen in this Streamline lifetime
    uint32_t mode = 0;                // its DLSSGMode: 0 off, 1 on, 2 auto, 3 dynamic
    int configured_generated = 0;     // its numFramesToGenerate
    bool legacy_presenting = false;   // GetState / the deferred inference / NGX evidence
    int legacy_actual = 0;            // GetState's numFramesActuallyPresented
    bool statement_fresh = false;     // the accepted in-band statement is current
    uint32_t statement = 0;           // the accepted stated multiplier
    bool dmfg = false;                // Dynamic MFG passthrough
};
struct FgEffective {
    bool presenting = false;          // generated frames are presented
    int actual = 0;                   // the multiplier (output frames per real frame) when known, else 0
    int generated = 0;                // generated frames per real frame (the route and the present order key on it)
    bool statement_authority = false; // the in-band statement decided
};
// Explicit Off wins; a fresh statement decides unless Dynamic MFG or Streamline's dynamic mode stands aside (the
// multiplier may change every frame there); otherwise the legacy evidence decides exactly as before.
inline FgEffective FrameGeneration_ResolveState(const FgFacts& facts) noexcept {
    FgEffective effective;
    const int configured = std::clamp(facts.configured_generated, 0, 5);
    if (facts.mode_known && facts.mode == 0) {
        effective.generated = configured;
        return effective;
    }
    const bool stand_aside = facts.dmfg || (facts.mode_known && facts.mode == 3);
    if (facts.statement_fresh && facts.statement >= 1 && !stand_aside) {
        effective.statement_authority = true;
        if (facts.statement >= 2) {
            const int multiplier = static_cast<int>(std::min<uint32_t>(facts.statement, 6u));
            effective.presenting = true;
            effective.actual = multiplier;
            effective.generated = multiplier - 1;
        } else {
            effective.actual = 1; // Streamline states that nothing is generated
        }
        return effective;
    }
    effective.presenting = facts.legacy_presenting;
    effective.actual = std::clamp(facts.legacy_actual, 0, 6);
    effective.generated = configured;
    return effective;
}

// Why the FG input route cannot take frames. Auto (2) and an unobserved mode
// are accepted; the route keys each input to a multiplier of 2x to 4x.
enum class FgRouteRejection : uint8_t { None, Dmfg, Off, Dynamic, NotPresenting, UnsupportedMultiplier };

inline FgRouteRejection FrameGeneration_RouteRejection(bool dmfg, bool mode_known, uint32_t mode,
    bool presenting, int generated) noexcept {
    if (dmfg) return FgRouteRejection::Dmfg;
    if (mode_known && mode == 0) return FgRouteRejection::Off;
    if (mode == 3) return FgRouteRejection::Dynamic;
    if (!presenting) return FgRouteRejection::NotPresenting;
    if (generated < 1 || generated > 3) return FgRouteRejection::UnsupportedMultiplier;
    return FgRouteRejection::None;
}

inline const char* FgRouteRejection_Text(FgRouteRejection reason) noexcept {
    switch (reason) {
        case FgRouteRejection::None: return "eligible";
        case FgRouteRejection::Dmfg: return "dmfg_passthrough";
        case FgRouteRejection::Off: return "mode_off";
        case FgRouteRejection::Dynamic: return "mode_dynamic";
        case FgRouteRejection::NotPresenting: return "fg_not_presenting";
        case FgRouteRejection::UnsupportedMultiplier: return "unsupported_multiplier";
    }
    return "unknown";
}
