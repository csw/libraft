#include <cstdint>
#include <sys/types.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "raft_c_if.h"
#include "raft_shm.h"

using boost::interprocess::anonymous_instance;
using boost::interprocess::interprocess_mutex;

using namespace raft;

static void start_fsm_worker(RaftFSM* fsm);
static void run_fsm_worker(RaftFSM* fsm);

static void dispatch_fsm_apply(CallSlot<LogEntry, true>& slot);
static void dispatch_fsm_apply_cmd(CallSlot<LogEntry, true>& slot);
static void dispatch_fsm_snapshot(CallSlot<Filename, false>& slot);
static void dispatch_fsm_restore(CallSlot<Filename, false>& slot);

static void init_err_msgs();
static RaftFSM*    fsm;
static std::thread fsm_worker;

namespace alloc {

//pool_allocator< CallSlot<ApplyArgs, true> > apply;

}

static std::vector<const char*> err_msgs;

const char* raft_err_msg(RaftError err)
{
    if (err < err_msgs.size()) {
        return err_msgs.at(err);
    } else {
        return "Unknown error";
    }
}

pid_t raft_init(RaftFSM *fsm_, int argc, char *argv[])
{
    init_err_msgs();
    raft::process_args(argc, argv);
    raft::shm_init("raft", true);

    fsm = fsm_;
    raft::run_raft();
    fprintf(stderr, "Started Raft process: pid %d.\n", raft_pid);
    raft::scoreboard->wait_for_raft(raft_pid);
    fprintf(stderr, "Raft is running.\n");
    start_fsm_worker(fsm);
    return raft_pid;
}

bool raft_is_leader()
{
    return raft::scoreboard->is_leader;
}

raft_future raft_apply_async(char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    auto start_t = Timings::clock::now();
    auto* slot = shm.construct< CallSlot<ApplyArgs, true> >(anonymous_instance)
        (CallTag::Apply, cmd, cmd_len, timeout_ns);
    fprintf(stderr, "Allocated call slot at %p.\n", slot);
    slot->timings = Timings(start_t);
    slot->timings.record("constructed");
    scoreboard->api_queue.put(slot->rec());
    
    return (raft_future) slot;
}

RaftError raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns, void **res)
{
    raft_future f = raft_apply_async(cmd, cmd_len, timeout_ns);
    raft_future_wait(f);
    fprintf(stderr, "Result of call %p is ready.\n", f);
    return raft_future_get_ptr(f, res);
}

raft_future raft_snapshot()
{
    auto* slot = shm.construct< CallSlot<NoArgs, false> >(anonymous_instance)
        (CallTag::Snapshot);
    scoreboard->api_queue.put(slot->rec());
    return (raft_future) slot;
}

raft_future raft_add_peer(const char *host, uint16_t port)
{
    auto* slot = shm.construct< CallSlot<NetworkAddr, false> >(anonymous_instance)
        (CallTag::AddPeer, host, port);
    scoreboard->api_queue.put(slot->rec());
    return (raft_future) slot;
}

raft_future raft_remove_peer(const char *host, uint16_t port)
{
    auto* slot = shm.construct< CallSlot<NetworkAddr, false> >(anonymous_instance)
        (CallTag::RemovePeer, host, port);
    scoreboard->api_queue.put(slot->rec());
    return (raft_future) slot;
}

raft_future raft_shutdown()
{
    auto* slot = shm.construct< CallSlot<NoArgs, false> >(anonymous_instance)
        (CallTag::Shutdown);
    scoreboard->api_queue.put(slot->rec());
    return (raft_future) slot;
}

RaftError raft_future_wait(raft_future f)
{
    auto* slot = (BaseSlot*) f;
    slot->wait();
    slot->timings.record("result received");
    slot->timings.print();
    return RAFT_SUCCESS;
}

RaftError raft_future_get_ptr(raft_future f, void** value_ptr)
{
    return ((BaseSlot*)f)->get_ptr(value_ptr);
}

void raft_future_dispose(raft_future f)
{
    ((BaseSlot*)f)->dispose();
}

void raft_fsm_snapshot_complete(raft_snapshot_req s, bool success)
{
    auto slot = (CallSlot<Filename, false>*) s;
    raft::mutex_lock l(slot->owned);
    slot->reply(success ? RAFT_SUCCESS : RAFT_E_OTHER);
}

void start_fsm_worker(RaftFSM* fsm)
{
    // check that there isn't already one started
    assert(fsm_worker.get_id() == std::thread::id());
    fsm_worker = std::thread(run_fsm_worker, fsm);
}

void run_fsm_worker(RaftFSM* fsm)
{
    fprintf(stderr, "FSM worker starting.\n");
    
    for (;;) {
        auto rec = scoreboard->fsm_queue.take();
        CallTag tag = rec.first;
        BaseSlot::pointer slot = rec.second;
        raft::mutex_lock l(slot->owned);
        slot->timings.record("FSM call received");
        fprintf(stderr, "FSM call received, tag %d, call %p.\n",
                tag, rec.second.get());
        assert(slot->state == raft::CallState::Pending);

        switch (tag) {
        case CallTag::FSMApply:
            dispatch_fsm_apply((CallSlot<LogEntry, true>&) *slot);
            break;
        case CallTag::FSMSnapshot: {
            l.unlock();
            dispatch_fsm_snapshot((CallSlot<Filename, false>&) *slot);
        }
            break;
        case CallTag::FSMRestore:
            dispatch_fsm_restore((CallSlot<Filename, false>&) *slot);
            break;
        default:
            fprintf(stderr, "Unhandled call type: %d\n",
                    tag);
            abort();
        }
    }
}

void dispatch_fsm_apply(CallSlot<LogEntry, true>& slot)
{
    LogEntry& log = slot.args;

    switch (log.log_type) {
    case RAFT_LOG_COMMAND:
        dispatch_fsm_apply_cmd(slot);
        break;
    case RAFT_LOG_NOOP:
        fprintf(stderr, "FSM command: noop\n");
        break;
    case RAFT_LOG_ADD_PEER:
        fprintf(stderr, "FSM command: add peer\n");
        break;
    case RAFT_LOG_REMOVE_PEER:
        fprintf(stderr, "FSM command: remove peer\n");
        break;
    case RAFT_LOG_BARRIER:
        fprintf(stderr, "FSM command: barrier\n");
        break;
    }
}

void dispatch_fsm_apply_cmd(CallSlot<LogEntry, true>& slot)
{
    LogEntry& log = slot.args;
    assert(log.data_buf);
    char* data_buf = (char*) raft::shm.get_address_from_handle(log.data_buf);
    fprintf(stderr, "Found command buffer at %p.\n", data_buf);
    slot.state = CallState::Dispatched;
    slot.timings.record("FSM call dispatched");
    void* response =
        fsm->apply(log.index, log.term, log.log_type, data_buf, log.data_len);
    slot.timings.record("FSM command applied");
    fprintf(stderr, "FSM response @ %p\n", response);
    slot.reply((uintptr_t) response);
}

void dispatch_fsm_snapshot(CallSlot<Filename, false>& slot)
{
    assert(strlen(slot.args.path) > 0);
    fsm->begin_snapshot(slot.args.path, &slot);
}

void dispatch_fsm_restore(CallSlot<Filename, false>& slot)
{
    assert(strlen(slot.args.path) > 0);
    int result = fsm->restore(slot.args.path);
    slot.reply(result == 0 ? RAFT_SUCCESS : RAFT_E_OTHER);
}

char* alloc_raft_buffer(size_t len)
{
    return (char*) shm.allocate(len);
}

void free_raft_buffer(char* buf)
{
    raft::shm.deallocate(buf);
}

void init_err_msgs()
{
    // TODO: dump these from the Raft code
    err_msgs = decltype(err_msgs)(N_RAFT_ERRORS);
    err_msgs[RAFT_SUCCESS] = "success";
    err_msgs[RAFT_E_LEADER] = "node is the leader";
    err_msgs[RAFT_E_NOT_LEADER] = "node is not the leader";
    err_msgs[RAFT_E_LEADERSHIP_LOST] = "leadership lost while committing log";
    err_msgs[RAFT_E_SHUTDOWN] = "raft is already shutdown";
    err_msgs[RAFT_E_ENQUEUE_TIMEOUT] = "timed out enqueuing operation";
    err_msgs[RAFT_E_KNOWN_PEER] = "peer already known";
    err_msgs[RAFT_E_UNKNOWN_PEER] = "peer is unknown";
    err_msgs[RAFT_E_LOG_NOT_FOUND] = "log not found";
    err_msgs[RAFT_E_PIPELINE_REPLICATION_NOT_SUPP] = "pipeline replication not supported";
    err_msgs[RAFT_E_TRANSPORT_SHUTDOWN] = "transport shutdown";
    err_msgs[RAFT_E_PIPELINE_SHUTDOWN] = "append pipeline closed";
    err_msgs[RAFT_E_OTHER] = "undetermined error";
    err_msgs[RAFT_E_INVALID_OP] = "invalid operation for call";
    err_msgs[RAFT_E_INVALID_ADDRESS] = "invalid address";
}
