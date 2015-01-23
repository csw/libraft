#include <cstdint>
#include <sys/types.h>

#include <thread>

#include "raft_c_if.h"
#include "raft_shm.h"

using boost::interprocess::anonymous_instance;
using boost::interprocess::interprocess_mutex;

bool raft_is_leader()
{
    return raft::scoreboard->is_leader;
}

void* raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    raft::SlotHandle sh(*raft::scoreboard);
    std::unique_lock<interprocess_mutex> l(sh.slot.owned);
    sh.slot.call_type = raft::CallType::Apply;
    sh.slot.state = raft::CallState::Pending;

    // manual memory management for now...
    raft::ApplyCall& call =
        *raft::shm.construct<raft::ApplyCall>(anonymous_instance)();
    fprintf(stderr, "Allocated call buffer at %p.\n", &call);

    call.cmd_buf = raft::shm.get_handle_from_address(cmd);
    call.cmd_len = cmd_len;
    call.timeout_ns = timeout_ns;

    sh.slot.handle = raft::shm.get_handle_from_address(&call);
    sh.slot.call_ready = true;
    sh.slot.call_cond.notify_one();
    sh.slot.ret_cond.wait(l, [&] () { return sh.slot.ret_ready; });
    assert(sh.slot.state == raft::CallState::Success
           || sh.slot.state == raft::CallState::Error);

    sh.slot.ret_ready = false;

    raft::shm.destroy_ptr(&call);
    return nullptr;
}

pid_t raft_init(RaftFSM *fsm)
{
    (void) fsm;
    raft::shm_init("raft", true);
    pid_t raft_pid = raft::run_raft();
    fprintf(stderr, "Started Raft process: pid %d.\n", raft_pid);
    raft::scoreboard->wait_for_raft(raft_pid);
    fprintf(stderr, "Raft is running.\n");
    return raft_pid;
}

void* alloc_raft_buffer(size_t len)
{
    return raft::shm.allocate(len);
}

void free_raft_buffer(void* buf)
{
    raft::shm.deallocate(buf);
}
