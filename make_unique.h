// -*- c++ -*-
#ifndef _MAKE_UNIQUE_H_
#define _MAKE_UNIQUE_H_

#include <memory>
#include <stdexcept>

template <typename T, typename ...Args>
std::unique_ptr<T> make_unique(Args&& ...args)
{
    T* ptr = new T(std::forward<Args>(args)...);
    if (ptr) {
        return std::unique_ptr<T>(ptr);
    } else {
        throw std::bad_alloc();
    }
}

#endif /* _MAKE_UNIQUE_H_ */
