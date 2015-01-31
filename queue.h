// -*- c++ -*-
#ifndef QUEUE_H
#define QUEUE_H

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace queue {

template <typename T, uint32_t Capacity,
          typename Mutex=std::mutex,
          typename CV=std::condition_variable>
class ArrayBlockingQueue
{
public:
    ArrayBlockingQueue() = default;

    void put(T val)
    {
        std::unique_lock<Mutex> lock(mutex);
        if (count == Capacity) {
            not_full.wait(lock, [&] () { return count < Capacity; });
        }

        array[inc(head)] = val;
        ++count;
        not_empty.notify_one();
    }

    T take()
    {
        std::unique_lock<Mutex> lock(mutex);
        if (count == 0) {
            not_empty.wait(lock, [&] () { return count > 0; });
        }

        T val = array[inc(tail)];
        --count;
        not_full.notify_one();
        return val;
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

    T array[Capacity];
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    Mutex mutex;
    CV not_empty;
    CV not_full;

};

}

#endif /* QUEUE_H */
