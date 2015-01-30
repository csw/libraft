#include <cstdint>
#include <pthread.h>
#include <sys/types.h>

#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

#include "raft_c_if.h"
#include "raft_shm.h"
#include "stats.h"

using boost::interprocess::anonymous_instance;
using boost::interprocess::interprocess_mutex;

using namespace raft;

static void start_fsm_worker(RaftFSM* fsm);
static void run_fsm_worker(RaftFSM* fsm);

static void dispatch_fsm_apply(api::FSMApply::slot_t& slot);
static void dispatch_fsm_apply_cmd(api::FSMApply::slot_t& slot);
static void dispatch_fsm_snapshot(api::FSMSnapshot::slot_t& slot);
static void dispatch_fsm_restore(api::FSMRestore::slot_t& slot);

namespace {

struct SnapshotJob {
    api::FSMSnapshot::slot_t* slot;
    raft_fsm_snapshot_handle  handle;
    raft_fsm_snapshot_func    func;
};

std::deque<SnapshotJob> snapshot_jobs;
std::mutex              snapshot_mutex;
std::condition_variable snapshot_available;
std::thread             snapshot_worker;

void run_snapshot_worker();
void run_snapshot_job(const SnapshotJob& job);

}

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

void raft_default_config(RaftConfig *cfg)
{
    *cfg = raft::default_config();
}

RaftError raft_parse_argv(int argc, char *argv[], RaftConfig *cfg)
{
    if (!cfg || !argv)
        return RAFT_E_INVALID_ADDRESS;
    return raft::parse_argv(argc, argv, *cfg);
}

pid_t raft_init(RaftFSM *fsm_, const RaftConfig *config_arg)
{
    init_err_msgs();
    RaftConfig config;
    if (config_arg) {
        config = *config_arg;
    } else {
        config = raft::default_config();
    }
    raft::shm_init("raft", true);

    fsm = fsm_;
    raft::run_raft(config);
    zlog_info(shm_cat, "Started Raft process: pid %d.", raft_pid);
    start_fsm_worker(fsm);
    raft::scoreboard->wait_for_raft(raft_pid);
    zlog_debug(shm_cat, "Raft is running.");
    return raft_pid;
}

void raft_cleanup()
{
    zlog_debug(fsm_cat, "Canceling FSM worker thread.");
    if (fsm_worker.joinable()) {
        if (pthread_cancel(fsm_worker.native_handle())) {
            perror("Failed to cancel FSM worker thread");
        }
        fsm_worker.join();
    }
    // TODO: look into cancellation behavior further
    if (snapshot_worker.joinable()) {
        if (pthread_cancel(snapshot_worker.native_handle())) {
            perror("Failed to cancel snapshot worker thread");
        }
        snapshot_worker.join();
    }
    raft::shm_cleanup();
}

bool raft_is_leader()
{
    return raft::scoreboard->is_leader;
}

raft_future raft_apply_async(char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    return (raft_future)
        send_api_request<api::Apply>(cmd, cmd_len, timeout_ns);
}

RaftError raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns, void **res)
{
    raft_future f = raft_apply_async(cmd, cmd_len, timeout_ns);
    raft_future_wait(f);
    zlog_debug(msg_cat, "Result of call %p is ready.", f);
    return raft_future_get_ptr(f, res);
}

raft_future raft_barrier(uint64_t timeout_ns)
{
    return (raft_future) send_api_request<api::Barrier>(timeout_ns);
}

raft_future raft_verify_leader()
{
    return (raft_future) send_api_request<api::VerifyLeader>();
}

raft_future raft_snapshot()
{
    return (raft_future) send_api_request<api::Snapshot>();
}

raft_future raft_add_peer(const char *host, uint16_t port)
{
    return (raft_future) send_api_request<api::AddPeer>(host, port);
}

raft_future raft_remove_peer(const char *host, uint16_t port)
{
    return (raft_future) send_api_request<api::RemovePeer>(host, port);
}

raft_future raft_shutdown()
{
    raft::shutdown_requested = true;
    return (raft_future) send_api_request<api::Shutdown>();
}

RaftError raft_future_wait(raft_future f)
{
    auto* slot = (BaseSlot*) f;
    slot->wait();
    slot->timings.print();
    return slot->error;
}

RaftError raft_future_get_ptr(raft_future f, void** value_ptr)
{
    return ((BaseSlot*)f)->get_ptr(value_ptr);
}

void raft_future_dispose(raft_future f)
{
    auto* slot = ((BaseSlot*)f);
    slot->dispose();
}

void raft_fsm_snapshot_complete(raft_snapshot_req s, bool success)
{
    auto slot = (api::FSMSnapshot::slot_t*) s;
    {
        raft::mutex_lock l(slot->owned);
        slot->reply(success ? RAFT_SUCCESS : RAFT_E_OTHER);
    }

    if (success) {
        zlog_info(fsm_cat, "Snapshot succeeded.");
    } else {
        zlog_error(fsm_cat, "Snapshot failed!");
    }
}

void start_fsm_worker(RaftFSM* fsm)
{
    // check that there isn't already one started
    assert(fsm_worker.get_id() == std::thread::id());
    fsm_worker = std::thread(run_fsm_worker, fsm);
}

void run_fsm_worker(RaftFSM* fsm)
{
    zlog_debug(fsm_cat, "FSM worker starting.\n");
    
    for (;;) {
        auto rec = scoreboard->fsm_queue.take();
        CallTag tag = rec.first;
        BaseSlot::pointer slot = rec.second;
        raft::mutex_lock l(slot->owned);
        slot->timings.record("FSM call received");
        zlog_debug(msg_cat, "FSM call received, tag %d, call %p.",
                   tag, rec.second.get());
        assert(slot->state == raft::CallState::Pending);

        switch (tag) {
        case CallTag::FSMApply:
            dispatch_fsm_apply((api::FSMApply::slot_t&) *slot);
            break;
        case CallTag::FSMSnapshot: {
            l.unlock();
            dispatch_fsm_snapshot((api::FSMSnapshot::slot_t&) *slot);
        }
            break;
        case CallTag::FSMRestore:
            dispatch_fsm_restore((api::FSMRestore::slot_t&) *slot);
            break;
        default:
            zlog_fatal(msg_cat, "Unhandled call type: %d", tag);
            abort();
        }
    }
}

void dispatch_fsm_apply(api::FSMApply::slot_t& slot)
{
    const LogEntry& log = slot.args;

    switch (log.log_type) {
    case RAFT_LOG_COMMAND:
        dispatch_fsm_apply_cmd(slot);
        break;
    case RAFT_LOG_NOOP:
        zlog_info(msg_cat, "FSM command: noop");
        break;
    case RAFT_LOG_ADD_PEER:
        zlog_info(msg_cat, "FSM command: add peer");
        break;
    case RAFT_LOG_REMOVE_PEER:
        zlog_info(msg_cat, "FSM command: remove peer");
        break;
    case RAFT_LOG_BARRIER:
        zlog_info(msg_cat, "FSM command: barrier");
        break;
    }
}

void dispatch_fsm_apply_cmd(api::FSMApply::slot_t& slot)
{
    const LogEntry& log = slot.args;
    assert(log.data_buf);
    char* data_buf = (char*) raft::shm.get_address_from_handle(log.data_buf);
    zlog_debug(fsm_cat, "Found command buffer at %p.", data_buf);
    slot.state = CallState::Dispatched;
    slot.timings.record("FSM call dispatched");
    void* response =
        fsm->apply(log.index, log.term, log.log_type, data_buf, log.data_len);
    slot.timings.record("FSM command applied");
    zlog_debug(fsm_cat, "FSM response @ %p", response);
    slot.reply((uintptr_t) response);
}

void dispatch_fsm_snapshot(api::FSMSnapshot::slot_t& slot)
{
    assert(strlen(slot.args.path) > 0);
    fsm->begin_snapshot(slot.args.path, &slot);
}

void dispatch_fsm_restore(api::FSMRestore::slot_t& slot)
{
    assert(strlen(slot.args.path) > 0);
    zlog_info(fsm_cat, "Passing restore request to FSM, path %s.",
              slot.args.path);
    int result = fsm->restore(slot.args.path);
    slot.reply(result == 0 ? RAFT_SUCCESS : RAFT_E_OTHER);
}

void raft_fsm_take_snapshot(raft_snapshot_req req,
                            raft_fsm_snapshot_handle h,
                            raft_fsm_snapshot_func f)
{
    assert(req);
    assert(h);
    assert(f);

    auto* slot = (api::FSMSnapshot::slot_t*) req;
    std::unique_lock<decltype(snapshot_mutex)> lock(snapshot_mutex);
    if (! snapshot_worker.joinable()) {
        snapshot_worker = std::thread(run_snapshot_worker);
    }
    snapshot_jobs.push_back(SnapshotJob { slot, h, f });
    snapshot_available.notify_one();
}

namespace {

void run_snapshot_worker()
{
    for (;;) {
        std::unique_lock<decltype(snapshot_mutex)> lock(snapshot_mutex);
        while (snapshot_jobs.empty())
            snapshot_available.wait(lock);
        const SnapshotJob job = snapshot_jobs.front();
        snapshot_jobs.pop_front();
        lock.unlock();
        run_snapshot_job(job);
    }
}

void run_snapshot_job(const SnapshotJob& job)
{
    bool success = false;

    zlog_info(fsm_cat, "Writing snapshot to %s.", job.slot->args.path);
    FILE *sink = fopen(job.slot->args.path, "w");
    if (sink) {
        int rc = (*job.func)(job.handle, sink);
        zlog_debug(fsm_cat, "Client snapshot function returned %d.", rc);
        success = (rc == 0);

        if (fclose(sink) != 0) {
            zlog_error(fsm_cat, "Closing snapshot pipe failed: %s",
                       strerror(errno));
            success = false;
        }
    } else {
        zlog_error(fsm_cat, "Opening snapshot pipe failed: %s",
                   strerror(errno));
    }

    raft_fsm_snapshot_complete(job.slot, success);
}

}

char* alloc_raft_buffer(size_t len)
{
    stats->buffer_alloc.inc();
    return (char*) shm.allocate(len);
}

void free_raft_buffer(const char* buf)
{
    stats->buffer_free.inc();
    raft::shm.deallocate((void*) buf);
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
