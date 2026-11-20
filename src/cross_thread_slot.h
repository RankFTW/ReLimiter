#pragma once

// Cross-thread PRESENT_START hand-off. Some engines send PRESENT_START on one
// thread and call Present on another (Gears of War: E-Day: the Present runs
// 0.8-5 ms after its own marker, and during a scene load often only after the
// next frame's marker). Two hand-offs live here, for two kinds of consumer.
//
// CrossThreadSlot holds the newest marker ("latest wins"). A Present on a
// thread without its own marker takes it exactly once; a Present on the
// marker's own thread claims it so no other thread takes it. The slot cannot
// tell whether the newest marker is the Present's own: a Present that runs a
// frame behind takes the next frame's marker. It serves consumers that check
// the pairing elsewhere or live with it (the native gate verifies it with ETW
// frame IDs; the GPU queue probe's attribution follows the same newest marker).
//
// CrossThreadRing keeps the newest eight markers, each under a key (the frame
// id the marker carries), and hands one over only by key. The consumer states
// which frame it is presenting (the frame id Streamline's
// NvAPI_D3D12_SetAsyncFrameMarker PRESENT_START states on the Present's own
// thread) and gets that frame's marker or nothing: a Present that runs behind
// takes its own marker, not the newest one, and a key that is absent, evicted,
// rejected or already consumed fails closed.
//
// Every operation is a bounded try-lock (a few retries over a copy of a few
// hundred bytes): contention never blocks a hook beyond that, it only loses
// one hand-off (the consumer's own checks then fail closed). Pure C++17, no
// Windows dependency.

#include <atomic>
#include <cstdint>
#include <type_traits>

// Holds one value; publishing replaces it, so an empty value (a rejected
// marker) also withdraws the previous one.
template <typename T>
class CrossThreadSlot {
public:
    // Marker thread: the newest marker (or an empty one, valid == false).
    // Returns its sequence, or 0 when contended (nothing published).
    uint64_t Publish(const T& value, bool valid) noexcept
    {
        if (!TryLock()) return 0;
        const uint64_t sequence = ++next_;
        value_ = value;
        sequence_ = sequence;
        valid_ = valid;
        Unlock();
        return sequence;
    }

    // Updates the published value of `sequence` while it is still the newest.
    template <typename Update>
    void Amend(uint64_t sequence, Update update) noexcept
    {
        if (sequence == 0 || !TryLock()) return;
        if (sequence_ == sequence) update(&value_);
        Unlock();
    }

    // Consumer that has its own same-thread marker of `sequence`.
    void Claim(uint64_t sequence) noexcept
    {
        if (sequence == 0 || !TryLock()) return;
        if (sequence_ == sequence) claimed_ = sequence;
        Unlock();
    }

    // Consumer without its own marker: the newest valid, unclaimed value,
    // exactly once.
    bool Take(T* value) noexcept
    {
        if (value == nullptr || !TryLock()) return false;
        const bool available = valid_ && sequence_ != 0 && sequence_ != claimed_;
        if (available) {
            *value = value_;
            claimed_ = sequence_;
        }
        Unlock();
        return available;
    }

private:
    bool TryLock() noexcept
    {
        for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
            if (!lock_.test_and_set(std::memory_order_acquire)) return true;
        }
        return false;
    }
    static constexpr int kLockAttempts = 64;
    void Unlock() noexcept { lock_.clear(std::memory_order_release); }

    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    T value_{};               // guarded by lock_
    uint64_t next_ = 0;       // guarded
    uint64_t sequence_ = 0;   // guarded: sequence of value_; 0 = none
    uint64_t claimed_ = 0;    // guarded: newest sequence claimed or taken
    bool valid_ = false;      // guarded
};

// The newest kCapacity markers, taken by key. Sequences rise with every
// publication and are never reused; an entry is consumed when it or a newer
// entry has been taken or claimed, so an older frame is never handed out after
// a newer one (a frame that never presented cannot be taken late).
template <typename T>
class CrossThreadRing {
    static_assert(std::is_trivially_copyable<T>::value, "the ring copies its payload under a spin lock");

public:
    static constexpr unsigned kCapacity = 8;

    // Marker thread: the marker of the frame `key`, or a tombstone
    // (valid == false) for a marker that was rejected. A tombstone takes a place
    // and advances the sequence but withdraws nothing: older valid entries stay
    // takable by their own key. Evicts the oldest entry. Returns the sequence,
    // or 0 when contended (nothing published).
    uint64_t Publish(const T& value, uint64_t key, bool valid) noexcept
    {
        if (!TryLock()) return 0;
        const uint64_t sequence = ++next_;
        Entry& entry = entries_[sequence % kCapacity];
        entry.value = value;
        entry.key = key;
        entry.sequence = sequence;
        entry.valid = valid;
        Unlock();
        return sequence;
    }

    // Consumer that has its own same-thread marker of `sequence`: that entry and
    // every older one are consumed.
    void Claim(uint64_t sequence) noexcept
    {
        if (sequence == 0 || !TryLock()) return;
        if (sequence <= next_ && sequence > consumed_) consumed_ = sequence;
        Unlock();
    }

    // Consumer without its own marker, with the frame id it presents: the newest
    // valid, unconsumed entry of that key. It and every older entry become
    // consumed (the resync). False when the key is absent (never published or
    // evicted), is only a tombstone, or was consumed, and when contended.
    bool TakeKey(uint64_t key, T* value) noexcept
    {
        if (value == nullptr || !TryLock()) return false;
        const Entry* found = nullptr;
        for (const Entry& entry : entries_) {
            if (entry.sequence > consumed_ && entry.valid && entry.key == key &&
                (!found || entry.sequence > found->sequence)) found = &entry;
        }
        if (found) {
            *value = found->value;
            consumed_ = found->sequence;
        }
        Unlock();
        return found != nullptr;
    }

    // Tests: hold the lock from the calling thread (false when it did not come
    // free) and release it, to make another call lose its hand-off.
    bool HoldLockForTest() noexcept { return TryLock(); }
    void ReleaseLockForTest() noexcept { Unlock(); }

private:
    struct Entry {
        T value{};
        uint64_t key = 0;
        uint64_t sequence = 0;    // 0 = never written
        bool valid = false;
    };
    bool TryLock() noexcept
    {
        for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
            if (!lock_.test_and_set(std::memory_order_acquire)) return true;
        }
        return false;
    }
    static constexpr int kLockAttempts = 64;
    void Unlock() noexcept { lock_.clear(std::memory_order_release); }

    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    Entry entries_[kCapacity]{};   // guarded by lock_; entry n lives at n % kCapacity
    uint64_t next_ = 0;            // guarded: the newest sequence
    uint64_t consumed_ = 0;        // guarded: every sequence up to this one is consumed
};
