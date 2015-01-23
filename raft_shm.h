// -*- c++ -*-
#ifndef RAFT_SHM_H
#define RAFT_SHM_H

#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

#include "raft_defs.h"

namespace raft {

using boost::interprocess::managed_shared_memory;
using boost::interprocess::interprocess_mutex;
using boost::interprocess::interprocess_condition;

using shm_handle = managed_shared_memory::handle_t;
using mutex_lock = std::unique_lock<boost::interprocess::interprocess_mutex>;

const static size_t SHM_SIZE = 64 * 1024 * 1024;

enum class APICall {
    Apply
};

enum class FSMOp {
    Apply
};

enum class CallState {
    Pending, Dispatched, Success, Error
};

template<typename CT> class SlotHandle;

template<typename CT>
class CallSlot
{
public:
    CallSlot() = default;

    CallSlot(CallSlot&) = delete;
    CallSlot& operator=(CallSlot&) = delete;

    CT                      call_type;
    // atomic?
    uint32_t                refcount;
    CallState               state;
    shm_handle              handle;
    uintptr_t               retval;
    RaftError               error;

    interprocess_mutex      owned;
    bool                    call_ready;
    interprocess_condition  call_cond;
    bool                    ret_ready;
    interprocess_condition  ret_cond;
private:
    interprocess_mutex      slot_busy;
    friend class SlotHandle<CT>;
    friend class Scoreboard;
};
using RaftCallSlot = CallSlot<APICall>;
using FSMCallSlot = CallSlot<FSMOp>;

class Scoreboard
{
public:
    Scoreboard();

    void wait_for_raft(pid_t raft_pid);

    std::atomic<bool> is_raft_running;
    std::atomic<bool> is_leader;
    RaftCallSlot slots[16];
    FSMCallSlot  fsm_slot;

    Scoreboard(Scoreboard&) = delete;
    Scoreboard& operator=(Scoreboard&) = delete;
    RaftCallSlot& grab_slot();
};

extern Scoreboard* scoreboard;

template<typename CT>
class SlotHandle
{
public:
    SlotHandle(CallSlot<CT>& slot);
    SlotHandle(CallSlot<CT>& slot, std::adopt_lock_t _t);
    ~SlotHandle();

    SlotHandle(SlotHandle&) = delete;
    SlotHandle& operator=(SlotHandle&) = delete;

    CallSlot<CT>& slot;

private:
    std::unique_lock<interprocess_mutex> slot_lock;
};

extern managed_shared_memory shm;

bool in_shm_bounds(void* ptr);

void shm_init(const char* name, bool create);

pid_t run_raft();

// call structs

struct ApplyCall {
    shm_handle cmd_buf;
    size_t     cmd_len;
    uint64_t   timeout_ns;
    uint64_t   dispatch_ns;
};

struct LogEntry {
    uint64_t      index;
    uint64_t      term;
    raft_log_type log_type;
    shm_handle    data_buf;
    size_t        data_len;
};

}

#endif /* RAFT_SHM_H */
