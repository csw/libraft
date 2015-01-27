// -*- c++ -*-
#ifndef RAFT_SHM_H
#define RAFT_SHM_H

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/allocators/private_node_allocator.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/smart_ptr/unique_ptr.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

#include "queue.h"
#include "raft_defs.h"
#include "raft_c_if.h"

namespace raft {

using boost::interprocess::anonymous_instance;
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

enum class CallTag {
    Invalid, Apply, Barrier, VerifyLeader, 
        AddPeer, RemovePeer, SetPeers, Shutdown, Snapshot, 
        FSMApply=100, FSMSnapshot, FSMRestore };

enum class CallState {
    Pending, Dispatched, Success, Error
};

bool is_terminal(CallState state);

template<typename CT> class SlotHandle;

// call structs

struct NoArgs {};

struct ApplyArgs {
    ApplyArgs(offset_ptr<char> cmd_buf_, size_t cmd_len_, uint64_t timeout_ns_);
    ApplyArgs() = delete;

    offset_ptr<char> cmd_buf;
    size_t           cmd_len;
    uint64_t         timeout_ns;
};

struct LogEntry {
    LogEntry(uint64_t index, uint64_t term, raft_log_type log_type,
             shm_handle data_buf, size_t data_len);
    LogEntry() = delete;

    uint64_t      index;
    uint64_t      term;
    raft_log_type log_type;
    shm_handle    data_buf;
    size_t        data_len;
};

struct Filename {
    Filename(const char* path);

    const static size_t MAXLEN = 255;

    char          path[MAXLEN+1];
};

struct NetworkAddr {
    NetworkAddr(const char* host, uint16_t port);

    char          host[256];
    uint16_t      port;
};

struct NoReturn {};

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

class BaseSlot
{
public:
    using pointer = offset_ptr<BaseSlot>;
    using call_rec = std::pair<CallTag, BaseSlot::pointer>;

    BaseSlot(CallTag tag);

    virtual ~BaseSlot() = default;

    interprocess_mutex      owned;
    bool                    ret_ready;
    interprocess_condition  ret_cond;

    CallTag                 tag;
    uint32_t                refcount;

    // atomic?
    CallState               state;

    uint64_t                retval;
    RaftError               error;
    Timings                 timings;

    BaseSlot(BaseSlot&) = delete;
    BaseSlot& operator=(BaseSlot&) = delete;

    call_rec rec();

    void reply(RaftError err);
    void reply(uint64_t retval);

    void wait();
    virtual void dispose() = 0;

    virtual RaftError get_ptr(void **res) = 0;
};

template <typename ArgT, bool HasRet>
class CallSlot : public BaseSlot
{
public:
    template<typename... Args>
    CallSlot(CallTag tag_, Args... argv)
        : BaseSlot(tag_),
          args(argv...)
    {}

    CallSlot(CallSlot&) = delete;
    CallSlot& operator=(CallSlot&) = delete;

    ArgT                    args;

    RaftError get_ptr(void** res)
    {
        if (! is_terminal(state)) {
            wait();
        }

        if (HasRet) {
            if (res) {
                if (error == RAFT_SUCCESS) {
                    *res = (void*) retval;
                }
                return error;
            } else {
                return RAFT_E_INVALID_ADDRESS;
            }
        } else {
            return RAFT_E_INVALID_OP;
        }
    }
    
    void dispose()
    {
        
        std::unique_lock<interprocess_mutex> lock(owned);
        if (ret_ready) {
            lock.unlock();
            shm.destroy_ptr(this);
        } else {
            assert(false && "not implemented");
        }
    }
};

extern template class CallSlot<NoArgs, true>;
extern template class CallSlot<NoArgs, false>;
extern template class CallSlot<ApplyArgs, true>;
extern template class CallSlot<LogEntry, true>;

class Scoreboard
{
public:
    Scoreboard();

    void wait_for_raft(pid_t raft_pid);

    std::atomic<bool> is_raft_running;
    std::atomic<bool> is_leader;

    RaftConfig        config;
    
    // TODO: look at using boost::interprocess::message_queue
    queue::ArrayBlockingQueue<BaseSlot::call_rec, 8,
                              interprocess_mutex,
                              interprocess_condition> api_queue;

    queue::ArrayBlockingQueue<BaseSlot::call_rec, 8,
                              interprocess_mutex,
                              interprocess_condition> fsm_queue;

    Scoreboard(Scoreboard&) = delete;
    Scoreboard& operator=(Scoreboard&) = delete;
};

bool in_shm_bounds(void* ptr);

void process_args(int argc, char *argv[]);

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

}

#endif /* RAFT_SHM_H */
