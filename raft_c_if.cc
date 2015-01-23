#include <cstdint>
#include <sys/types.h>

#include <thread>

#include "raft_c_if.h"
#include "raft_shm.h"

using boost::interprocess::anonymous_instance;
using boost::interprocess::interprocess_mutex;

void start_fsm_worker(RaftFSM* fsm);
void run_fsm_worker(RaftFSM* fsm);

void dispatch_fsm_apply(raft::CallSlot<raft::FSMOp>& slot, raft::LogEntry& log);
void dispatch_fsm_apply_cmd(raft::CallSlot<raft::FSMOp>& slot, raft::LogEntry& log);

static RaftFSM*    fsm;
static std::thread fsm_worker;

bool raft_is_leader()
{
    return raft::scoreboard->is_leader;
}

void* raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    raft::SlotHandle<raft::APICall> sh(raft::scoreboard->grab_slot(),
                                       std::adopt_lock);
    raft::RaftCallSlot& slot = sh.slot;
    std::unique_lock<interprocess_mutex> l(slot.owned);
    slot.call_type = raft::APICall::Apply;
    slot.state = raft::CallState::Pending;

    // manual memory management for now...
    raft::ApplyCall& call =
        *raft::shm.construct<raft::ApplyCall>(anonymous_instance)();
    fprintf(stderr, "Allocated call buffer at %p.\n", &call);

    call.cmd_buf = raft::shm.get_handle_from_address(cmd);
    call.cmd_len = cmd_len;
    call.timeout_ns = timeout_ns;

    slot.handle = raft::shm.get_handle_from_address(&call);
    slot.call_ready = true;
    slot.call_cond.notify_one();
    slot.ret_cond.wait(l, [&] () { return slot.ret_ready; });
    assert(slot.state == raft::CallState::Success
           || slot.state == raft::CallState::Error);

    slot.ret_ready = false;
    raft::shm.destroy_ptr(&call);

    return (void*) slot.retval;
}

pid_t raft_init(RaftFSM *fsm_)
{
    raft::shm_init("raft", true);
    fsm = fsm_;
    pid_t raft_pid = raft::run_raft();
    fprintf(stderr, "Started Raft process: pid %d.\n", raft_pid);
    raft::scoreboard->wait_for_raft(raft_pid);
    fprintf(stderr, "Raft is running.\n");
    start_fsm_worker(fsm);
    return raft_pid;
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
    raft::CallSlot<raft::FSMOp>& slot = raft::scoreboard->fsm_slot;

    for (;;) {
        // refactor, combine this with the Go side...
        raft::mutex_lock l(slot.owned);
        slot.call_cond.wait(l, [&] () { return slot.call_ready; });
        assert(slot.state == raft::CallState::Pending);

        assert(slot.handle);
        void* call_addr = raft::shm.get_address_from_handle(slot.handle);
        fprintf(stderr, "Found FSM op buffer at %p.\n", call_addr);

        switch (slot.call_type) {
        case raft::FSMOp::Apply:
            dispatch_fsm_apply(slot, *(raft::LogEntry*)call_addr);
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
