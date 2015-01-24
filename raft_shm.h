// -*- c++ -*-
#ifndef RAFT_SHM_H
#define RAFT_SHM_H

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/smart_ptr/unique_ptr.hpp>
#include <boost/interprocess/allocators/private_node_allocator.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

#include "raft_defs.h"

namespace raft {

using boost::interprocess::managed_mapped_file;
using boost::interprocess::offset_ptr;
using boost::interprocess::private_node_allocator;
using boost::interprocess::interprocess_mutex;
using boost::interprocess::interprocess_condition;

using shm_handle = managed_mapped_file::handle_t;
using mutex_lock = std::unique_lock<boost::interprocess::interprocess_mutex>;

class Scoreboard;

// TODO: add knobs for these
const static char SHM_PATH[] = "/tmp/raft_shm";
const static size_t SHM_SIZE = 64 * 1024 * 1024;

extern pid_t               raft_pid;
extern managed_mapped_file shm;
extern Scoreboard*         scoreboard;

template <typename T>
using pool_allocator =
    private_node_allocator<T, decltype(shm)::segment_manager, 32>;

template <typename T>
using unique_ptr = boost::interprocess::unique_ptr<T, pool_allocator<T> >;

template <typename T>
unique_ptr<T> alloc_unique(pool_allocator<T>* alloc);

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

class Timings
{
public:
    using clock = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;

    Timings() = default;
    Timings(time_point t);

    void record(const char *tag);
    void record(const char *tag, time_point t);
    void print();
private:

    const static uint32_t MAX_ENT = 32;

    struct entry {
        char       tag[20];
        time_point ts;
    };

    uint32_t n_entries;
    entry    entries[MAX_ENT];
};

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
    union {
        ApplyCall           apply;
        LogEntry            log_entry;
    } call;
    uintptr_t               retval;
    RaftError               error;
    Timings                 timings;

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

bool in_shm_bounds(void* ptr);

/**
 * Set up shared memory and any resident resources.
 *
 * @param name currently ignored...
 * @param create whether to create a new shared memory region (for the client) 
 *               or map an existing one (for the Raft side).
 */
void shm_init(const char* name, bool create);

/**
 * Start the Raft process.
 *
 * To be called from the client after shm_init().
 */
pid_t run_raft();

template <typename T>
unique_ptr<T> alloc_unique(pool_allocator<T>* alloc)
{
    T* obj = alloc->allocate_one();
    return boost::interprocess::make_managed_unique_ptr(obj, *alloc);
}


}

#endif /* RAFT_SHM_H */
