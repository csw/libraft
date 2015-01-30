// -*- c++ -*-
#ifndef STATS_H
#define STATS_H

#include <atomic>
#include <memory>
#include <cstdint>

namespace raft {

class Stats {
public:

    class Counter {
    public:
        std::atomic<uint64_t> c;

        inline operator uint64_t() const {
            return c.load(std::memory_order_relaxed);
        }

        inline void inc() {
            c.fetch_add(1, std::memory_order_relaxed);
        }
    };

    Counter buffer_alloc;
    Counter buffer_free;
};

extern std::unique_ptr<Stats> stats;

void init_stats();

}

#undef CDEF

#endif /* STATS_H */
