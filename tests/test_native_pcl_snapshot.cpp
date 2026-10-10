#include "native_pcl_snapshot_policy.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kFrequency = 10'000'000;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

NativePclSnapshot Sample(uint64_t display, uint64_t publication) {
    NativePclSnapshot snapshot;
    snapshot.status = NativePclStatus::Native;
    snapshot.running = snapshot.available = snapshot.reliable = true;
    snapshot.display_qpc = display;
    snapshot.sample_qpc = publication;
    snapshot.input_to_frame_start_us = 3000.0;
    snapshot.frame_start_to_present_us = 7000.0;
    snapshot.present_to_display_us = 5000.0;
    snapshot.total_us = 15000.0;
    snapshot.dropped_accumulation_us = 0.0;
    return snapshot;
}

void TestDelayedHealthyDelivery() {
    // Actual worst-delay sample from PID43104/capture1, frame11687.
    const auto source = Sample(245957431280, 245982630048);
    auto snapshot = source;
    NativePcl_ApplySnapshotFreshness(&snapshot, true, source.sample_qpc, kFrequency);
    Check(snapshot.status == NativePclStatus::Native && snapshot.available &&
          snapshot.reliable, "fresh collection must survive 2.52s ETW delivery delay");
    Check(std::abs(snapshot.sample_age_us - 2519876.8) < 0.01,
          "report full observation age instead of hiding ETW delay");
    Check(snapshot.total_us == 15000.0, "transport delay is not PC latency");
    snapshot = source;
    NativePcl_ApplySnapshotFreshness(&snapshot, true,
        source.sample_qpc + 10'083'123, kFrequency);
    Check(snapshot.available, "retain value across the recorded 1.008s batch gap");
    snapshot = source;
    NativePcl_ApplySnapshotFreshness(&snapshot, true,
        source.sample_qpc + 2 * kFrequency, kFrequency);
    Check(snapshot.available, "exactly two seconds is still fresh");
    snapshot = source;
    NativePcl_ApplySnapshotFreshness(&snapshot, true,
        source.sample_qpc + 2 * kFrequency + 1, kFrequency);
    Check(snapshot.status == NativePclStatus::Stale && !snapshot.available &&
          !snapshot.reliable && snapshot.running && std::isnan(snapshot.total_us),
          "stop showing latency if collector stops publishing");
    snapshot = Sample(source.display_qpc + 3 * kFrequency,
                      source.sample_qpc + 3 * kFrequency);
    NativePcl_ApplySnapshotFreshness(&snapshot, true, snapshot.sample_qpc, kFrequency);
    Check(snapshot.available, "new collector publication restores native data");
}

void TestUnavailableAndInvalidStates() {
    const auto source = Sample(100, 200);
    for (const auto status : {NativePclStatus::Starting, NativePclStatus::Warming,
             NativePclStatus::NoPclStats, NativePclStatus::EtwAccessDenied,
             NativePclStatus::EventLoss, NativePclStatus::StartFailed,
             NativePclStatus::Stopped, NativePclStatus::Stale}) {
        auto snapshot = source;
        snapshot.status = status;
        snapshot.available = snapshot.reliable = false;
        snapshot.etw_events_lost = 3;
        NativePcl_ApplySnapshotFreshness(&snapshot, true, 250, kFrequency);
        Check(snapshot.status == status && !snapshot.available &&
              snapshot.etw_events_lost == 3 && std::isnan(snapshot.total_us),
              "do not revive failed/disabled collector states from old values");
    }
    auto snapshot = source;
    NativePcl_ApplySnapshotFreshness(&snapshot, false, 250, kFrequency);
    Check(snapshot.status == NativePclStatus::Disabled && !snapshot.running &&
          !snapshot.available, "disable applies immediately");
    snapshot = source;
    snapshot.status = NativePclStatus::LowFpsUnreliable;
    snapshot.reliable = false;
    NativePcl_ApplySnapshotFreshness(&snapshot, true, 250, kFrequency);
    Check(snapshot.status == NativePclStatus::LowFpsUnreliable &&
          snapshot.available && !snapshot.reliable, "preserve low-FPS warning");
    for (int invalid = 0; invalid < 5; ++invalid) {
        snapshot = source;
        uint64_t now = 250, frequency = kFrequency;
        if (invalid == 0) snapshot.display_qpc = 0;
        if (invalid == 1) snapshot.sample_qpc = 0;
        if (invalid == 2) snapshot.display_qpc = 201;
        if (invalid == 3) now = 199;
        if (invalid == 4) frequency = 0;
        NativePcl_ApplySnapshotFreshness(&snapshot, true, now, frequency);
        Check(!snapshot.available && snapshot.status == NativePclStatus::Stale,
              "malformed chronology/frequency must not preserve available data");
    }
}

std::vector<std::string> Split(const std::string& line) {
    // Historical relimiter_pcl captures (the family has been removed)
    // contain numeric and fixed status fields, no quoted commas.
    std::vector<std::string> values;
    std::istringstream stream(line);
    std::string value;
    while (std::getline(stream, value, ',')) values.push_back(value);
    return values;
}

void Replay(const char* path) {
    std::ifstream file(path);
    Check(file.good(), "capture cannot be opened");
    std::string line;
    std::getline(file, line);
    const auto header = Split(line);
    const auto column = [&](const char* name) {
        for (size_t i = 0; i < header.size(); ++i)
            if (header[i] == name) return i;
        throw std::runtime_error(std::string("missing column: ") + name);
    };
    const size_t display = column("display_qpc"), publication = column("sample_qpc"),
        frequency = column("qpc_frequency"), status = column("status");
    std::vector<NativePclSnapshot> samples;
    uint64_t clock = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const auto values = Split(line);
        Check(values.at(status) == "native", "replay expects a native-only capture");
        const uint64_t row_clock = std::stoull(values.at(frequency));
        Check(row_clock != 0 && (clock == 0 || row_clock == clock), "clock transition");
        clock = row_clock;
        samples.push_back(Sample(std::stoull(values.at(display)),
                                 std::stoull(values.at(publication))));
        if (samples.size() > 1)
            Check(samples.back().sample_qpc >= samples[samples.size() - 2].sample_qpc,
                  "publication order is not monotonic");
    }
    Check(!samples.empty() && clock >= 120, "empty/invalid capture");
    const uint64_t step = (clock + 60) / 120;
    size_t current = 0, queries = 0, old_stale = 0, new_stale = 0;
    size_t old_transitions = 0, new_transitions = 0;
    bool previous_old = false, previous_new = false;
    for (uint64_t now = samples.front().sample_qpc; now <= samples.back().sample_qpc;
         now += step) {
        while (current + 1 < samples.size() && samples[current + 1].sample_qpc <= now)
            ++current;
        auto snapshot = samples[current];
        const bool before = static_cast<double>(now - snapshot.display_qpc) /
            static_cast<double>(clock) > 2.0;
        NativePcl_ApplySnapshotFreshness(&snapshot, true, now, clock);
        const bool after = !snapshot.available;
        old_stale += before;
        new_stale += after;
        if (queries != 0) {
            old_transitions += before != previous_old;
            new_transitions += after != previous_new;
        }
        previous_old = before;
        previous_new = after;
        ++queries;
    }
    std::cout << "Read-only 120Hz capture replay: samples=" << samples.size()
        << " queries=" << queries << " old_stale=" << old_stale
        << " old_transitions=" << old_transitions << " new_stale=" << new_stale
        << " new_transitions=" << new_transitions << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        TestDelayedHealthyDelivery();
        TestUnavailableAndInvalidStates();
        if (argc == 2) Replay(argv[1]);
        std::cout << "native PCL snapshot freshness tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}