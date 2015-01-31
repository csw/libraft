// -*- c++ -*-
#ifndef LOCKS_H
#define LOCKS_H

#include <pthread.h>

namespace locks {

template <typename L>
void unlocker(void* lock_ptr)
{
    L* lock = (L*) lock_ptr;
    lock->unlock();
}

template <typename L>
void unlocker_checked(void* lock_ptr)
{
    L& lock = *((L*) lock_ptr);
    if (lock)
        lock.unlock();
}

}

#endif /* LOCKS_H */
