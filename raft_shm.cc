#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include <list>
#include <string>

#include "raft_shm.h"
#include "stats.h"

namespace raft {

using boost::interprocess::unique_instance;

namespace api {

// required to avoid stupid link errors...

#define api_call(name, argT, hasRet) \
    const CallTag       name::tag; \
    name::allocator_t*  name::allocator;
#include "raft_api_calls.h"
#include "raft_fsm_calls.h"
#undef api_call

}

namespace {

const struct option LONG_OPTS[] = {
    { "shm-path", required_argument, NULL, 'm' },
    { "shm-size", required_argument, NULL, 'M' },
    { "dir",      required_argument, NULL, 'd' },
    { "p",        required_argument, NULL, 'p' },
    { "port",     required_argument, NULL, 'p' },
    { "single",   no_argument,       NULL, 's' },
    { "peers",    required_argument, NULL, 'P' },
    { "",         0,                 NULL, 0   }
};

std::list<BaseSlot*> orphan_backlog;
queue::Deque<BaseSlot*> orphaned_calls;

std::thread raft_watcher;
std::thread orphan_gc_thread;

uint64_t parse_size(const char *spec);
std::vector<const char*> build_raft_argv(const RaftConfig& cfg);
void watch_raft_proc(pid_t raft_pid);
void run_orphan_gc();
void scan_orphans(std::list<BaseSlot*>& calls);
bool try_dispose_orphan(BaseSlot* orphan);
void report_process_status(const char *desc, pid_t pid, int status);
void init_client_allocators();
void init_raft_allocators();

}

zlog_category_t*    msg_cat;
zlog_category_t*    fsm_cat;
zlog_category_t*    shm_cat;
bool                msg_timing = false;

pid_t               raft_pid;
managed_mapped_file shm;
Scoreboard*         scoreboard;

bool is_terminal(CallState state)
{
    switch (state) {
    case CallState::Pending:
    case CallState::Dispatched:
        return false;
    case CallState::Success:
    case CallState::Error:
        return true;
    default: // make poor old gcc happy
        assert(false && "unexpected call state!");
    }
}

ApplyArgs::ApplyArgs(offset_ptr<char> cmd_buf_, size_t cmd_len_, uint64_t timeout_ns_)
    : cmd_buf(cmd_buf_),
      cmd_len(cmd_len_),
      timeout_ns(timeout_ns_)
{}

BarrierArgs::BarrierArgs(uint64_t timeout_ns_)
    : timeout_ns(timeout_ns_)
{}

LogEntry::LogEntry(uint64_t index_, uint64_t term_, raft_log_type log_type_,
                   shm_handle data_buf_, size_t data_len_)
    : index(index_),
      term(term_),
      log_type(log_type_),
      data_buf(data_buf_),
      data_len(data_len_)
{}

Filename::Filename(const char* path_)
{
    strncpy(path, path_, MAXLEN);
}

NetworkAddr::NetworkAddr(const char* host_, uint16_t port_)
    : port(port_)
{
    strncpy(host, host_, 255);
}

Scoreboard::Scoreboard()
    : is_leader(false),
      shutdown_requested(false),
      raft_killed(false)
{}

void Scoreboard::wait_for_raft(pid_t raft_pid)
{
    while (! is_raft_running) {
        // if the process exits, the raft_watcher thread will catch it
        usleep(100000); // 100 ms
    }
}

bool in_shm_bounds(const void* ptr)
{
    char* base = (char*) shm.get_address();
    char* cptr = (char*) ptr;
    return (cptr >= base) && (cptr < base + shm.get_size());
}

void track_orphan(BaseSlot* slot)
{
    zlog_debug(msg_cat, "Enqueueing orphaned call: %p", slot);
    orphaned_calls.put(slot);
}

namespace {

const static auto orphan_interval = std::chrono::milliseconds(100);

void run_orphan_gc()
{
    try {
        zlog_debug(msg_cat, "Orphan GC thread started.");
        orphan_backlog = decltype(orphan_backlog)();
        auto next_check = std::chrono::system_clock::now() + orphan_interval;
        
        for (;;) {
            BaseSlot* orphan;
            if (orphan_backlog.empty()) {
                orphan = orphaned_calls.take();
            } else {
                auto res = orphaned_calls.poll_until(next_check);
                orphan = res.first ? res.second : nullptr;
            }

            if (orphan && !try_dispose_orphan(orphan)) {
                orphan_backlog.push_back(orphan);
            }

            if (!orphan_backlog.empty()
                && std::chrono::system_clock::now() > next_check) {

                // zlog_debug(msg_cat, "Scanning backlog of %lu orphans.",
                //            orphan_backlog.size());
                scan_orphans(orphan_backlog);
                next_check = std::chrono::system_clock::now() + orphan_interval;
            }
        }
    } catch (queue::queue_closed&) {
        zlog_debug(msg_cat, "Orphan queue is closed, GC thread exiting.");
        return;
    }
}

void scan_orphans(std::list<BaseSlot*>& calls)
{
    for (auto slot_i = calls.begin(); slot_i != calls.end();) {

        BaseSlot* orphan = *slot_i;
        if (try_dispose_orphan(orphan)) {
            slot_i = calls.erase(slot_i);
        } else {
            ++slot_i;
        }
    }
}

bool try_dispose_orphan(BaseSlot* orphan)
{
    assert(orphan);
    std::unique_lock<decltype(orphan->owned)>
        slot_lock(orphan->owned, std::try_to_lock);
    if (slot_lock && is_terminal(orphan->state)) {
        // zlog_debug(msg_cat, "Disposing of terminal orphan: %p", orphan);
        orphan->dispose();
        return true;
    } else {
        return false;
    }
}

uint64_t parse_size(const char *spec)
{
    char *endp = nullptr;
    uint64_t coeff = strtoul(spec, &endp, 10);
    uint64_t val;
    assert(endp);
    switch (*endp) {
    case '\0':
        val = coeff;
        break;
    case 'k':
    case 'K':
        val = coeff * 1024;
        break;
    case 'm':
    case 'M':
        val = coeff * 1024 * 1024;
        break;
    case 'g':
    case 'G':
        val = coeff * 1024 * 1024 * 1024;
        break;
    default:
        zlog_error(shm_cat, "Unhandled trailing character %c in size", *endp);
        val = coeff;
    }
    return val;
}

}

RaftError parse_argv(int argc, char *argv[], RaftConfig &cfg)
{
    cfg = default_config();

    int opterr_old = opterr;
    opterr = 0;

    while (true) {
        int optionIdx;
        int c = getopt_long(argc, argv, "d:p:s", LONG_OPTS, &optionIdx);
        if (c == -1)
            break;
        switch (c) {
        case 'm':
            strncpy(cfg.shm_path, optarg, 255);
            break;
        case 'M':
            cfg.shm_size = parse_size(optarg);
            break;
        case 'd': // Raft dir
            strncpy(cfg.base_dir, optarg, 255);
            break;
        case 'p': // port
            cfg.listen_port = atoi(optarg);
            break;
        case 's': // single-node
            cfg.EnableSingleNode = true;
            break;
        case 'P': // peers
            strncpy(cfg.peers, optarg, 255);
            break;
        case '?': // unknown arg
            opterr = opterr_old;
            optind = optind-1;
            return RAFT_SUCCESS;
        }
    }

    opterr = opterr_old;
    return RAFT_SUCCESS;
}

RaftConfig default_config()
{
    const static uint64_t US = 1000; // ns
    const static uint64_t MS = 1000 * US;
    const static uint64_t S  = 1000 * MS;
    const static uint64_t M  = 60 * S;
    

    RaftConfig cfg;
    strncpy(cfg.shm_path, SHM_PATH, 255);
    cfg.shm_size = SHM_SIZE;
    cfg.base_dir[0] = '\0';
    cfg.peers[0] = '\0';
    cfg.listen_port = 9001;

    // as dumped from Go
    cfg.HeartbeatTimeout = 1*S;
    cfg.ElectionTimeout = 1*S;
    cfg.CommitTimeout = 50*MS;
    cfg.MaxAppendEntries = 64;
    cfg.ShutdownOnRemove = true;
    cfg.DisableBootstrapAfterElect = true;
    cfg.TrailingLogs = 10240;
    cfg.SnapshotInterval = 2*M;
    cfg.SnapshotThreshold = 8192;
    cfg.EnableSingleNode = false;
    cfg.LeaderLeaseTimeout = 500*MS;

    cfg.RetainSnapshots = 8;

    return cfg;
}

void shm_init(const char* path, bool create, const RaftConfig* config)
{
    init_stats();
    if (zlog_init("zlog.conf")) {
        fprintf(stderr, "zlog init failed\n");
    }
    msg_cat = zlog_get_category("raft_msg");
    fsm_cat = zlog_get_category("raft_fsm");
    shm_cat = zlog_get_category("raft_shm");

    // register on-exit callback to call remove()?
    if (create) {
        // client side
        assert(config);
        struct stat shm_stat;
        if (stat(path, &shm_stat) == 0) {
            if (unlink(path) == -1) {
                perror("Failed to remove old shared memory file");
                exit(1);
            }
        } else if (errno != ENOENT) {
            perror("Problem with shared memory file");
            exit(1);
        }
        shm = managed_mapped_file(boost::interprocess::create_only, 
                                  path, config->shm_size);
        zlog_debug(shm_cat, "Mapped shared memory file %s, %llu MB.",
                   path, config->shm_size / 1048576);
        scoreboard = shm.construct<Scoreboard>(unique_instance)();
        RaftConfig* shared_config = shm.construct<RaftConfig>(unique_instance)();
        *shared_config = *config;
        strncpy(shared_config->shm_path, path, 255);
        init_client_allocators();
        orphaned_calls.reset();
        zlog_debug(shm_cat, "Initialized shared memory and scoreboard.");
    } else {
        // Raft side
        assert(!config);
        shm = managed_mapped_file(boost::interprocess::open_only,
                                  path);
        zlog_debug(shm_cat, "Opened shared memory file %s.", path);
        // unlink the file after we've mapped it, nobody else will need it
        // XXX: add option to leave it for debugging?
        if (unlink(path) == -1) {
            perror("Failed to unlink shared memory file");
            exit(1);
        }

        auto ret = shm.find<Scoreboard>(unique_instance);
        scoreboard = ret.first;
        assert(scoreboard);
        init_raft_allocators();
        zlog_debug(shm_cat, "Found scoreboard.");
    }
    zlog_debug(shm_cat, "Mapped shared memory at base address %p.",
               raft::shm.get_address());
}

void shm_cleanup()
{
    scoreboard->api_queue.close();

    orphaned_calls.close();
    assert(orphan_gc_thread.joinable());
    orphan_gc_thread.join();
    
    if (raft_watcher.joinable())
        raft_watcher.join();

    shm = decltype(shm)();
}

pid_t run_raft()
{
    pid_t kidpid = fork();
    if (kidpid == -1) {
        perror("Cannot fork");
        exit(1);
    } else if (kidpid) {
        // parent
        raft_pid = kidpid;
        // start the watcher thread
        assert(! raft_watcher.joinable());
        raft_watcher = std::thread(watch_raft_proc, raft_pid);
        // start the call GC thread
        assert(! orphan_gc_thread.joinable());
        orphan_gc_thread = std::thread(run_orphan_gc);
        return kidpid;
    } else {
        // child
        auto config = shm.find<RaftConfig>(unique_instance).first;
        auto argv = build_raft_argv(*config);
        int rc = execvp("raft_if", (char * const *)argv.data());
        if (rc) {
            perror("Exec failed");
        }
        exit(1);
    }
}

void orphan_cleanup(const ApplyArgs args)
{
    // zlog_debug(shm_cat, "Cleaning up orphan Apply args.");
    const char* raw_buf = (const char*) args.cmd_buf.get();
    if (in_shm_bounds((void*) raw_buf)) {
        free_raft_buffer(raw_buf);
    }
}

BaseSlot::BaseSlot(CallTag tag_)
    : tag(tag_),
      state(CallState::Pending),
      client_state(ClientState::Issued),
      retval(0),
      error(RAFT_SUCCESS),
      timings()
{}

BaseSlot::call_rec BaseSlot::rec()
{
    return { tag, pointer(this) };
}

void BaseSlot::reply(RaftError err)
{
    error = err;
    assert(! is_terminal(state));
    state = (err == RAFT_SUCCESS) ? CallState::Success : CallState::Error;
    ret_cond.notify_one();
    timings.record("reply sent");
}

void BaseSlot::reply(uint64_t retval_)
{
    retval = retval_;
    reply(RAFT_SUCCESS);
}


void BaseSlot::wait()
{
    std::unique_lock<interprocess_mutex> lock(owned);
    ret_cond.wait(lock, [&] () { return is_terminal(state); });
    client_state = ClientState::Observed;
    timings.record("result received");
}

int kill_raft_()
{
    scoreboard->raft_killed = true;
    return kill(raft_pid, SIGTERM);
}

namespace {

std::vector<const char*> build_raft_argv(const RaftConfig& cfg)
{
    std::vector<const char*> args;
    args.push_back("raft_if");

    args.push_back("--shm-path");
    args.push_back(cfg.shm_path);

    args.push_back(nullptr);

    return args;
}

void watch_raft_proc(pid_t raft_pid)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(raft_pid, &status, 0);
        assert(pid != 0);
        if (pid > 0) {
            if (WIFSTOPPED(status)) {
                // attached a debugger or something...
                continue;
            } else {
                report_process_status("Raft process", raft_pid, status);
                // TODO: bubble this back up to the client? recover?
                if (scoreboard->shutdown_requested
                    && WIFEXITED(status)
                    && WEXITSTATUS(status) == 0) {
                    return;
                } else if (scoreboard->shutdown_requested
                           && scoreboard->raft_killed) {
                    zlog_warn(shm_cat, "Raft intentionally killed during shutdown!");
                    return;
                } else {
                    exit(1);
                }
            }
        } else {
            perror("waitpid failed");
            exit(1);
        }
    }
}

void report_process_status(const char *desc, pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        fprintf(stderr, "%s (pid %d) exited with status %d.\n",
                desc, pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s (pid %d) terminated by signal %d.\n",
                desc, pid, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        fprintf(stderr, "%s (pid %d) stopped by signal %d.\n",
                desc, pid, WSTOPSIG(status));
    } else {
        assert(false && "impossible process status!");
    }
}

void init_client_allocators()
{
#define api_call(name, argT, hasRet) \
    api::name::allocator = new api::name::allocator_t(shm.get_segment_manager());
#include "raft_api_calls.h"
#undef api_call
}

void init_raft_allocators()
{
#define api_call(name, argT, hasRet)                                    \
    api::name::allocator = new api::name::allocator_t(shm.get_segment_manager());
#include "raft_fsm_calls.h"
#undef api_call
}

}

Timings::Timings(time_point t)
    : n_entries(0)
{
    record("start", t);
}

void Timings::record(const char *tag)
{
    record(tag, clock::now());
}

void Timings::record(const char *tag, time_point t)
{
    if (n_entries < MAX_ENT) {
        entry& ent = entries[n_entries++];
        ent.ts = t;
        strncpy(ent.tag, tag, 19);
    }
}

void Timings::print()
{
    if (!msg_timing)
        return;

    if (n_entries <= 1) {
        zlog_warn(msg_cat, "No timing data!");
        return;
    }
    
    time_point start = entries[0].ts;
    time_point prev  = entries[0].ts;
    for (uint32_t i = 1; i < n_entries; ++i) {
        int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(entries[i].ts - start).count();
        int64_t delta_us = std::chrono::duration_cast<std::chrono::microseconds>(entries[i].ts - prev).count();
        fprintf(stderr, "%-20s @ %7" PRId64 " us, delta %7" PRId64 " us.\n",
                entries[i].tag, elapsed_us, delta_us);
        prev = entries[i].ts;
    }
}

}
