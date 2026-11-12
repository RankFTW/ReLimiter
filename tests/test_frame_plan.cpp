#include "frame_plan.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

// Directly tests the production frame-plan registry and stage-event queue
// (src/frame_plan.cpp). Concurrency checks are stress tests with fixed
// iteration counts; they cannot prove the absence of a race, only catch one.

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                            \
            ++s_failures;                                                        \
        }                                                                       \
    } while (false)

    constexpr unsigned kClaimThreadCount = 8;
    constexpr unsigned kReaderThreadCount = 3;

    // ── Helpers ──

    PacingPlanKey MakeKey(
        uint64_t frame_id,
        uint32_t producer_epoch = 3,
        uint32_t controller_generation = 5,
        PacingIdentityDomain domain = PacingIdentityDomain::NvApiFrame)
    {
        PacingPlanKey key{};
        key.identity_domain = domain;
        key.producer_epoch = producer_epoch;
        key.controller_generation = controller_generation;
        key.frame_id = frame_id;
        return key;
    }

    // Every payload field is derived from `counter`, so a plan mixing two
    // publications fails PlanMatchesCounter.
    PacingFramePlan MakePlan(const PacingPlanKey& key, uint64_t counter)
    {
        PacingFramePlan plan{};
        plan.key = key;
        plan.grid_index = counter;
        plan.grid_epoch = static_cast<uint32_t>(counter * 3 + 1);
        plan.pipeline_generation = static_cast<uint32_t>(counter ^ 0x5A5A5A5Au);
        plan.output_target_qpc = static_cast<int64_t>(counter) * 1000 + 7;
        plan.source_wake_qpc = static_cast<int64_t>(counter) * 1000 - 3;
        plan.lead_us = static_cast<double>(counter) * 0.25;
        plan.interval_us = static_cast<double>(counter) + 0.5;
        plan.fg_divisor = 1.0 + static_cast<double>(counter % 4);
        plan.rung = static_cast<PacingRung>(counter % 5);
        plan.mode = static_cast<PacingTimingMode>(counter % 4);
        plan.marker = static_cast<PacingSourceMarker>(counter % 3);
        plan.hold_advance_us = counter % 3 == 0 ? 0.0f : static_cast<float>(counter) * 0.5f;
        plan.law_hold_sequence = counter % 2 == 0 ? 0 : 1'000'000'000'000ull + counter; // a 64-bit identity
        return plan;
    }

    PacingFramePlan MakeRungPlan(
        const PacingPlanKey& key,
        uint64_t counter,
        PacingRung rung)
    {
        PacingFramePlan plan = MakePlan(key, counter);
        plan.rung = rung;
        return plan;
    }

    bool SamePlan(const PacingFramePlan& left, const PacingFramePlan& right)
    {
        return left.key == right.key &&
            left.grid_index == right.grid_index &&
            left.grid_epoch == right.grid_epoch &&
            left.pipeline_generation == right.pipeline_generation &&
            left.output_target_qpc == right.output_target_qpc &&
            left.source_wake_qpc == right.source_wake_qpc &&
            left.lead_us == right.lead_us &&
            left.interval_us == right.interval_us &&
            left.fg_divisor == right.fg_divisor &&
            left.rung == right.rung &&
            left.mode == right.mode &&
            left.marker == right.marker &&
            left.hold_advance_us == right.hold_advance_us &&
            left.law_hold_sequence == right.law_hold_sequence;
    }

    bool PlanMatchesCounter(const PacingFramePlan& plan, const PacingPlanKey& key)
    {
        return SamePlan(plan, MakePlan(key, plan.grid_index));
    }

    bool PlanMatchesCounter(
        const PacingFramePlan& plan,
        const PacingPlanKey& key,
        PacingRung rung)
    {
        return SamePlan(plan, MakeRungPlan(key, plan.grid_index, rung));
    }

    // All keys share slot 7 so every publication evicts the previous one.
    PacingPlanKey MakeCollidingKey(uint64_t counter)
    {
        return MakeKey(
            7 + 256 * counter,
            1 + static_cast<uint32_t>(counter % 97),
            1 + static_cast<uint32_t>(counter % 89));
    }

    // Zeroed atomic counters indexed by a 64-bit round or publication counter.
    class Counters {
    public:
        explicit Counters(uint64_t count)
            : values_(new std::atomic<uint32_t>[static_cast<size_t>(count)])
        {
            for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
                values_[index].store(0, std::memory_order_relaxed);
            }
        }

        std::atomic<uint32_t>& operator[](uint64_t index) noexcept
        {
            return values_[static_cast<size_t>(index)];
        }

    private:
        std::unique_ptr<std::atomic<uint32_t>[]> values_;
    };

    PacingStageEvent MakeEvent(uint32_t producer, uint64_t sequence)
    {
        PacingStageEvent event{};
        event.key = MakeKey(sequence, producer + 1);
        event.grid_index = sequence;
        event.qpc = static_cast<int64_t>(sequence) * 10 + producer;
        event.kind = PacingStageEventKind::OutputRelease;
        event.rung = PacingRung::MarkerHold;
        event.status = PacingPlanStatus::Held;
        return event;
    }

    // ── Registry: single-threaded semantics ──

    void TestExactKeyFind()
    {
        auto registry = std::make_unique<FramePlanRegistry>();
        const PacingPlanKey key = MakeKey(1000);
        const PacingFramePlan published = MakePlan(key, 42);
        CHECK(registry->Publish(published));

        PacingFramePlan found{};
        CHECK(registry->Find(key, &found));
        CHECK(SamePlan(found, published));
        CHECK(registry->Find(key, nullptr));

        PacingPlanKey other = key;
        other.identity_domain = PacingIdentityDomain::PclFrame;
        CHECK(!registry->Find(other, &found));
        CHECK(SamePlan(found, PacingFramePlan{}));
        other = key;
        other.producer_epoch = 4;
        CHECK(!registry->Find(other, &found));
        other = key;
        other.controller_generation = 6;
        CHECK(!registry->Find(other, &found));
        other = key;
        other.frame_id = key.frame_id + FramePlanRegistry::kCapacity;
        CHECK(!registry->Find(other, &found));
        other = key;
        other.frame_id = key.frame_id + 1;
        CHECK(!registry->Find(other, &found));

        // Find never consumes the claim.
        CHECK(registry->ClaimOutput(key, published.rung, nullptr) ==
            FramePlanClaim::Claimed);

        // Frame ID 0 is a legitimate NvAPI frame.
        const PacingPlanKey zero = MakeKey(0);
        CHECK(registry->Publish(MakePlan(zero, 9)));
        CHECK(registry->Find(zero, &found));
        CHECK(SamePlan(found, MakePlan(zero, 9)));
    }

    void TestInvalidKeyRejected()
    {
        auto registry = std::make_unique<FramePlanRegistry>();
        const PacingPlanKey valid = MakeKey(9);
        CHECK(registry->Publish(MakePlan(valid, 1)));

        PacingPlanKey invalid = valid;
        invalid.identity_domain = PacingIdentityDomain::Unknown;
        CHECK(!registry->Publish(MakePlan(invalid, 2)));
        CHECK(!registry->Find(invalid, nullptr));
        CHECK(registry->ClaimOutput(invalid, PacingRung::SourceOnly, nullptr) ==
            FramePlanClaim::Missing);
        invalid = valid;
        invalid.producer_epoch = 0;
        CHECK(!registry->Publish(MakePlan(invalid, 3)));
        invalid = valid;
        invalid.controller_generation = 0;
        CHECK(!registry->Publish(MakePlan(invalid, 4)));

        // A rejected publication leaves the slot's plan and claim intact.
        PacingFramePlan found{};
        CHECK(registry->Find(valid, &found));
        CHECK(SamePlan(found, MakePlan(valid, 1)));
        CHECK(registry->ClaimOutput(valid, found.rung, nullptr) ==
            FramePlanClaim::Claimed);

        // An empty registry has nothing to find for any valid key.
        auto empty = std::make_unique<FramePlanRegistry>();
        CHECK(!empty->Find(MakeKey(0), nullptr));
        CHECK(empty->ClaimOutput(MakeKey(0), PacingRung::SourceOnly, nullptr) ==
            FramePlanClaim::Missing);
    }

    void TestSlotReuseGivesMissing()
    {
        auto registry = std::make_unique<FramePlanRegistry>();
        const PacingPlanKey old_key = MakeKey(10);
        const PacingPlanKey new_key = MakeKey(10 + FramePlanRegistry::kCapacity);
        CHECK(registry->Publish(MakeRungPlan(old_key, 1, PacingRung::MarkerHold)));
        CHECK(registry->Publish(MakeRungPlan(new_key, 2, PacingRung::MarkerHold)));

        PacingFramePlan plan = MakePlan(old_key, 99);
        CHECK(!registry->Find(old_key, &plan));
        CHECK(registry->ClaimOutput(old_key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Missing);
        CHECK(SamePlan(plan, PacingFramePlan{}));
        CHECK(registry->ClaimOutput(new_key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Claimed);
        CHECK(SamePlan(plan, MakeRungPlan(new_key, 2, PacingRung::MarkerHold)));
        CHECK(registry->ClaimOutput(new_key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Duplicate);
        CHECK(SamePlan(plan, MakeRungPlan(new_key, 2, PacingRung::MarkerHold)));

        // Neighbouring slots are independent.
        const PacingPlanKey neighbour = MakeKey(11);
        CHECK(registry->Publish(MakeRungPlan(neighbour, 3, PacingRung::MarkerHold)));
        CHECK(registry->Find(new_key, nullptr));
        CHECK(registry->ClaimOutput(neighbour, PacingRung::MarkerHold, nullptr) ==
            FramePlanClaim::Claimed);
    }

    void TestOtherRungAndDuplicate()
    {
        auto registry = std::make_unique<FramePlanRegistry>();
        const PacingPlanKey key = MakeKey(20);
        const PacingFramePlan published =
            MakeRungPlan(key, 5, PacingRung::MarkerHold);
        CHECK(registry->Publish(published));

        PacingFramePlan plan{};
        CHECK(registry->ClaimOutput(key, PacingRung::GpuQueueWait, &plan) ==
            FramePlanClaim::OtherRung);
        CHECK(SamePlan(plan, published));
        CHECK(registry->ClaimOutput(key, PacingRung::SourceOnly, &plan) ==
            FramePlanClaim::OtherRung);
        CHECK(registry->ClaimOutput(key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Claimed);
        CHECK(SamePlan(plan, published));
        CHECK(registry->ClaimOutput(key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Duplicate);
        CHECK(SamePlan(plan, published));
        // The rung check comes first, also after the claim.
        CHECK(registry->ClaimOutput(key, PacingRung::NativeHold, &plan) ==
            FramePlanClaim::OtherRung);
        CHECK(registry->Find(key, &plan));

        // Publishing the same key again starts a new, unclaimed publication.
        const PacingFramePlan republished =
            MakeRungPlan(key, 6, PacingRung::MarkerHold);
        CHECK(registry->Publish(republished));
        CHECK(registry->ClaimOutput(key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Claimed);
        CHECK(SamePlan(plan, republished));
        CHECK(registry->ClaimOutput(key, PacingRung::MarkerHold, &plan) ==
            FramePlanClaim::Duplicate);
    }

    void TestClear()
    {
        auto registry = std::make_unique<FramePlanRegistry>();
        for (uint64_t frame = 100; frame < 100 + FramePlanRegistry::kCapacity; ++frame) {
            CHECK(registry->Publish(
                MakeRungPlan(MakeKey(frame), frame, PacingRung::MarkerHold)));
        }
        CHECK(registry->ClaimOutput(MakeKey(150), PacingRung::MarkerHold, nullptr) ==
            FramePlanClaim::Claimed);

        registry->Clear();
        for (uint64_t frame = 100; frame < 100 + FramePlanRegistry::kCapacity; ++frame) {
            CHECK(!registry->Find(MakeKey(frame), nullptr));
            CHECK(registry->ClaimOutput(MakeKey(frame), PacingRung::MarkerHold, nullptr) ==
                FramePlanClaim::Missing);
        }

        // Plans published after Clear are claimable, including the same key.
        CHECK(registry->Publish(MakeRungPlan(MakeKey(150), 1, PacingRung::MarkerHold)));
        CHECK(registry->ClaimOutput(MakeKey(150), PacingRung::MarkerHold, nullptr) ==
            FramePlanClaim::Claimed);

        // The global registry follows the same rules.
        const PacingPlanKey global_key = MakeKey(77, 11, 13);
        CHECK(g_frame_plans.Publish(MakeRungPlan(global_key, 3, PacingRung::GpuQueueWait)));
        CHECK(g_frame_plans.Find(global_key, nullptr));
        g_frame_plans.Clear();
        CHECK(!g_frame_plans.Find(global_key, nullptr));
    }

    // ── Registry: concurrency ──

    void TestSingleClaimUnderContention()
    {
        constexpr uint64_t kRounds = 2000;
        auto registry = std::make_unique<FramePlanRegistry>();
        Counters claimed(kRounds + 1);
        Counters duplicate(kRounds + 1);
        Counters wrong(kRounds + 1);
        std::atomic<uint64_t> published_round{ 0 };
        std::atomic<uint64_t> finished{ 0 };

        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < kClaimThreadCount; ++worker) {
            workers.emplace_back([&]() {
                for (uint64_t round = 1; round <= kRounds; ++round) {
                    while (published_round.load(std::memory_order_acquire) < round) {
                        std::this_thread::yield();
                    }
                    const PacingPlanKey key = MakeKey(round);
                    PacingFramePlan plan{};
                    const FramePlanClaim result =
                        registry->ClaimOutput(key, PacingRung::GpuQueueWait, &plan);
                    const bool plan_ok = SamePlan(
                        plan, MakeRungPlan(key, round, PacingRung::GpuQueueWait));
                    if (result == FramePlanClaim::Claimed && plan_ok) {
                        claimed[round].fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (result == FramePlanClaim::Duplicate && plan_ok) {
                        duplicate[round].fetch_add(1, std::memory_order_relaxed);
                    }
                    else {
                        wrong[round].fetch_add(1, std::memory_order_relaxed);
                    }
                    finished.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }

        for (uint64_t round = 1; round <= kRounds; ++round) {
            CHECK(registry->Publish(
                MakeRungPlan(MakeKey(round), round, PacingRung::GpuQueueWait)));
            published_round.store(round, std::memory_order_release);
            while (finished.load(std::memory_order_acquire) < round * kClaimThreadCount) {
                std::this_thread::yield();
            }
        }
        for (std::thread& worker : workers) worker.join();

        uint64_t bad_rounds = 0;
        for (uint64_t round = 1; round <= kRounds; ++round) {
            if (claimed[round].load() != 1 ||
                duplicate[round].load() != kClaimThreadCount - 1 ||
                wrong[round].load() != 0) {
                if (bad_rounds == 0) {
                    std::fprintf(stderr,
                        "round %llu: claimed %u duplicate %u wrong %u\n",
                        static_cast<unsigned long long>(round),
                        claimed[round].load(), duplicate[round].load(),
                        wrong[round].load());
                }
                ++bad_rounds;
            }
        }
        CHECK(bad_rounds == 0);
    }

    // Claimers chase a writer that republishes one slot with a new key every
    // iteration. A claim must return the exact plan it read, at most once per
    // publication, and a key evicted before the claim started is Missing.
    void TestClaimRaceWithSlotRepublish()
    {
        constexpr uint64_t kPublications = 200000;
        auto registry = std::make_unique<FramePlanRegistry>();
        Counters claims(kPublications + 1);
        std::atomic<uint64_t> latest{ 0 };
        std::atomic<bool> done{ false };
        std::atomic<uint64_t> wrong_plans{ 0 };
        std::atomic<uint64_t> evicted_not_missing{ 0 };
        std::atomic<uint64_t> total_claimed{ 0 };

        std::vector<std::thread> claimers;
        for (unsigned claimer = 0; claimer < kClaimThreadCount / 2; ++claimer) {
            claimers.emplace_back([&]() {
                while (!done.load(std::memory_order_acquire)) {
                    const uint64_t counter = latest.load(std::memory_order_acquire);
                    if (counter == 0) continue;
                    const PacingPlanKey key = MakeCollidingKey(counter);
                    PacingFramePlan plan{};
                    const FramePlanClaim result =
                        registry->ClaimOutput(key, PacingRung::MarkerHold, &plan);
                    if (result == FramePlanClaim::Claimed ||
                        result == FramePlanClaim::Duplicate) {
                        if (plan.grid_index != counter ||
                            !PlanMatchesCounter(plan, key, PacingRung::MarkerHold)) {
                            wrong_plans.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (result == FramePlanClaim::Claimed) {
                            claims[counter].fetch_add(1, std::memory_order_relaxed);
                            total_claimed.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    else if (result != FramePlanClaim::Missing) {
                        wrong_plans.fetch_add(1, std::memory_order_relaxed);
                    }

                    // The slot already holds `counter` or newer: an older key
                    // of the same slot must be Missing.
                    if (counter > 1 &&
                        registry->ClaimOutput(MakeCollidingKey(counter - 1),
                            PacingRung::MarkerHold, nullptr) != FramePlanClaim::Missing) {
                        evicted_not_missing.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (uint64_t counter = 1; counter <= kPublications; ++counter) {
            const PacingPlanKey key = MakeCollidingKey(counter);
            CHECK(registry->Publish(MakeRungPlan(key, counter, PacingRung::MarkerHold)));
            latest.store(counter, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
        for (std::thread& claimer : claimers) claimer.join();

        uint64_t double_claims = 0;
        for (uint64_t counter = 1; counter <= kPublications; ++counter) {
            if (claims[counter].load() > 1) ++double_claims;
        }
        CHECK(double_claims == 0);
        CHECK(wrong_plans.load() == 0);
        CHECK(evicted_not_missing.load() == 0);
        CHECK(total_claimed.load() > 0);
        std::printf("slot republish race: %llu claims over %llu publications\n",
            static_cast<unsigned long long>(total_claimed.load()),
            static_cast<unsigned long long>(kPublications));
    }

    // ABA: the same key republished every iteration. A claimer holding a copy
    // of publication c must not consume the claim of publication c + 1, so no
    // payload is claimed twice.
    void TestClaimRaceWithSameKeyRepublish()
    {
        constexpr uint64_t kPublications = 200000;
        auto registry = std::make_unique<FramePlanRegistry>();
        Counters claims(kPublications + 1);
        const PacingPlanKey key = MakeKey(33);
        std::atomic<bool> done{ false };
        std::atomic<uint64_t> wrong_plans{ 0 };
        std::atomic<uint64_t> total_claimed{ 0 };

        CHECK(registry->Publish(MakeRungPlan(key, 1, PacingRung::FgInputWait)));
        std::vector<std::thread> claimers;
        for (unsigned claimer = 0; claimer < kClaimThreadCount / 2; ++claimer) {
            claimers.emplace_back([&]() {
                while (!done.load(std::memory_order_acquire)) {
                    PacingFramePlan plan{};
                    const FramePlanClaim result =
                        registry->ClaimOutput(key, PacingRung::FgInputWait, &plan);
                    if (result == FramePlanClaim::Missing) continue;
                    if (result == FramePlanClaim::OtherRung ||
                        plan.grid_index == 0 || plan.grid_index > kPublications ||
                        !PlanMatchesCounter(plan, key, PacingRung::FgInputWait)) {
                        wrong_plans.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (result == FramePlanClaim::Claimed) {
                        claims[plan.grid_index].fetch_add(1, std::memory_order_relaxed);
                        total_claimed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (uint64_t counter = 2; counter <= kPublications; ++counter) {
            CHECK(registry->Publish(MakeRungPlan(key, counter, PacingRung::FgInputWait)));
        }
        done.store(true, std::memory_order_release);
        for (std::thread& claimer : claimers) claimer.join();

        // After the writer stops, the final publication is claimable once.
        PacingFramePlan plan{};
        const FramePlanClaim final_result =
            registry->ClaimOutput(key, PacingRung::FgInputWait, &plan);
        CHECK(final_result == FramePlanClaim::Claimed ||
            final_result == FramePlanClaim::Duplicate);
        CHECK(plan.grid_index == kPublications);
        if (final_result == FramePlanClaim::Claimed) {
            claims[kPublications].fetch_add(1, std::memory_order_relaxed);
        }
        CHECK(claims[kPublications].load() == 1);

        uint64_t double_claims = 0;
        for (uint64_t counter = 1; counter <= kPublications; ++counter) {
            if (claims[counter].load() > 1) ++double_claims;
        }
        CHECK(double_claims == 0);
        CHECK(wrong_plans.load() == 0);
        CHECK(total_claimed.load() > 0);
        std::printf("same-key republish race: %llu claims over %llu publications\n",
            static_cast<unsigned long long>(total_claimed.load()),
            static_cast<unsigned long long>(kPublications));
    }

    // Readers under a continuous writer only ever see whole publications:
    // payload changes under one key, and keys changing within one slot.
    void TestConcurrentReadersNeverSeeTornPlans()
    {
        constexpr uint64_t kPublications = 500000;
        auto registry = std::make_unique<FramePlanRegistry>();
        const PacingPlanKey fixed_key = MakeKey(45);
        std::atomic<uint64_t> latest{ 0 };
        std::atomic<bool> done{ false };
        std::atomic<uint64_t> torn{ 0 };
        std::atomic<uint64_t> found_fixed{ 0 };
        std::atomic<uint64_t> found_moving{ 0 };

        CHECK(registry->Publish(MakePlan(fixed_key, 1)));
        std::vector<std::thread> readers;
        for (unsigned reader = 0; reader < kReaderThreadCount; ++reader) {
            readers.emplace_back([&]() {
                uint64_t last_fixed_counter = 0;
                while (!done.load(std::memory_order_acquire)) {
                    PacingFramePlan plan{};
                    if (registry->Find(fixed_key, &plan)) {
                        // Publications are ordered, so a reader never goes back.
                        if (!PlanMatchesCounter(plan, fixed_key) ||
                            plan.grid_index < last_fixed_counter) {
                            torn.fetch_add(1, std::memory_order_relaxed);
                        }
                        last_fixed_counter = plan.grid_index;
                        found_fixed.fetch_add(1, std::memory_order_relaxed);
                    }

                    const uint64_t counter = latest.load(std::memory_order_acquire);
                    if (counter == 0) continue;
                    const PacingPlanKey moving_key = MakeCollidingKey(counter);
                    if (registry->Find(moving_key, &plan)) {
                        if (plan.grid_index != counter ||
                            !PlanMatchesCounter(plan, moving_key)) {
                            torn.fetch_add(1, std::memory_order_relaxed);
                        }
                        found_moving.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (uint64_t counter = 2; counter <= kPublications; ++counter) {
            CHECK(registry->Publish(MakePlan(fixed_key, counter)));
            CHECK(registry->Publish(MakePlan(MakeCollidingKey(counter), counter)));
            latest.store(counter, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
        for (std::thread& reader : readers) reader.join();

        CHECK(torn.load() == 0);
        CHECK(found_fixed.load() > 0);
        CHECK(found_moving.load() > 0);
        std::printf("concurrent readers: %llu fixed-key and %llu moving-key reads\n",
            static_cast<unsigned long long>(found_fixed.load()),
            static_cast<unsigned long long>(found_moving.load()));
    }

    // ── Stage-event queue ──

    void TestStageQueueOrderAndDrops()
    {
        auto queue = std::make_unique<PacingStageEventQueue>();
        PacingStageEvent event{};
        CHECK(!queue->Pop(&event));
        CHECK(!queue->Pop(nullptr));

        for (uint64_t sequence = 0; sequence < PacingStageEventQueue::kCapacity; ++sequence) {
            CHECK(queue->Push(MakeEvent(0, sequence)));
        }
        CHECK(queue->Dropped() == 0);
        CHECK(!queue->Push(MakeEvent(0, 5000)));
        CHECK(!queue->Push(MakeEvent(0, 5001)));
        CHECK(queue->Dropped() == 2);

        for (uint64_t sequence = 0; sequence < PacingStageEventQueue::kCapacity; ++sequence) {
            CHECK(queue->Pop(&event));
            const PacingStageEvent expected = MakeEvent(0, sequence);
            CHECK(event.key == expected.key);
            CHECK(event.grid_index == expected.grid_index);
            CHECK(event.qpc == expected.qpc);
            CHECK(event.kind == expected.kind);
            CHECK(event.rung == expected.rung);
            CHECK(event.status == expected.status);
        }
        CHECK(!queue->Pop(&event));

        // Space is reusable after a drain; the drop count is cumulative.
        CHECK(queue->Push(MakeEvent(1, 7)));
        CHECK(queue->Pop(&event));
        CHECK(event.grid_index == 7);
        CHECK(queue->Dropped() == 2);
    }

    void TestStageQueueMultiProducerFifo()
    {
        constexpr uint32_t kProducers = 4;
        constexpr uint64_t kEventsPerProducer = 50000;
        auto queue = std::make_unique<PacingStageEventQueue>();
        std::atomic<uint32_t> producers_done{ 0 };
        std::atomic<uint64_t> pushed{ 0 };

        std::vector<std::thread> producers;
        for (uint32_t producer = 0; producer < kProducers; ++producer) {
            producers.emplace_back([&, producer]() {
                for (uint64_t sequence = 1; sequence <= kEventsPerProducer; ++sequence) {
                    if (queue->Push(MakeEvent(producer, sequence))) {
                        pushed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                producers_done.fetch_add(1, std::memory_order_release);
            });
        }

        uint64_t last_sequence[kProducers] = {};
        uint64_t popped = 0;
        uint64_t out_of_order = 0;
        PacingStageEvent event{};
        for (;;) {
            const bool finished =
                producers_done.load(std::memory_order_acquire) == kProducers;
            bool drained_any = false;
            while (queue->Pop(&event)) {
                drained_any = true;
                ++popped;
                const uint32_t producer = event.key.producer_epoch - 1;
                if (producer >= kProducers ||
                    event.grid_index <= last_sequence[producer] ||
                    event.qpc != static_cast<int64_t>(event.grid_index) * 10 + producer) {
                    ++out_of_order;
                    continue;
                }
                last_sequence[producer] = event.grid_index;
            }
            if (finished && !drained_any) break;
        }
        for (std::thread& producer : producers) producer.join();

        CHECK(out_of_order == 0);
        CHECK(popped == pushed.load());
        CHECK(popped + queue->Dropped() == kProducers * kEventsPerProducer);
        std::printf("stage queue: %llu delivered, %llu dropped\n",
            static_cast<unsigned long long>(popped),
            static_cast<unsigned long long>(queue->Dropped()));
    }

    void TestGlobalStageQueue()
    {
        PacingStageEvent event{};
        while (FramePlan_PopStageEvent(&event)) {}
        const uint64_t dropped_before = FramePlan_DroppedStageEvents();

        for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
            CHECK(FramePlan_PushStageEvent(MakeEvent(2, sequence)));
        }
        for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
            CHECK(FramePlan_PopStageEvent(&event));
            CHECK(event.grid_index == sequence);
            CHECK(event.key.producer_epoch == 3);
        }
        CHECK(!FramePlan_PopStageEvent(&event));
        CHECK(!FramePlan_PopStageEvent(nullptr));

        for (uint64_t sequence = 0; sequence < PacingStageEventQueue::kCapacity; ++sequence) {
            CHECK(FramePlan_PushStageEvent(MakeEvent(0, sequence)));
        }
        CHECK(!FramePlan_PushStageEvent(MakeEvent(0, 9999)));
        CHECK(FramePlan_DroppedStageEvents() == dropped_before + 1);
        uint64_t drained = 0;
        while (FramePlan_PopStageEvent(&event)) ++drained;
        CHECK(drained == PacingStageEventQueue::kCapacity);
    }

    // ── Hard-limit ready maxima (developer key unified_pacer) ──

    int s_ready_wakes = 0;
    const void* s_ready_wake_address = nullptr;

    PacingStageEvent ReadyEvent(uint64_t index, int64_t qpc, PacingStageEventKind kind = PacingStageEventKind::GpuReady)
    {
        PacingStageEvent event{};
        event.key = MakeKey(index, 1);
        event.grid_index = index;
        event.qpc = qpc;
        event.kind = kind;
        event.rung = PacingRung::SourceOnly;
        return event;
    }

    void ResetReadyMaxima()
    {
        PacingStageEvent drained{};
        while (FramePlan_PopStageEvent(&drained)) {}
        g_up_ready_index.store(0);
        g_up_ready_qpc.store(0);
        s_ready_wakes = 0;
        s_ready_wake_address = nullptr;
    }

    // Two independent monotone maxima: an older value is never stored, a producer that loses a
    // race simply stops, and every index raise (only) wakes the waiters.
    void TestReadyMaximaOrdering()
    {
        ResetReadyMaxima();
        FramePlan_SetReadyWaker([](const void* address) noexcept {
            ++s_ready_wakes;
            s_ready_wake_address = address;
        });

        // Key off (publish flag clear): nothing is published.
        CHECK(!g_up_ready_publish.load());
        FramePlan_PushStageEvent(ReadyEvent(3, 300));
        CHECK(g_up_ready_index.load() == 0 && g_up_ready_qpc.load() == 0 && s_ready_wakes == 0);

        g_up_ready_publish.store(true);
        FramePlan_PushStageEvent(ReadyEvent(5, 500));
        CHECK(g_up_ready_index.load() == 5 && g_up_ready_qpc.load() == 500);
        CHECK(s_ready_wakes == 1 && s_ready_wake_address == &g_up_ready_index);

        // An older producer (index 4, QPC 450) that arrives after: nothing is stored, no wake.
        FramePlan_PushStageEvent(ReadyEvent(4, 450));
        CHECK(g_up_ready_index.load() == 5 && g_up_ready_qpc.load() == 500 && s_ready_wakes == 1);
        // The same index again: no raise.
        FramePlan_PushStageEvent(ReadyEvent(5, 480));
        CHECK(g_up_ready_index.load() == 5 && g_up_ready_qpc.load() == 500 && s_ready_wakes == 1);

        // The two maxima are independent: a lower index with a newer QPC raises only the QPC
        // (the completion evidence is fresher), a higher index with an older QPC raises only the index.
        FramePlan_RaiseReady(4, 700);
        CHECK(g_up_ready_index.load() == 5 && g_up_ready_qpc.load() == 700 && s_ready_wakes == 1);
        FramePlan_RaiseReady(8, 600);
        CHECK(g_up_ready_index.load() == 8 && g_up_ready_qpc.load() == 700 && s_ready_wakes == 2);

        // Other stage events and an unkeyed grid index publish nothing.
        FramePlan_PushStageEvent(ReadyEvent(20, 2000, PacingStageEventKind::OutputRelease));
        FramePlan_PushStageEvent(ReadyEvent(0, 3000));
        CHECK(g_up_ready_index.load() == 8 && g_up_ready_qpc.load() == 700 && s_ready_wakes == 2);

        g_up_ready_publish.store(false);
        FramePlan_SetReadyWaker(nullptr);
        ResetReadyMaxima();
    }

    // Several producers and one reader with fixed counts: both values are monotone for the reader
    // at every read and end at the maxima; no producer waits for another.
    void TestReadyMaximaProducersAndReader()
    {
        ResetReadyMaxima();
        g_up_ready_publish.store(true);
        constexpr unsigned kProducers = 3;
        constexpr uint64_t kPerProducer = 20000;
        std::atomic<unsigned> started{0};
        std::atomic<bool> done{false};
        std::atomic<uint64_t> backwards{0};
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> threads;
        for (unsigned producer = 0; producer < kProducers; ++producer) {
            threads.emplace_back([&, producer]() {
                started.fetch_add(1);
                while (started.load() < kProducers) {}
                for (uint64_t step = 0; step < kPerProducer; ++step) {
                    const uint64_t index = step * kProducers + producer + 1;
                    FramePlan_PushStageEvent(ReadyEvent(index, static_cast<int64_t>(index) * 10));
                    // (the queue's consumer is the reader thread only: Pop has a single consumer)
                }
            });
        }
        std::thread reader([&]() {
            uint64_t last_index = 0;
            int64_t last_qpc = 0;
            PacingStageEvent drained{};
            while (!done.load(std::memory_order_acquire)) {
                while (FramePlan_PopStageEvent(&drained)) {}
                const int64_t qpc = g_up_ready_qpc.load(std::memory_order_acquire);
                const uint64_t index = g_up_ready_index.load(std::memory_order_acquire);
                reads.fetch_add(1, std::memory_order_relaxed);
                if (index < last_index || qpc < last_qpc) backwards.fetch_add(1);
                last_index = index;
                last_qpc = qpc;
            }
        });
        for (unsigned producer = 0; producer < kProducers; ++producer) threads[producer].join();
        done.store(true, std::memory_order_release);
        reader.join();

        CHECK(g_up_ready_index.load() == kPerProducer * kProducers);
        CHECK(g_up_ready_qpc.load() == static_cast<int64_t>(kPerProducer * kProducers) * 10);
        CHECK(backwards.load() == 0);
        std::printf("ready maxima: %llu reads against %u producers\n",
            static_cast<unsigned long long>(reads.load()), kProducers);
        g_up_ready_publish.store(false);
        ResetReadyMaxima();
    }
}

int main()
{
    std::printf("sizeof(FramePlanRegistry) = %zu bytes, sizeof(PacingStageEventQueue) = %zu bytes\n",
        sizeof(FramePlanRegistry), sizeof(PacingStageEventQueue));
    CHECK(sizeof(FramePlanRegistry) == FramePlanRegistry::kCapacity * 104);

    TestExactKeyFind();
    TestInvalidKeyRejected();
    TestSlotReuseGivesMissing();
    TestOtherRungAndDuplicate();
    TestClear();
    TestSingleClaimUnderContention();
    TestClaimRaceWithSlotRepublish();
    TestClaimRaceWithSameKeyRepublish();
    TestConcurrentReadersNeverSeeTornPlans();
    TestStageQueueOrderAndDrops();
    TestStageQueueMultiProducerFifo();
    TestGlobalStageQueue();
    TestReadyMaximaOrdering();
    TestReadyMaximaProducersAndReader();

    if (s_failures != 0) {
        std::fprintf(stderr, "%d frame-plan checks failed\n", s_failures);
        return 1;
    }
    std::puts("Frame-plan registry and stage-event queue checks passed");
    return 0;
}
