#include "pacing_join.h"

namespace
{
    int64_t MillisecondsToTicks(uint64_t milliseconds, uint64_t qpc_frequency) noexcept
    {
        return static_cast<int64_t>(qpc_frequency / 1000 * milliseconds +
            qpc_frequency % 1000 * milliseconds / 1000);
    }

    bool IsMarkerDomain(PacingIdentityDomain domain) noexcept
    {
        return domain == PacingIdentityDomain::NvApiFrame ||
            domain == PacingIdentityDomain::PclFrame;
    }

    template <typename Ring, typename Entry>
    void Retain(Ring* ring, const Entry& entry, uint64_t* capacity_evictions) noexcept
    {
        if (ring->Full()) {
            ring->PopFront();
            ++*capacity_evictions;
        }
        ring->PushBack(entry);
    }

    template <typename Ring>
    void EvictOlderThan(Ring* ring, int64_t now_qpc, int64_t retention_ticks) noexcept
    {
        while (ring->Count() != 0 && now_qpc - ring->Oldest().received_qpc > retention_ticks) {
            ring->PopFront();
        }
    }

    // Newest-first scan; returns the most recent entry accepted by the predicate.
    template <typename Ring, typename Predicate>
    auto FindNewest(const Ring& ring, Predicate predicate) noexcept -> decltype(&ring.Oldest())
    {
        for (uint64_t sequence = ring.TailSequence(); sequence > ring.HeadSequence();) {
            --sequence;
            if (predicate(ring.At(sequence).record)) return &ring.At(sequence);
        }
        return nullptr;
    }

    void MergeGate(const PacingGateRecord& record, PacingGateJoin* gate) noexcept
    {
        if (PacingFrameKey_IsValid(record.key)) gate->key = record.key;
        if (record.event == PacingGateEvent::Signalled || !gate->signalled) gate->status = record.status;
        if (record.fg != 0) gate->fg = 1;
        gate->ticket = record.ticket;
        if (record.native_queue != 0) gate->native_queue = record.native_queue;
        if (record.swapchain_generation != 0) gate->swapchain_generation = record.swapchain_generation;
        if (record.present_sequence != 0) gate->present_sequence = record.present_sequence;
        if (record.target_qpc != 0) gate->target_qpc = record.target_qpc;
        if (record.event == PacingGateEvent::Signalled) {
            gate->signalled = true;
            gate->signal_before_qpc = record.signal_before_qpc;
            gate->signal_after_qpc = record.signal_after_qpc;
        }
        else {
            gate->post_wait = true;
            gate->post_wait_qpc = record.post_wait_qpc;
            gate->pre_wait_qpc = record.pre_wait_qpc;
        }
    }
}

// ── Record intake ──

void PacingJoiner::Reset(uint64_t qpc_frequency) noexcept
{
    qpc_frequency_ = qpc_frequency;
    retention_ticks_ = MillisecondsToTicks(PACING_JOIN_RETENTION_MS, qpc_frequency);
    unjoined_present_ticks_ = MillisecondsToTicks(PACING_JOIN_UNJOINED_PRESENT_MS, qpc_frequency);
    display_grace_ticks_ = MillisecondsToTicks(PACING_JOIN_DISPLAY_GRACE_MS, qpc_frequency);
    sources_.Clear();
    presents_.Clear();
    gates_.Clear();
    readies_.Clear();
    reflexes_.Clear();
    pending_displays_.Clear();
    swapchains_ = {};
    swapchain_use_clock_ = 0;
    unjoined_cursor_ = 0;
    marker_epoch_valid_ = false;
    marker_domain_ = PacingIdentityDomain::Unknown;
    marker_epoch_ = 0;
    latest_present_valid_ = false;
    latest_present_swapchain_ = 0;
    latest_present_received_qpc_ = 0;
    statistics_ = {};
}

void PacingJoiner::AddSource(const PacingSourceRecord& record, int64_t now_qpc) noexcept
{
    if (!PacingFrameKey_IsValid(record.key)) {
        ++statistics_.invalid_records;
        return;
    }
    EvictOlderThan(&sources_, now_qpc, retention_ticks_);
    Retain(&sources_, RetainedSource{ record, now_qpc }, &statistics_.source_capacity_evictions);
    if (IsMarkerDomain(record.key.domain)) {
        marker_epoch_valid_ = true;
        marker_domain_ = record.key.domain;
        marker_epoch_ = record.key.producer_epoch;
    }
}

void PacingJoiner::AddPresent(const PacingPresentRecord& record, int64_t now_qpc) noexcept
{
    latest_present_valid_ = true;
    latest_present_swapchain_ = record.native_swapchain;
    latest_present_received_qpc_ = now_qpc;
    // Aged records leave through TryTakeRow so undecided ones still get a row.
    if (presents_.Full()) {
        const RetainedPresent& oldest = presents_.Oldest();
        if (!oldest.claimed && !oldest.unjoined_emitted) ++statistics_.unjoined_present_drops;
        presents_.PopFront();
        ++statistics_.present_capacity_evictions;
    }
    RetainedPresent entry{};
    entry.record = record;
    entry.received_qpc = now_qpc;
    presents_.PushBack(entry);
}

void PacingJoiner::AddGate(const PacingGateRecord& record, int64_t now_qpc) noexcept
{
    EvictOlderThan(&gates_, now_qpc, retention_ticks_);
    // PostWait follows its Signalled event; merge into the retained ticket.
    if (record.event == PacingGateEvent::PostWait && record.ticket != 0) {
        for (uint64_t sequence = gates_.TailSequence(); sequence > gates_.HeadSequence();) {
            --sequence;
            RetainedGate& entry = gates_.At(sequence);
            if (entry.record.ticket == record.ticket) {
                MergeGate(record, &entry.record);
                return;
            }
        }
    }
    RetainedGate entry{};
    entry.received_qpc = now_qpc;
    MergeGate(record, &entry.record);
    Retain(&gates_, entry, &statistics_.gate_capacity_evictions);
}

void PacingJoiner::AddReady(const PacingReadyRecord& record, int64_t now_qpc) noexcept
{
    EvictOlderThan(&readies_, now_qpc, retention_ticks_);
    Retain(&readies_, RetainedReady{ record, now_qpc }, &statistics_.ready_capacity_evictions);
}

void PacingJoiner::AddReflex(const PacingReflexRecord& record, int64_t now_qpc) noexcept
{
    if (!PacingFrameKey_IsValid(record.key)) {
        ++statistics_.invalid_records;
        return;
    }
    EvictOlderThan(&reflexes_, now_qpc, retention_ticks_);
    Retain(&reflexes_, RetainedReflex{ record, now_qpc }, &statistics_.reflex_capacity_evictions);
}

bool PacingJoiner::AddDisplay(const PacingDisplaySample& sample, int64_t now_qpc) noexcept
{
    return AddDisplay(sample, now_qpc, PacingDisplayContext{});
}

bool PacingJoiner::AddDisplay(const PacingDisplaySample& sample, int64_t now_qpc,
    const PacingDisplayContext& context) noexcept
{
    if (pending_displays_.Full()) {
        ++statistics_.pending_display_drops;
        return false;
    }
    pending_displays_.PushBack(PendingDisplay{ sample, now_qpc, context });
    FindOrAddSwapchain(sample.native_swapchain); // the swapchain now has a display stream
    return true;
}

bool PacingJoiner::LatestPresentSwapchain(
    uint64_t* native_swapchain, int64_t* received_qpc) const noexcept
{
    if (!latest_present_valid_) return false;
    *native_swapchain = latest_present_swapchain_;
    *received_qpc = latest_present_received_qpc_;
    return true;
}

// ── Row emission ──

bool PacingJoiner::TryTakeRow(int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept
{
    EvictAgedRecords(now_qpc);
    if (TakeDisplayRow(now_qpc, final_drain, row)) return true;
    return TakeUnjoinedPresentRow(now_qpc, final_drain, row);
}

void PacingJoiner::EvictAgedRecords(int64_t now_qpc) noexcept
{
    EvictOlderThan(&sources_, now_qpc, retention_ticks_);
    EvictOlderThan(&gates_, now_qpc, retention_ticks_);
    EvictOlderThan(&readies_, now_qpc, retention_ticks_);
    EvictOlderThan(&reflexes_, now_qpc, retention_ticks_);
}

bool PacingJoiner::TakeDisplayRow(int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept
{
    while (pending_displays_.Count() != 0) {
        const PendingDisplay& pending = pending_displays_.Oldest();
        if (!final_drain && now_qpc - pending.received_qpc < display_grace_ticks_) {
            if (!pending_displays_.Full()) return false;
            ++statistics_.pending_display_forced;
        }
        const PacingDisplaySample sample = pending.sample;
        const PacingDisplayContext context = pending.context;
        pending_displays_.PopFront();
        if (sample.qpc_frequency != 0 && sample.qpc_frequency != qpc_frequency_) {
            ++statistics_.clock_mismatches;
            continue;
        }
        BuildDisplayRow(sample, row);
        row->display_continuity_break = context.continuity_break;
        return true;
    }
    return false;
}

bool PacingJoiner::TakeUnjoinedPresentRow(
    int64_t now_qpc, bool final_drain, PacingJoinedRow* row) noexcept
{
    // Retention expiry: an undecided record still gets its row before it leaves.
    while (presents_.Count() != 0 &&
           now_qpc - presents_.Oldest().received_qpc > retention_ticks_) {
        RetainedPresent& oldest = presents_.Oldest();
        const bool undecided = !oldest.claimed && !oldest.unjoined_emitted;
        if (undecided) BuildUnjoinedRow(&oldest, row);
        presents_.PopFront();
        if (undecided) return true;
    }

    if (unjoined_cursor_ < presents_.HeadSequence()) unjoined_cursor_ = presents_.HeadSequence();
    for (uint64_t sequence = unjoined_cursor_; sequence < presents_.TailSequence(); ++sequence) {
        RetainedPresent& entry = presents_.At(sequence);
        const bool decided = entry.claimed || entry.unjoined_emitted;
        if (!decided && !final_drain) {
            // Receipt order is age order: younger records follow.
            if (now_qpc - entry.received_qpc < unjoined_present_ticks_) break;
            if (!DisplayNoLongerExpected(entry.record)) continue; // retention expiry decides
        }
        if (sequence == unjoined_cursor_) ++unjoined_cursor_;
        if (decided) continue;
        BuildUnjoinedRow(&entry, row);
        return true;
    }
    return false;
}

void PacingJoiner::BuildDisplayRow(const PacingDisplaySample& sample, PacingJoinedRow* row) noexcept
{
    *row = PacingJoinedRow{};
    row->row_kind = 1;
    row->has_display = true;
    row->display = sample;
    row->display_source = PacingJoin_DisplaySource(sample);
    ++statistics_.display_samples;

    // Rule 1: exactly one retained Present on the same swapchain and thread
    // whose native call spans the observed present start.
    RetainedPresent* present = FindPresentForDisplay(sample, &row->present_candidates);
    if (row->present_candidates == 0) ++statistics_.present_not_found;
    else if (row->present_candidates > 1) ++statistics_.present_ambiguous;
    else ++statistics_.present_unique;
    if (present != nullptr) {
        row->has_present = true;
        row->present = present->record;
        if (present->unjoined_emitted) {
            row->late_display = true;
            ++statistics_.late_display_joins;
        }
        present->claimed = true;
        const PacingFrameKey& taken = present->record.cross_thread_key;
        if (PacingFrameKey_IsValid(taken) && sample.pcl_frame_id != 0) {
            if (static_cast<uint32_t>(taken.frame_id) == sample.pcl_frame_id) {
                ++statistics_.cross_thread_agreements;
            } else {
                ++statistics_.cross_thread_disagreements;
            }
        }
    }
    SwapchainDisplayState* state = FindOrAddSwapchain(sample.native_swapchain);
    if (sample.present_start_qpc > state->joined_present_start_qpc) {
        state->joined_present_start_qpc = sample.present_start_qpc;
    }

    // Rule 2 applies only when the Present attribution is unavailable.
    if (row->has_present && PacingFrameKey_IsValid(row->present.key)) {
        row->key = row->present.key;
        row->join_rule = PacingJoinRule::PresentAttribution;
        ++statistics_.rule_present_attribution;
        AttachSourceByKey(row);
    }
    else if (const RetainedSource* source = FindSourceByPclFrameId(sample.pcl_frame_id)) {
        row->key = source->record.key;
        row->join_rule = PacingJoinRule::PclFrameId;
        row->has_source = true;
        row->source = source->record;
        ++statistics_.rule_pcl_frame_id;
    }
    else {
        ++statistics_.rule_none;
    }
    AttachWorkerRecords(row);
}

void PacingJoiner::BuildUnjoinedRow(RetainedPresent* entry, PacingJoinedRow* row) noexcept
{
    *row = PacingJoinedRow{};
    row->row_kind = 2;
    row->has_present = true;
    row->present = entry->record;
    if (PacingFrameKey_IsValid(entry->record.key)) {
        row->key = entry->record.key;
        row->join_rule = PacingJoinRule::PresentAttribution;
        AttachSourceByKey(row);
    }
    AttachWorkerRecords(row);
    entry->unjoined_emitted = true;
    ++statistics_.unjoined_presents;
}

// ── Lookups ──

PacingJoiner::RetainedPresent* PacingJoiner::FindPresentForDisplay(
    const PacingDisplaySample& sample, uint32_t* candidates) noexcept
{
    *candidates = 0;
    if (sample.present_start_qpc == 0) return nullptr;
    RetainedPresent* match = nullptr;
    for (uint64_t sequence = presents_.HeadSequence(); sequence < presents_.TailSequence(); ++sequence) {
        RetainedPresent& entry = presents_.At(sequence);
        const PacingPresentRecord& record = entry.record;
        if (record.native_swapchain != sample.native_swapchain ||
            record.thread_id != sample.thread_id ||
            record.p_qpc <= 0 || record.e_qpc < record.p_qpc ||
            static_cast<uint64_t>(record.p_qpc) > sample.present_start_qpc ||
            sample.present_start_qpc > static_cast<uint64_t>(record.e_qpc)) {
            continue;
        }
        ++*candidates;
        match = &entry;
    }
    return *candidates == 1 ? match : nullptr;
}

const PacingJoiner::RetainedSource* PacingJoiner::FindSourceByPclFrameId(
    uint32_t pcl_frame_id) const noexcept
{
    if (pcl_frame_id == 0 || !marker_epoch_valid_) return nullptr;
    return FindNewest(sources_, [this, pcl_frame_id](const PacingSourceRecord& record) {
        return record.key.domain == marker_domain_ &&
            record.key.producer_epoch == marker_epoch_ &&
            static_cast<uint32_t>(record.key.frame_id) == pcl_frame_id;
    });
}

void PacingJoiner::AttachSourceByKey(PacingJoinedRow* row) noexcept
{
    const PacingFrameKey key = row->key;
    const RetainedSource* source = FindNewest(sources_,
        [&key](const PacingSourceRecord& record) { return record.key == key; });
    if (source == nullptr) {
        ++statistics_.source_missing;
        return;
    }
    row->has_source = true;
    row->source = source->record;
}

void PacingJoiner::AttachWorkerRecords(PacingJoinedRow* row) noexcept
{
    const PacingFrameKey key = row->key;
    const bool key_valid = PacingFrameKey_IsValid(key);

    // Gate: the Present's GPU ticket, else an FG input wait for the frame key.
    const RetainedGate* gate = nullptr;
    if (row->has_present && row->present.gpu_ticket != 0) {
        const uint64_t ticket = row->present.gpu_ticket;
        gate = FindNewest(gates_,
            [ticket](const PacingGateJoin& candidate) { return candidate.ticket == ticket; });
    }
    else if (key_valid) {
        gate = FindNewest(gates_, [&key](const PacingGateJoin& candidate) {
            return candidate.fg != 0 && candidate.key == key;
        });
    }
    if (gate != nullptr) {
        row->has_gate = true;
        row->gate = gate->record;
        ++statistics_.gate_joined;
    }

    // Ready: frame key, else (swapchain generation, present sequence).
    const RetainedReady* ready = nullptr;
    if (key_valid) {
        ready = FindNewest(readies_,
            [&key](const PacingReadyRecord& candidate) { return candidate.key == key; });
    }
    if (ready == nullptr && row->has_present && row->present.swapchain_generation != 0 &&
        row->present.present_sequence != 0) {
        const uint64_t generation = row->present.swapchain_generation;
        const uint64_t sequence = row->present.present_sequence;
        ready = FindNewest(readies_, [generation, sequence](const PacingReadyRecord& candidate) {
            return candidate.swapchain_generation == generation &&
                candidate.present_sequence == sequence;
        });
    }
    if (ready != nullptr) {
        row->has_ready = true;
        row->ready = ready->record;
        ++statistics_.ready_joined;
    }

    if (key_valid) {
        const RetainedReflex* reflex = FindNewest(reflexes_,
            [&key](const PacingReflexRecord& candidate) { return candidate.key == key; });
        if (reflex != nullptr) {
            row->has_reflex = true;
            row->reflex = reflex->record;
            ++statistics_.reflex_joined;
        }
    }
}

// A retained Present can no longer receive a display sample once its
// swapchain's stream joined a later present start (PresentMon completes
// presents in order per swapchain), or when that swapchain never delivered a
// display sample (no ETW, access denied, not started). A quiet stream is no
// evidence: ETW delivers display samples in bursts about 1 s apart, so such a
// Present waits for its sample or for retention expiry.
bool PacingJoiner::DisplayNoLongerExpected(const PacingPresentRecord& record) const noexcept
{
    const SwapchainDisplayState* state = FindSwapchain(record.native_swapchain);
    if (state == nullptr) return true;
    return record.e_qpc > 0 &&
        state->joined_present_start_qpc > static_cast<uint64_t>(record.e_qpc);
}

const PacingJoiner::SwapchainDisplayState* PacingJoiner::FindSwapchain(
    uint64_t native_swapchain) const noexcept
{
    for (const SwapchainDisplayState& state : swapchains_) {
        if (state.in_use && state.native_swapchain == native_swapchain) return &state;
    }
    return nullptr;
}

PacingJoiner::SwapchainDisplayState* PacingJoiner::FindOrAddSwapchain(
    uint64_t native_swapchain) noexcept
{
    ++swapchain_use_clock_;
    SwapchainDisplayState* replacement = &swapchains_[0];
    for (SwapchainDisplayState& state : swapchains_) {
        if (state.in_use && state.native_swapchain == native_swapchain) {
            state.last_use = swapchain_use_clock_;
            return &state;
        }
        if (!replacement->in_use) continue;
        if (!state.in_use || state.last_use < replacement->last_use) replacement = &state;
    }
    *replacement = SwapchainDisplayState{};
    replacement->in_use = true;
    replacement->native_swapchain = native_swapchain;
    replacement->last_use = swapchain_use_clock_;
    return replacement;
}

// ── Metric input ──

PacingMetricInput PacingJoin_MetricInput(
    const PacingJoinedRow& row, uint64_t qpc_frequency) noexcept
{
    PacingMetricInput input{};
    input.qpc_frequency = qpc_frequency;
    input.row_kind = row.row_kind;
    input.key = row.key;
    if (row.has_display) {
        input.native_swapchain = row.display.native_swapchain;
        input.observer_epoch = row.display.observer_epoch;
        input.display_qpc = row.display.display_qpc;
        input.display_index = row.display.display_index;
        input.frame_type = row.display.frame_type;
        input.frame_classification = row.display.frame_classification;
        input.loss_window = row.display.loss_window != 0;
        input.continuity_break = row.display_continuity_break;
    }
    else if (row.has_present) {
        input.native_swapchain = row.present.native_swapchain;
    }
    if (row.has_source && row.source.release_qpc > 0) {
        input.source_joined = true;
        input.source_release_qpc = row.source.release_qpc;
        input.interval_us = row.source.interval_us;
        input.fg_divisor = row.source.fg_divisor;
    }
    if (row.has_present) {
        input.output_kind = row.present.output_kind;
        input.output_arrival_qpc = row.present.output_arrival_qpc;
        input.output_release_qpc = row.present.output_release_qpc;
    }
    input.late_display = row.late_display;
    if (row.has_gate && row.gate.signalled) {
        input.gate_signal_before_qpc = row.gate.signal_before_qpc;
        input.gate_fg = row.gate.fg != 0;
    }
    // Residence needs the pre-hold completion; a query behind a hold measures it.
    if (row.has_ready && row.ready.valid != 0 && row.ready.before_gate != 0 &&
        row.ready.ready_qpc > 0) {
        input.ready_qpc = row.ready.ready_qpc;
    }
    return input;
}
