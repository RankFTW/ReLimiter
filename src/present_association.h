#pragma once

// One PresentMon observation per PresentEvent, built by the native PCL
// collector for the pacing display samples of that present, and its
// real/generated frame classification. Enum values are first-party wire
// values mapped explicitly from PresentMon, never the dependency's numbering.
//
// Frame generation (DLSS-G): one app frame produces several presents of one
// swapchain. The app frame's PCL simulation marker is attached to the FIRST
// of them, but that present is the interpolated frame G(N-1,N), shown as
// soon as generation finishes; the app frame's own present follows it
// without a marker. PresentFrameGrouper below groups presents by marker and
// names the last present of each group Real and the earlier ones Generated.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

enum class PresentObservationFinalState : uint8_t {
    Unknown = 0,
    Presented = 1,
    Discarded = 2
};

enum class PresentFrameClassification : uint8_t {
    Unknown = 0,
    Real = 1,
    Generated = 2,
    Conflicting = 3
};

// Unknown is retained, not guessed.
enum class PresentObservationMode : uint8_t {
    Unknown, HardwareLegacyFlip, HardwareLegacyCopy, HardwareIndependentFlip,
    ComposedFlip, ComposedCopyGpuGdi, ComposedCopyCpuGdi, HardwareComposedIndependentFlip
};

enum PresentClassificationEvidence : uint32_t {
    PRESENT_CLASSIFICATION_EVIDENCE_NONE = 0,
    PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER = 1u << 0,
    PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK = 1u << 1,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION = 1u << 2,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED = 1u << 3,
    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED = 1u << 4,
    PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES = 1u << 5,
    // An earlier present of the same app frame (PresentFrameGrouper).
    PRESENT_CLASSIFICATION_EVIDENCE_EARLIER_PRESENT_OF_FRAME = 1u << 6
};

// Independent of the number of displayed instances carried by the event.
// PresentMon identities are observer aliases scoped by observer_epoch.
struct PresentTimelineObservation {
    uint32_t observer_epoch = 0;
    PresentObservationFinalState final_state =
        PresentObservationFinalState::Unknown;
    uint64_t native_swapchain = 0;
    uint32_t thread_id = 0;
    uint64_t present_start_qpc = 0;
    uint64_t present_end_qpc = 0;
    uint64_t report_qpc = 0;
    uint64_t qpc_frequency = 0;
    uint32_t displayed_instance_count = 0;
    uint32_t pcl_frame_id = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t gpu_ready_qpc = 0;
    PresentFrameClassification frame_classification =
        PresentFrameClassification::Unknown;
    PresentObservationMode presentation_mode = PresentObservationMode::Unknown;
    int32_t observed_sync_interval = -1;
    uint32_t observed_present_flags = 0;
    bool supports_tearing = false;
    // PresentMon has no validity bit: zero may mean unavailable or zero delay.
    uint64_t nvidia_flip_delay_qpc = 0;
};

// Real evidence and generated evidence together are Conflicting.
PresentFrameClassification PresentAssociation_ClassifyFrame(
    uint32_t evidence) noexcept;

// The app frame identity a present's pacing samples and PC Latency use.
// PresentMon attaches it to the present carrying the PCL simulation marker;
// under frame generation that is the generated frame, so the collector moves
// it to the real present of the group.
struct PresentAppIdentity {
    uint32_t pcl_frame_id = 0;
    uint64_t pcl_sim_start_qpc = 0;
    uint64_t pcl_input_ping_qpc = 0;
    uint64_t pcl_present_start_qpc = 0;
    uint64_t app_sim_start_qpc = 0;
    // Simulation start a present without its own marker reuses; 0 means the
    // chain's last simulation start.
    uint64_t fallback_sim_start_qpc = 0;
};

enum class PresentGroupRole : uint8_t {
    // No group inference: classification and identity are the present's own.
    Standalone = 0,
    // Earlier present of a multi-present app frame: no identity of its own.
    Generated = 1,
    // Last present of a multi-present app frame: carries the frame identity.
    Real = 2
};

// Standalone keeps own; Real takes head (the group's marker present);
// Generated carries none and reuses head's simulation start as fallback.
PresentAppIdentity PresentAssociation_EffectiveIdentity(
    PresentGroupRole role,
    const PresentAppIdentity& own,
    const PresentAppIdentity& head) noexcept;

template <typename Payload>
struct PresentGroupResolved {
    Payload present{};
    // The group's marker present; the present itself when Standalone.
    Payload head{};
    PresentGroupRole role = PresentGroupRole::Standalone;
};

// Per-swapchain, deterministic grouping of presents into app frames. A group
// opens at a present carrying a PCL simulation marker and holds it and the
// markerless presents that follow, until the next marker. The last held
// present can only be known once the group closes, so presents are held (in
// arrival order, at most Capacity) and resolved when the next marker present
// arrives or the group goes stale. A group of one present is Standalone:
// without frame generation nothing is inferred.
//
// PresentMon frame types stay authoritative: a present with one (Application,
// Repeated, XeFG, AFMF) is never held, never reassigned, and makes its group
// pass through unchanged.
template <typename Payload, size_t Capacity = 8>
class PresentFrameGrouper
{
public:
    using Resolved = PresentGroupResolved<Payload>;

    // Resolved presents are appended to *out in arrival order.
    void Add(
        const Payload& present,
        bool carries_marker,
        bool presentmon_typed,
        uint64_t now_qpc,
        std::vector<Resolved>* out)
    {
        if (carries_marker) {
            ResolvePending(out);
            open_ = true;
            passthrough_ = presentmon_typed;
            if (passthrough_) {
                Emit(present, present, PresentGroupRole::Standalone, out);
            }
            else {
                Hold(present, now_qpc);
            }
            return;
        }

        if (!open_ || passthrough_) {
            Emit(present, present, PresentGroupRole::Standalone, out);
            return;
        }
        if (presentmon_typed || count_ == Capacity) {
            // PresentMon's own type, or a group no marker has closed in
            // Capacity presents: nothing inferred for this group.
            ReleasePendingStandalone(out);
            passthrough_ = true;
            Emit(present, present, PresentGroupRole::Standalone, out);
            return;
        }
        Hold(present, now_qpc);
    }

    // A group whose last present arrived limit_qpc ago or more is taken as
    // complete. Presents of it that arrive later are Standalone.
    void FlushIfStale(uint64_t now_qpc, uint64_t limit_qpc, std::vector<Resolved>* out)
    {
        if (count_ == 0 || now_qpc < last_hold_qpc_ || now_qpc - last_hold_qpc_ < limit_qpc) return;
        ResolvePending(out);
        open_ = false;
    }

    size_t PendingCount() const noexcept { return count_; }

private:
    void Hold(const Payload& present, uint64_t now_qpc)
    {
        pending_[count_++] = present;
        last_hold_qpc_ = now_qpc;
    }

    static void Emit(
        const Payload& present, const Payload& head, PresentGroupRole role, std::vector<Resolved>* out)
    {
        Resolved resolved;
        resolved.present = present;
        resolved.head = head;
        resolved.role = role;
        out->push_back(std::move(resolved));
    }

    void ResolvePending(std::vector<Resolved>* out)
    {
        for (size_t index = 0; index < count_; ++index) {
            const PresentGroupRole role = count_ == 1 ? PresentGroupRole::Standalone
                : index + 1 == count_ ? PresentGroupRole::Real
                                      : PresentGroupRole::Generated;
            Emit(pending_[index], pending_[0], role, out);
        }
        ClearPending();
    }

    void ReleasePendingStandalone(std::vector<Resolved>* out)
    {
        for (size_t index = 0; index < count_; ++index) {
            Emit(pending_[index], pending_[index], PresentGroupRole::Standalone, out);
        }
        ClearPending();
    }

    void ClearPending() noexcept
    {
        for (size_t index = 0; index < count_; ++index) pending_[index] = Payload{};
        count_ = 0;
    }

    std::array<Payload, Capacity> pending_{};
    size_t count_ = 0;
    uint64_t last_hold_qpc_ = 0;
    bool open_ = false;
    bool passthrough_ = false;
};
