/*
 * libraft, C interface to Hashicorp's Raft implementation.
 * Copyright (C) 2015 Clayton Wheeler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <cstdint>
#include <sys/types.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "raft_if.h"
#include "raft_shm.h"
#include "stats.h"
#include "zserge-jsmn/jsmn.h"

using boost::interprocess::anonymous_instance;
using boost::interprocess::interprocess_mutex;

using namespace raft;

static void start_fsm_worker(RaftFSM* fsm);
static void run_fsm_worker(void* fsm);
static void fsm_worker_step(RaftFSM* fsm);

static void dispatch_fsm_apply(api::FSMApply::slot_t& slot, RaftFSM* fsm);
static void dispatch_fsm_apply_cmd(api::FSMApply::slot_t& slot, RaftFSM* fsm);
static void dispatch_fsm_snapshot(api::FSMSnapshot::slot_t& slot, RaftFSM* fsm);
static void dispatch_fsm_restore(api::FSMRestore::slot_t& slot, RaftFSM* fsm);

namespace {

struct SnapshotJob {
    api::FSMSnapshot::slot_t* slot;
    raft_fsm_snapshot_handle  handle;
    raft_fsm_snapshot_func    func;
};

queue::Deque<SnapshotJob> snapshot_queue;
std::thread               snapshot_worker;

void run_snapshot_worker();
void run_snapshot_job(const SnapshotJob& job);

CallTag   future_tag(const raft_future f);
CallState future_state(const raft_future f);
RaftError future_get(raft_future f, uint64_t* res);
// must be terminal already
uint64_t  future_get_f(raft_future f);
// as above, but calls dispose()
RaftError future_eat(raft_future f, uint64_t* res);

}

static void init_err_msgs();
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

const struct option* raft_getopt_long_opts()
{
    return raft::arg::LONG_OPTS;
}

bool is_raft_option(int optkey)
{
    return raft::arg::is_valid(optkey);
}

int raft_apply_option(RaftConfig *cfg, int option, const char* arg)
{
    if (!cfg) {
        zlog_error(client_cat, "Config pointer is null!");
        return 1;
    }
    if (! raft::arg::is_valid(option)) {
        zlog_error(client_cat, "Invalid option flag: %d", option);
        return 1;
    }

    return raft::arg::apply(*cfg, (arg::Getopt) option, arg);
}

pid_t raft_init(RaftFSM *fsm, const RaftConfig *config_arg)
{
    snapshot_queue.reset();

    init_err_msgs();
    RaftConfig config;
    if (config_arg) {
        config = *config_arg;
    } else {
        config = raft::default_config();
    }
    raft::shm_init(config.shm_path, true, &config);

    raft::run_raft();
    zlog_info(shm_cat, "Started Raft process: pid %d.", raft_pid);
    start_fsm_worker(fsm);
    raft::scoreboard->wait_for_raft(raft_pid);
    zlog_debug(shm_cat, "Raft is running.");
    return raft_pid;
}

void raft_cleanup()
{
    zlog_debug(fsm_cat, "Closing FSM queue.");
    raft::scoreboard->fsm_queue.close();
    assert(fsm_worker.joinable());
    fsm_worker.join();
    zlog_debug(fsm_cat, "FSM worker finished.");

    zlog_debug(fsm_cat, "Closing snapshot queue.");
    snapshot_queue.close();
    if (snapshot_worker.joinable()) {
        snapshot_worker.join();
        zlog_debug(fsm_cat, "Snapshot worker finished.");
    }
    raft::shm_cleanup();
}

bool raft_is_leader()
{
    return raft::scoreboard->is_leader;
}

raft_future raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns)
{
    if (cmd && in_shm_bounds((void*) cmd)
        && in_shm_bounds((void*) (cmd+cmd_len))) {
        return (raft_future)
            send_api_request<api::Apply>(cmd, cmd_len, timeout_ns);
    } else {
        return make_error_request<api::Apply>(RAFT_E_INVALID_ADDRESS,
                                              cmd, cmd_len, timeout_ns);
    }
}

RaftError raft_apply_sync(char* cmd, size_t cmd_len, uint64_t timeout_ns, void **res)
{
    if (res == nullptr)
        return RAFT_E_INVALID_ADDRESS;
    raft_future f = raft_apply(cmd, cmd_len, timeout_ns);
    raft_future_wait(f);
    zlog_debug(msg_cat, "Result of call %p is ready.", f);
    RaftError err = raft_future_get_fsm_reply(f, res);
    raft_future_dispose(f);
    return err;
}

raft_future raft_barrier(uint64_t timeout_ns)
{
    return (raft_future) send_api_request<api::Barrier>(timeout_ns);
}

raft_future raft_verify_leader()
{
    return (raft_future) send_api_request<api::VerifyLeader>();
}

RaftState raft_state()
{
    raft_future f = send_api_request<api::GetState>();
    RaftError err = raft_future_wait(f);
    RaftState state;
    if (!err) {
        state = (RaftState) future_get_f(f);
    } else {
        state = RAFT_INVALID_STATE;
    }
    raft_future_dispose(f);
    return state;
}

time_t raft_last_contact()
{
    raft_future f = send_api_request<api::GetState>();
    RaftError err = raft_future_wait(f);
    time_t last_contact;
    if (!err) {
        last_contact = (time_t) future_get_f(f);
    } else {
        last_contact = 0;
    }
    raft_future_dispose(f);
    return last_contact;
}

RaftIndex raft_last_index()
{
    raft_future f = send_api_request<api::LastIndex>();
    uint64_t idx;
    RaftError err = future_eat(f, &idx);
    if (!err) {
        return idx;
    } else {
        zlog_error(msg_cat, "LastIndex call failed: %s",
                   raft_err_msg(err));
        return 0;
    }
}

RaftError raft_leader(char** res)
{
    if (res == nullptr)
        return RAFT_E_INVALID_ADDRESS;

    raft_future f = send_api_request<api::GetLeader>();
    uint64_t pval;
    RaftError err = future_eat(f, &pval);
    if (!err) {
        assert(pval);
        *res = (char*) raft::shm.get_address_from_handle((shm_handle) pval);
    }        
    return err;
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

raft_future raft_stats()
{
    return (raft_future) send_api_request<api::Stats>();
}

raft_future raft_shutdown()
{
    raft::scoreboard->shutdown_requested = true;
    return (raft_future) send_api_request<api::Shutdown>();
}

RaftError raft_future_wait(raft_future f)
{
    if (f == nullptr || ! raft::in_shm_bounds(f))
        return RAFT_E_INVALID_OP;

    auto* slot = (BaseSlot*) f;
    slot->wait();
    slot->timings.print();
    return slot->error;
}

bool raft_future_poll(raft_future f)
{
    return ((BaseSlot*) f)->poll();
}

bool raft_future_wait_for(raft_future f, uint64_t wait_ms)
{
    return ((BaseSlot*) f)->wait_for(std::chrono::milliseconds(wait_ms));
}

RaftError raft_future_get_fsm_reply(raft_future f, void** value_ptr)
{
    if (future_tag(f) != raft::api::Apply::tag)
        return RAFT_E_INVALID_OP;
    return ((BaseSlot*)f)->get_ptr(value_ptr);
}

RaftError raft_future_get_stats(raft_future f, const char** res)
{
    if (future_tag(f) != raft::api::Stats::tag)
        return RAFT_E_INVALID_OP;
    if (!res)
        return RAFT_E_INVALID_ADDRESS;

    uint64_t handle;
    RaftError err = future_get(f, &handle);
    if (!err) {
        assert(handle);
        *res = (char*) raft::shm.get_address_from_handle((shm_handle) handle);
    }
    return err;
}

namespace {

CallTag future_tag(const raft_future f)
{
    assert(f);
    return ((BaseSlot*)f)->tag;
}

CallState future_state(const raft_future f)
{
    assert(f);
    // XXX: use an atomic
    return ((BaseSlot*)f)->state;
}

RaftError future_get(raft_future f, uint64_t* res)
{
    if (!f)
        return RAFT_E_INVALID_OP;
    if (!res)
        return RAFT_E_INVALID_ADDRESS;

    RaftError err = RAFT_SUCCESS;
    if (! is_terminal(future_state(f)))
        err = raft_future_wait(f);

    if (!err) {
        *res = ((BaseSlot*)f)->value();
    }
    return err;
}

uint64_t future_get_f(raft_future f)
{
    assert(f);
    assert(is_terminal(future_state(f)));
    return ((BaseSlot*)f)->value();
}

RaftError future_eat(raft_future f, uint64_t* res)
{
    RaftError err = future_get(f, res);
    raft_future_dispose(f);
    return err;
}

}

void raft_future_dispose(raft_future f)
{
    auto* slot = ((BaseSlot*)f);
    slot->dispose();
}

// callback for FSM or snapshot thread
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

static void run_fsm_worker(void* fsm_p)
{
    RaftFSM* fsm = (RaftFSM*) fsm_p;
    zlog_debug(fsm_cat, "FSM worker starting.");
    
    try {
        for (;;) {
            fsm_worker_step(fsm);
        }
    } catch (queue::queue_closed& e) {
        zlog_debug(fsm_cat, "FSM worker exiting: queue closed.");
    }
}

static void fsm_worker_step(RaftFSM* fsm)
{
    auto rec = scoreboard->fsm_queue.take();
    CallTag tag = rec.first;
    BaseSlot::pointer slot = rec.second;
    raft::mutex_lock l(slot->owned);
    slot->timings.record("FSM call received");
    zlog_debug(msg_cat, "FSM call received, tag %s, call %p.",
               tag_name(tag), rec.second.get());
    assert(slot->state == raft::CallState::Pending);

    switch (tag) {
    case CallTag::FSMApply:
        dispatch_fsm_apply((api::FSMApply::slot_t&) *slot, fsm);
        break;
    case CallTag::FSMSnapshot: {
        l.unlock();
        dispatch_fsm_snapshot((api::FSMSnapshot::slot_t&) *slot, fsm);
    }
        break;
    case CallTag::FSMRestore:
        dispatch_fsm_restore((api::FSMRestore::slot_t&) *slot, fsm);
        break;
    default:
        zlog_fatal(msg_cat, "Unhandled call type: %s", tag_name(tag));
        abort();
    }
}

void dispatch_fsm_apply(api::FSMApply::slot_t& slot, RaftFSM* fsm)
{
    const LogEntry& log = slot.args;

    switch (log.log_type) {
    case RAFT_LOG_COMMAND:
        dispatch_fsm_apply_cmd(slot, fsm);
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

void dispatch_fsm_apply_cmd(api::FSMApply::slot_t& slot, RaftFSM* fsm)
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

void dispatch_fsm_snapshot(api::FSMSnapshot::slot_t& slot, RaftFSM* fsm)
{
    assert(strlen(slot.args.path) > 0);
    fsm->begin_snapshot(slot.args.path, &slot);
}

void dispatch_fsm_restore(api::FSMRestore::slot_t& slot, RaftFSM* fsm)
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
    if (! snapshot_worker.joinable()) {
        snapshot_worker = std::thread(run_snapshot_worker);
    }
    snapshot_queue.emplace(SnapshotJob { slot, h, f });
}

namespace {

void run_snapshot_worker()
{
    try {
        for (;;) {
            const SnapshotJob job = snapshot_queue.take();
            run_snapshot_job(job);
        }
    } catch (queue::queue_closed&) {
        zlog_debug(fsm_cat, "Snapshot queue closed, worker exiting.");
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

void raft_print_stats(const char *stats)
{
    if (!stats) {
        zlog_error(client_cat, "Invalid stats pointer %p", stats);
        return;
    }

    jsmn_parser p;
    jsmntok_t tokens[64];
    jsmn_init(&p);
    int parsed;
    parsed = jsmn_parse(&p, stats, strlen(stats),
                        tokens, sizeof(tokens)/sizeof(tokens[0]));
    if (parsed < 1) {
        zlog_error(client_cat, "JSON parsing error: type %d", parsed);
        return;
    } else if (tokens[0].type != JSMN_OBJECT) {
        zlog_error(client_cat, "Invalid stats data");
        return;
    }

    fprintf(stderr, "Raft stats:\n");

    const int key_start = 1;
    for (int key = 0; key < tokens[0].size; ++key) {
        int key_i = key_start + 2*key;
        const jsmntok_t& key_t = tokens[key_i];
        const jsmntok_t& val_t = tokens[key_i+1];
        if (key_t.type != JSMN_STRING || val_t.type != JSMN_STRING) {
            zlog_error(client_cat, "Unexpected JSON content near character %d:\n%s",
                       key_t.start, stats);
            return;
        }
        const std::string key_s(&stats[key_t.start], key_t.end - key_t.start);
        const std::string val_s(&stats[val_t.start], val_t.end - val_t.start);
        fprintf(stderr, "    %20s: %s\n", key_s.c_str(), val_s.c_str());
    }
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
