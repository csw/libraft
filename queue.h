// -*- c++ -*-
#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>

#include "locks.h"

using std::atomic;

namespace queue {

class queue_closed : public std::runtime_error
{
public:
    queue_closed()
        : std::runtime_error("queue closed!")
    {}

    ~queue_closed() {}
};

template <typename T, uint32_t Capacity,
          typename Mutex=std::mutex,
          typename CV=std::condition_variable>
class ArrayBlockingQueue
{
public:
    ArrayBlockingQueue() = default;

    void put(T val)
    {
        check_closed();
        std::unique_lock<Mutex> lock(mutex);
        pthread_cleanup_push(locks::unlocker_checked<decltype(lock)>, &lock);
        while (count == Capacity) {
            not_full.wait(lock);
            check_closed();
        }

        array[inc(head)] = val;
        ++count;
        not_empty.notify_one();
        pthread_cleanup_pop(0);
    }

    T take()
    {
        check_closed();
        std::unique_lock<Mutex> lock(mutex);
        pthread_cleanup_push(locks::unlocker_checked<decltype(lock)>, &lock);
        while (count == 0) {
            not_empty.wait(lock);
            check_closed();
        }

        T val = array[inc(tail)];
        --count;
        not_full.notify_one();
        return val;
        pthread_cleanup_pop(0); // macro craziness here...
    }

    void close() {
        std::unique_lock<Mutex> lock(mutex);
        closed = true;
        not_full.notify_all();
        not_empty.notify_all();
    }

    // not copyable
    ArrayBlockingQueue(ArrayBlockingQueue&) = delete;
    ArrayBlockingQueue& operator=(ArrayBlockingQueue&) = delete;

private:
    uint32_t inc(uint32_t& i)
    {
        uint32_t prev = i;
        if (i == Capacity-1) {
            i = 0;
        } else {
            ++i;
        }
        return prev;
    }

    void check_closed()
    {
        if (closed)
            throw queue_closed();
    }

    T array[Capacity];
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    Mutex         mutex;
    CV            not_empty;
    CV            not_full;
    atomic<bool>  closed { false };
};

}

#endif /* QUEUE_H */
