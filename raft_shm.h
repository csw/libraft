// -*- c++ -*-
#ifndef RAFT_SHM_H
#define RAFT_SHM_H

#include <atomic>
#include <thread>
#include <vector>
#include <sys/types.h>

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

namespace raft {

using boost::interprocess::managed_shared_memory;
using boost::interprocess::interprocess_mutex;
using boost::interprocess::interprocess_condition;

using shm_handle = managed_shared_memory::handle_t;

const static size_t SHM_SIZE = 64 * 1024 * 1024;

enum class CallType {
    Apply
};

enum class CallState {
    Pending, Dispatched, Success, Error
};

class CallSlot
{
public:
    CallSlot() = default;

    CallSlot(CallSlot&) = delete;
    CallSlot& operator=(CallSlot&) = delete;

    void reset();

    CallType                call_type;
    // atomic?
    uint32_t                refcount;
    CallState               state;
    shm_handle              handle;
    interprocess_mutex      owned;
    bool                    call_ready;
    interprocess_condition  call_cond;
    bool                    ret_ready;
    interprocess_condition  ret_cond;
private:
    interprocess_mutex      slot_busy;
    friend class SlotHandle;
    friend class Scoreboard;
};

class Scoreboard
{
public:
    Scoreboard();
    std::atomic<bool> is_leader;
    CallSlot slots[16];

    Scoreboard(Scoreboard&) = delete;
    Scoreboard& operator=(Scoreboard&) = delete;
private:
    friend class SlotHandle;
    CallSlot& grab_slot();
};

extern Scoreboard* scoreboard;

class SlotHandle
{
public:
    SlotHandle(Scoreboard&);
    ~SlotHandle();

    SlotHandle(SlotHandle&) = delete;
    SlotHandle& operator=(SlotHandle&) = delete;

    CallSlot& slot;

private:
    std::unique_lock<interprocess_mutex> slot_lock;
};

extern managed_shared_memory shm;

void init(const char* name, bool create);

// call structs

struct ApplyCall {
    shm_handle cmd_buf;
    size_t     cmd_len;
    uint64_t   timeout_ns;
    uint64_t   dispatch_ns;
    uintptr_t  response;
    char       errmsg[64];
    uint32_t   errlen;
};

}

#endif /* RAFT_SHM_H */
