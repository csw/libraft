// -*- c++ -*-
#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>

using std::atomic;
using std::chrono::time_point;

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
        while (count == Capacity) {
            not_full.wait(lock);
            check_closed();
        }

        array[inc(head)] = val;
        ++count;
        not_empty.notify_one();
    }

    T take()
    {
        check_closed();
        std::unique_lock<Mutex> lock(mutex);
        while (count == 0) {
            not_empty.wait(lock);
            check_closed();
        }

        T val = array[inc(tail)];
        --count;
        not_full.notify_one();
        return val;
    }

    void close()
    {
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

template <typename T,
          typename Mutex=std::timed_mutex,
          typename CV=std::condition_variable_any>
class Deque
{
public:
    using lock_t = std::unique_lock<Mutex>;

    Deque() = default;

    void put(T val)
    {
        lock_t lock(mutex);
        check_closed();
        deque.push_back(val);
        ready.notify_one();
    }

    template <typename ...Args>
    void emplace(Args... args)
    {
        lock_t lock(mutex);
        check_closed();
        deque.emplace_back(args...);
        ready.notify_one();
    }

    T take()
    {
        lock_t lock(mutex);
        check_closed();
        while (deque.empty()) {
            ready.wait(lock);
            check_closed();
        }
        T val = std::move(deque.front());
        deque.pop_front();
        return val;
    }

    template <typename Clock, typename Duration>
    std::pair<bool, T> poll_until(const time_point<Clock, Duration>& end_time)
    {
        std::unique_lock<Mutex> lock(mutex, end_time);
        if (lock) {
            check_closed();
            while (deque.empty()) {
                if (ready.wait_until(lock, end_time) == std::cv_status::timeout) {
                    return { false, T() };
                }
                check_closed();
            }
            T val = std::move(deque.front());
            deque.pop_front();
            return { true, val };
        } else {
            return { false, T() };
        }
    }

    void close()
    {
        std::unique_lock<Mutex> lock(mutex);
        closed = true;
        ready.notify_all();
    }

    void reset()
    {
        std::unique_lock<Mutex> lock(mutex);
        closed = false;
        deque.clear();
        ready.notify_all();
    }

    // not copyable
    Deque(Deque&) = delete;
    Deque& operator=(Deque&) = delete;

private:
    void check_closed()
    {
        if (closed)
            throw queue_closed();
    }

    std::deque<T> deque;
    Mutex         mutex;
    CV            ready;
    bool          closed = false;
};

}

#endif /* QUEUE_H */
