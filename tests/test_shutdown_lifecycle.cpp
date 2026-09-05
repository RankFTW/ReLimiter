// Direct tests of production ShutdownAdmission; the worker/reload ordering
// cases below are deliberately standalone models, not Windows integration.
// Manual integration still required: pause a first Present before it marks
// present_seen, begin unload, then resume it and verify reload is rejected;
// block/fault the display worker's driver set and verify restore follows that
// call even after stop times out; block an init-phase logger write and verify
// shutdown retains the file/module rather than closing beneath the writer.
#include "../src/shutdown_admission.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

static void TestAdmission() {
    ShutdownAdmission admission;
    Check(admission.IsDrained(), "initial state is closed and drained");
    Check(!admission.TryEnter(), "initial state rejects writers");
    Check(admission.Open(), "open a fresh lifetime");
    Check(!admission.Open(), "cannot reopen an already open lifetime");
    Check(admission.TryEnter(), "first writer admitted");
    Check(admission.TryEnter(), "second writer admitted");
    admission.Close();
    admission.Close();
    Check(!admission.TryEnter(), "close rejects new writers");
    Check(!admission.IsDrained(), "close does not hide admitted writers");
    Check(!admission.Open(), "cannot reopen retained writers");
    admission.Leave();
    Check(!admission.IsDrained(), "one writer still owns the file");
    admission.Leave();
    Check(admission.IsDrained(), "all writers drained");
    Check(admission.Open(), "clean logger reload allowed");
    admission.Close();
}

static void TestPendingProducerPublication() {
    ShutdownAdmission admission;
    Check(admission.Open(), "open concurrent test");
    std::atomic<bool> entered{false};
    std::atomic<bool> resume{false};
    bool published = false;
    std::thread producer([&] {
        Check(admission.TryEnter(), "producer enters before shutdown");
        entered.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
        published = true;
        admission.Leave();
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    admission.Close();
    Check(!admission.IsDrained(), "paused synchronous writer retains ownership");
    Check(!admission.Open(), "retained late writer prevents reinitialization");
    resume.store(true, std::memory_order_release);
    while (!admission.IsDrained()) std::this_thread::yield();
    Check(published, "drain acquires producer publication before destruction");
    producer.join();
}

static void TestAdmissionCloseRace() {
    // Fixed iteration count, no random inputs. Exercises both outcomes of the
    // same-word admission/close CAS race without any sleep/timing assertions.
    for (int i = 0; i < 256; ++i) {
        ShutdownAdmission admission;
        Check(admission.Open(), "open race lifetime");
        std::atomic<bool> ready{false};
        std::atomic<bool> go{false};
        std::thread producer([&] {
            ready.store(true, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (admission.TryEnter()) admission.Leave();
        });
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        admission.Close();
        producer.join();
        Check(admission.IsDrained(), "raced close neither leaks nor loses writers");
        Check(!admission.TryEnter(), "raced close stays terminal");
    }
}

static void TestShutdownOrderModels() {
    // Mirrors StopOwnedProducers: a failed earlier stop must not skip later
    // stop calls, and no dependent resource is released on any failure.
    for (int failed_worker = 0; failed_worker < 9; ++failed_worker) {
        std::vector<int> stops;
        bool all_stopped = true;
        for (int worker = 0; worker < 9; ++worker) {
            const auto stop = [&] {
                stops.push_back(worker);
                return worker != failed_worker;
            };
            all_stopped = stop() && all_stopped;
        }
        Check(stops.size() == 9, "all producer stops attempted despite failure");
        Check(!all_stopped, "failed producer retains dependencies and logger");
    }

    // Models the existing on_present wrapper: mark seen BEFORE late-init work.
    // A pre-shutdown read would allow the retained late-init flag to poison a
    // reload; the decision belongs after the admitted callback completes.
    bool present_seen = false;
    bool late_init_attempted = false;
    const bool stale_reload_decision = !present_seen;
    present_seen = true; // admitted callback resumes during rundown
    late_init_attempted = true;
    const bool startup_reload_allowed = !present_seen; // after callback drain
    Check(stale_reload_decision && late_init_attempted,
          "model reproduces first-Present race");
    Check(!startup_reload_allowed, "post-drain decision rejects poisoned reload");

    // Models an in-flight driver disable completing after Stop times out. Only
    // worker final cleanup may restore; a caller-side restore would be undone.
    for (bool driver_fault : {false, true}) {
        std::vector<int> operations;
        bool override_applied = false;
        operations.push_back(1); // worker enters disable
        operations.push_back(2); // caller requests stop and times out
        override_applied = true; // driver may apply before returning OR faulting
        operations.push_back(driver_fault ? 4 : 3);
        override_applied = false; // unconditional worker final cleanup
        operations.push_back(5);
        Check(operations.front() == 1 && operations[1] == 2 &&
              operations.back() == 5 && !override_applied,
              "final cleanup follows last disable on success and fault");
    }
}

int main() {
    TestAdmission();
    TestPendingProducerPublication();
    TestAdmissionCloseRace();
    TestShutdownOrderModels();
    std::cout << "Shutdown admission and lifecycle order checks passed\n";
    return 0;
}