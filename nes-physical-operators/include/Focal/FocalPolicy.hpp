#pragma once
#include <atomic>
#include <shared_mutex>

struct SingleThreadedPolicy
{
    using Counter = uint32_t;
    using Flag    = uint8_t;
    // no-op lock
    struct NullMutex
    {
        void lock() {}
        void unlock() {}
        bool try_lock() { return true; }
        void lock_shared() {}
        void unlock_shared() {}
        bool try_lock_shared() { return true; }
    };
    using Mutex = NullMutex;

    static bool decrementAndIsZero(Counter& c) { return --c == 0; }
    static void markBit(Flag& b) { b = 1; }
    static bool testBit(const Flag& b) { return b != 0; }
    static bool testAndSetPresent(Flag& b) { const bool was = b != 0; b = 1; return was; }
    static uint32_t loadCounter(const Counter& c) { return c; }
    static void storeCounter(Counter& c, uint32_t v) { c = v; }
};

struct MultiThreadedPolicy
{
    using Counter = std::atomic<uint32_t>;
    using Flag    = std::atomic<uint8_t>;
    using Mutex   = std::shared_mutex;

    static bool decrementAndIsZero(Counter& c) { return c.fetch_sub(1, std::memory_order_acq_rel) == 1; }
    static void markBit(Flag& b) { b.store(1, std::memory_order_relaxed); }
    static bool testBit(const Flag& b) { return b.load(std::memory_order_relaxed) != 0; }
    static bool testAndSetPresent(Flag& b) { return b.exchange(1, std::memory_order_relaxed) != 0; }
    static uint32_t loadCounter(const Counter& c) { return c.load(std::memory_order_relaxed); }
    static void storeCounter(Counter& c, uint32_t v) { c.store(v, std::memory_order_relaxed); }
};