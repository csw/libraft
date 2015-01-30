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

#include "zlog/src/zlog.h"

#include "queue.h"
#include "raft_defs.h"
#include "raft_c_if.h"
#include "stats.h"

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

extern zlog_category_t*    msg_cat;
extern zlog_category_t*    fsm_cat;
extern zlog_category_t*    shm_cat;

extern pid_t               raft_pid;
extern managed_mapped_file shm;
extern Scoreboard*         scoreboard;
extern bool                shutdown_requested;

enum class CallTag {
    Invalid, Apply, Barrier, VerifyLeader, 
        AddPeer, RemovePeer, SetPeers, Shutdown, Snapshot, 
        FSMApply=100, FSMSnapshot, FSMRestore };

enum class CallState {
    Pending, Dispatched, Success, Error
};

enum class ClientState {
    Issued, Observed, Abandoned
};

bool is_terminal(CallState state);
bool in_shm_bounds(const void* ptr);

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

struct BarrierArgs {
    BarrierArgs(uint64_t timeout_ns);

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

template <typename ArgT>
void orphan_cleanup(const ArgT arg) { (void) arg; }

void orphan_cleanup(const ApplyArgs args);

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

    interprocess_mutex       owned;
    bool                     ret_ready;
    interprocess_condition   ret_cond;

    const CallTag            tag;

    // atomic?
    CallState                state;
    std::atomic<ClientState> client_state;

    uint64_t                 retval;
    RaftError                error;
    Timings                  timings;

    BaseSlot(BaseSlot&) = delete;
    BaseSlot& operator=(BaseSlot&) = delete;

    call_rec rec();

    void reply(RaftError err);
    void reply(uint64_t retval);

    void wait();
    virtual void dispose() = 0;

    virtual RaftError get_ptr(void **res) = 0;
};

template <typename Call>
class CallSlot : public BaseSlot
{
public:
    template<typename... Args>
    CallSlot(Args... argv)
        : BaseSlot(Call::tag),
          args(argv...)
    {}

    CallSlot(CallSlot&) = delete;
    CallSlot& operator=(CallSlot&) = delete;

    const typename Call::arg_t         args;

    RaftError get_ptr(void** res)
    {
        if (! is_terminal(state)) {
            wait();
        }

        if (Call::has_ret) {
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
        switch (client_state.load(std::memory_order_relaxed)) {
        case ClientState::Issued:
            client_state = ClientState::Abandoned;
            track_orphan(this);
            break;
        case ClientState::Observed:
            Call::allocator->deallocate_one(this);
            stats->call_free.inc();
            break;
        case ClientState::Abandoned:
            orphan_cleanup(args);
            Call::allocator->deallocate_one(this);
            stats->call_free.inc();
            break;
        }
    }
};

namespace api {

#define api_call(name, argT, hasRet)                                    \
    struct name {                                                       \
        const static CallTag tag = CallTag::name;                       \
        const static bool    has_ret = hasRet;                          \
        using arg_t = argT;                                             \
        using slot_t = CallSlot<name>;                                  \
        using allocator_t = private_node_allocator<slot_t, decltype(shm)::segment_manager, 256>; \
        static allocator_t* allocator;                                  \
    };
#include "raft_api_calls.h"
#include "raft_fsm_calls.h"
#undef api_call

}


using CallQueue =
    queue::ArrayBlockingQueue<BaseSlot::call_rec, 8,
                              interprocess_mutex,
                              interprocess_condition>;

class Scoreboard
{
public:
    Scoreboard();

    void wait_for_raft(pid_t raft_pid);

    std::atomic<bool> is_raft_running;
    std::atomic<bool> is_leader;

    RaftConfig        config;
    
    // TODO: look at using boost::interprocess::message_queue
    CallQueue api_queue;
    CallQueue fsm_queue;

    Scoreboard(Scoreboard&) = delete;
    Scoreboard& operator=(Scoreboard&) = delete;
};

template <typename Call, typename... Args>
typename Call::slot_t* send_request(CallQueue& queue, Args... argv)
{
    auto start_t = Timings::clock::now();
    stats->call_alloc.inc();
    auto slot_ptr = Call::allocator->allocate_one();
    auto* slot = new(&*slot_ptr) typename Call::slot_t (argv...);
    auto built_t = Timings::clock::now();
    slot->timings = Timings(start_t);
    slot->timings.record("constructed", built_t);
    queue.put(slot->rec());
    return slot;
}

template <typename Call, typename... Args>
typename Call::slot_t* send_api_request(Args... argv)
{
    return send_request<Call, Args...>(scoreboard->api_queue, argv...);
}

template <typename Call, typename... Args>
typename Call::slot_t* send_fsm_request(Args... argv)
{
    return send_request<Call, Args...>(scoreboard->fsm_queue, argv...);
}


void track_orphan(BaseSlot* slot);

// Startup, shutdown, etc.

RaftConfig default_config();

RaftError parse_argv(int argc, char *argv[], RaftConfig &config);

/**
 * Set up shared memory and any resident resources.
 *
 * @param name currently ignored...
 * @param create whether to create a new shared memory region (for the client) 
 *               or map an existing one (for the Raft side).
 */
void shm_init(const char* name, bool create, const RaftConfig* cfg);

void shm_cleanup();

/**
 * Start the Raft process.
 *
 * To be called from the client after shm_init().
 */
pid_t run_raft();

}

#endif /* RAFT_SHM_H */
