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

static void dispatch_fsm_apply(raft::FSMCallSlot& slot, raft::LogEntry& log);
static void dispatch_fsm_apply_cmd(raft::FSMCallSlot& slot, raft::LogEntry& log);

static void init_err_msgs();

static RaftFSM*    fsm;
static std::thread fsm_worker;

static std::vector<const char*> err_msgs;

const char* raft_err_msg(RaftError err)
{
    if (err < err_msgs.size()) {
        return err_msgs.at(err);
    } else {
        return "Unknown error";
    }
}

pid_t raft_init(RaftFSM *fsm_)
{
    init_err_msgs();
    raft::shm_init("raft", true);

    fsm = fsm_;
    pid_t raft_pid = raft::run_raft();
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

RaftError raft_apply(void **res, char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    auto start_t = Timings::clock::now();
    raft::SlotHandle<raft::APICall> sh(raft::scoreboard->grab_slot(),
                                       std::adopt_lock);
    raft::RaftCallSlot& slot = sh.slot;
    slot.timings = Timings(start_t);
    std::unique_lock<interprocess_mutex> l(slot.owned);
    slot.timings.record("slot owned");
    slot.call_type = raft::APICall::Apply;
    slot.state = raft::CallState::Pending;

    ApplyCall& call = slot.call.apply;
    call.cmd_buf = raft::shm.get_handle_from_address(cmd);
    call.cmd_len = cmd_len;
    call.timeout_ns = timeout_ns;
    call.dispatch_ns = 0;

    slot.call_ready = true;
    slot.call_cond.notify_one();
    slot.timings.record("call issued");
    slot.ret_cond.wait(l, [&] () { return slot.ret_ready; });
    slot.timings.record("call returned");

    if (slot.state == raft::CallState::Success) {
        assert(slot.error == RAFT_SUCCESS);
        if (res)
            *res = (void*) slot.retval;

    } else if (slot.state == raft::CallState::Error) {
        assert(slot.error != RAFT_SUCCESS);
        if (res)
            *res = nullptr;

    } else {
        fprintf(stderr, "Unexpected call state %d!\n", slot.state);
        abort();
    }

    slot.ret_ready = false;
    slot.timings.record("call completed");
    slot.timings.print();

    return slot.error;
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
    raft::FSMCallSlot& slot = raft::scoreboard->fsm_slot;

    for (;;) {
        // refactor, combine this with the Go side...
        raft::mutex_lock l(slot.owned);
        slot.call_cond.wait(l, [&] () { return slot.call_ready; });
        assert(slot.state == raft::CallState::Pending);

        switch (slot.call_type) {
        case raft::FSMOp::Apply:
            dispatch_fsm_apply(slot, slot.call.log_entry);
            break;
        default:
            fprintf(stderr, "Unhandled log entry type: %d\n",
                    slot.call_type);
            abort();
        }

        slot.state = raft::CallState::Success;
        slot.ret_ready = true;
        slot.ret_cond.notify_one();
        slot.call_ready = false;
        
    }
}

void dispatch_fsm_apply(raft::CallSlot<raft::FSMOp>& slot, raft::LogEntry& log)
{
    switch (log.log_type) {
    case RAFT_LOG_COMMAND:
        dispatch_fsm_apply_cmd(slot, log);
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

void dispatch_fsm_apply_cmd(raft::CallSlot<raft::FSMOp>& slot, raft::LogEntry& log)
{
    assert(log.data_buf);
    void* data_buf = raft::shm.get_address_from_handle(log.data_buf);
    fprintf(stderr, "Found command buffer at %p.\n", data_buf);
    slot.state = raft::CallState::Dispatched;
    void* response =
        fsm->apply(log.index, log.term, log.log_type, data_buf, log.data_len);
    slot.retval = (uintptr_t) response;
}

void* alloc_raft_buffer(size_t len)
{
    return raft::shm.allocate(len);
}

void free_raft_buffer(void* buf)
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
}
