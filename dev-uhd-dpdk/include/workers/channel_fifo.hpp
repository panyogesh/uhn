#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <utility>

namespace flexsdr {

// Single-producer single-consumer ring (lock-free).
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity_pow2 = 1024)
      : _cap(capacity_pow2 ? capacity_pow2 : 1024),
        _mask(_cap - 1),
        _buf(_cap) {
        // require power-of-two
        if ((_cap & (_cap - 1)) != 0) {
            // round up to next power-of-two
            std::size_t p = 1; while (p < _cap) p <<= 1;
            _cap = p; _mask = _cap - 1; _buf.assign(_cap, T{});
        }
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

    bool push(const T& v) {
        auto h = _head.load(std::memory_order_relaxed);
        auto n = h + 1;
        if (n - _tail.load(std::memory_order_acquire) > _cap) return false; // full
        _buf[h & _mask] = v;
        _head.store(n, std::memory_order_release);
        return true;
    }
    bool push(T&& v) {
        auto h = _head.load(std::memory_order_relaxed);
        auto n = h + 1;
        if (n - _tail.load(std::memory_order_acquire) > _cap) return false;
        _buf[h & _mask] = std::move(v);
        _head.store(n, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        auto t = _tail.load(std::memory_order_relaxed);
        if (t == _head.load(std::memory_order_acquire)) return false; // empty
        out = std::move(_buf[t & _mask]);
        _tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool peek(T& out) const {
        auto t = _tail.load(std::memory_order_relaxed);
        if (t == _head.load(std::memory_order_acquire)) return false;
        out = _buf[t & _mask];
        return true;
    }

    std::size_t size() const {
        auto h = _head.load(std::memory_order_acquire);
        auto t = _tail.load(std::memory_order_acquire);
        return (h - t);
    }
    std::size_t capacity() const { return _cap; }
    bool empty() const { return size() == 0; }
    void clear() {
        _tail.store(_head.load(std::memory_order_relaxed), std::memory_order_release);
    }

private:
    std::size_t              _cap;
    std::size_t              _mask;
    std::vector<T>           _buf;
    std::atomic<std::size_t> _head{0};
    std::atomic<std::size_t> _tail{0};
};

} // namespace flexsdr
