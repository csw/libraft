// -*- c++ -*-
#ifndef LOCKS_H
#define LOCKS_H

#include <cstdio>
#include <pthread.h>

namespace locks {

template <typename L>
void unlocker(void* lock_ptr)
{
    L* lock = (L*) lock_ptr;
    fprintf(stderr, "Releasing lock (unchecked) during cancellation!\n");
    lock->unlock();
}

template <typename L>
void unlocker_checked(void* lock_ptr)
{
    L& lock = *((L*) lock_ptr);
    if (lock.owns_lock()) {
        fprintf(stderr, "Releasing lock (checked) during cancellation!\n");
        lock.unlock();
    }
}

}

#endif /* LOCKS_H */
