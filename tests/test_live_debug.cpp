#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "json.h"
#include "live_debug_buffer.h"
#include "live_debug_protocol.h"
#include "pacing_analysis.h"

namespace
{
    using namespace relimiter::live_debug;
    using relimiter::json::Value;

    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    bool TestProtocolLayout()
    {
        MessageHeader request;
        request.type = MessageType::GetSchedulerRows;
        return Check(request.magic == kProtocolMagic, "protocol magic") &&
            Check(request.version == kProtocolVersion, "protocol version") &&
            Check(ResponseType(request.type) ==
                static_cast<MessageType>(0x8002), "response type flag") &&
            Check(sizeof(StatusWire) == 264, "status ABI size") &&
            Check(sizeof(SchedulerRowWire) == 328, "scheduler ABI size") &&
            Check(sizeof(NativePclRowWire) == 216, "native PCL ABI size");
    }

    bool TestProducerQueue()
    {
        ProducerQueue<int, 4> queue;
        if (!Check(queue.Push(1) && queue.Push(2) && queue.Push(3) && queue.Push(4),
                   "fill producer queue")) return false;
        if (!Check(!queue.Push(5) && queue.DroppedRows() == 1,
                   "drop when producer queue is full")) return false;

        int value = 0;
        if (!Check(queue.Pop(&value) && value == 1, "FIFO pop 1") ||
            !Check(queue.Pop(&value) && value == 2, "FIFO pop 2") ||
            !Check(queue.Push(6) && queue.Push(7), "producer wraparound push")) {
            return false;
        }
        const int expected[] = { 3, 4, 6, 7 };
        for (int expected_value : expected) {
            if (!Check(queue.Pop(&value) && value == expected_value,
                       "producer queue preserves wraparound order")) return false;
        }
        return Check(!queue.Pop(&value), "empty producer queue");
    }

    bool TestConcurrentProducers()
    {
        constexpr int producer_count = 4;
        constexpr int values_per_producer = 10000;
        ProducerQueue<uint32_t, 1024> queue;
        std::atomic<bool> start{ false };
        std::atomic<bool> producers_done{ false };
        std::atomic<uint32_t> accepted{ 0 };
        std::vector<uint32_t> consumed;
        consumed.reserve(producer_count * values_per_producer);

        std::thread consumer([&] {
            uint32_t value = 0;
            while (!producers_done.load(std::memory_order_acquire)) {
                if (queue.Pop(&value)) consumed.push_back(value);
                else std::this_thread::yield();
            }
            while (queue.Pop(&value)) consumed.push_back(value);
        });

        std::vector<std::thread> producers;
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int index = 0; index < values_per_producer; ++index) {
                    const uint32_t value =
                        static_cast<uint32_t>(producer * values_per_producer + index);
                    if (queue.Push(value)) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (std::thread& producer : producers) producer.join();
        producers_done.store(true, std::memory_order_release);
        consumer.join();

        std::sort(consumed.begin(), consumed.end());
        const bool unique = std::adjacent_find(consumed.begin(), consumed.end()) == consumed.end();
        const uint64_t attempted = producer_count * values_per_producer;
        return Check(consumed.size() == accepted.load(), "concurrent queue accepted count") &&
            Check(unique, "concurrent queue contains no duplicate publications") &&
            Check(accepted.load() + queue.DroppedRows() == attempted,
                  "concurrent queue accounts for every publication attempt");
    }

    bool TestRetention()
    {
        RetainedRows<int, 3> rows;
        for (int value = 10; value <= 50; value += 10) rows.Append(value);
        return Check(rows.FirstSequence() == 3, "retention first sequence") &&
            Check(rows.NextSequence() == 6, "retention next sequence") &&
            Check(rows.Count() == 3, "retention bounded count") &&
            Check(rows.AtSequence(3).sequence == 3 && rows.AtSequence(3).row == 30,
                  "retention oldest row after wrap") &&
            Check(rows.AtSequence(5).sequence == 5 && rows.AtSequence(5).row == 50,
                  "retention newest row after wrap");
    }

    bool TestJson()
    {
        Value parsed;
        std::string error;
        const std::string input =
            R"({"array":[true,null,1.25],"text":"rocket \uD83D\uDE80"})";
        if (!Check(relimiter::json::Parse(input, &parsed, &error), "parse valid JSON")) {
            std::cerr << error << '\n';
            return false;
        }
        const Value* array = parsed.Find("array");
        const Value* text = parsed.Find("text");
        if (!Check(array && array->IsArray() && array->AsArray().size() == 3,
                   "parsed JSON array") ||
            !Check(text && text->AsString().find("rocket") == 0,
                   "parsed Unicode JSON string")) {
            return false;
        }

        const std::string serialized = relimiter::json::Stringify(parsed);
        Value round_trip;
        if (!Check(relimiter::json::Parse(serialized, &round_trip, &error),
                   "JSON round trip")) return false;
        if (!Check(!relimiter::json::Parse("{\"bad\":]", &round_trip, &error),
                   "reject invalid JSON")) return false;

        const std::string large_id = "{\"id\":9007199254740993}";
        if (!Check(relimiter::json::Parse(large_id, &round_trip, &error) &&
               relimiter::json::Stringify(round_trip) == large_id,
               "preserve JSON integers above double precision")) return false;

        const Value not_a_number = Value::Object{
            { "value", std::numeric_limits<double>::quiet_NaN() },
        };
        return Check(
            relimiter::json::Stringify(not_a_number) == "{\"value\":null}",
            "serialize non-finite telemetry as null");
    }

    bool TestPacingAnalysis()
    {
        std::vector<SequencedRow<SchedulerRowWire>> rows(100);
        for (size_t index = 0; index < rows.size(); ++index) {
            rows[index].sequence = index + 1;
            rows[index].row.actual_frame_time_us =
                index == 99 ? 30000.0 : 10000.0 + static_cast<double>(index % 3) * 25.0;
            rows[index].row.effective_interval_us = 10000.0;
            rows[index].row.jitter_us = static_cast<double>(index % 3) * 25.0;
            rows[index].row.wake_error_us = static_cast<double>(index % 5) - 2.0;
            rows[index].row.deadline_drift_us = static_cast<double>(index % 7) - 3.0;
            rows[index].row.predictor_warm = index >= 7;
            rows[index].row.overload = index < 5;
        }
        PacingAnalysisMetrics metrics;
        const auto stable_segment = LatestStableSchedulerSegment(rows);
        return Check(stable_segment.size() == 93, "latest stable warm segment") &&
            Check(AnalyzeSchedulerRows(rows, &metrics), "analyze scheduler rows") &&
            Check(metrics.valid_frame_times == 93, "analysis valid row count") &&
            Check(metrics.target_interval_us == 10000.0, "analysis target interval") &&
            Check(metrics.stutter_threshold_us == 20000.0, "analysis stutter threshold") &&
            Check(metrics.stutter_count == 1, "analysis stutter count") &&
            Check(metrics.overload_rows == 0, "analysis excludes cold overload rows") &&
            Check(metrics.warm_rows == 93, "analysis predictor warm count") &&
            Check(metrics.p99_frame_time_us > metrics.median_frame_time_us,
                  "analysis upper percentile");
    }
}

int main()
{
    if (!TestProtocolLayout() ||
        !TestProducerQueue() ||
        !TestConcurrentProducers() ||
        !TestRetention() ||
        !TestJson() ||
        !TestPacingAnalysis()) {
        return 1;
    }
    std::cout << "live debug protocol and analysis tests passed\n";
    return 0;
}