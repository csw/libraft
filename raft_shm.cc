#include <cstdio>
#include <getopt.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include <list>
#include <string>

#include "raft_shm.h"

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
    { "dir",      required_argument, NULL, 'd' },
    { "p",        required_argument, NULL, 'p' },
    { "port",     required_argument, NULL, 'p' },
    { "single",   no_argument,       NULL, 's' },
    { "peers",    required_argument, NULL, 'P' },
    { "",         0,                 NULL, 0   }
};

RaftConfig config;

std::mutex orphan_mutex;
std::list<BaseSlot*> orphaned_calls;

std::thread raft_watcher;
std::thread orphan_gc_thread;

std::vector<const char*> build_raft_argv(RaftConfig cfg);
void watch_raft_proc(pid_t raft_pid);
void run_orphan_gc();
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
bool                shutdown_requested = false;

bool is_terminal(CallState state)
{
    switch (state) {
    case CallState::Pending:
    case CallState::Dispatched:
        return false;
    case CallState::Success:
    case CallState::Error:
        return true;
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
    : is_leader(false)
{}

void Scoreboard::wait_for_raft(pid_t raft_pid)
{
    while (! is_raft_running) {
        // if the process exits, the raft_watcher thread will catch it
        usleep(100000); // 100 ms
    }
}

bool in_shm_bounds(void* ptr)
{
    char* base = (char*) shm.get_address();
    char* cptr = (char*) ptr;
    return (cptr >= base) && (cptr < base + shm.get_size());
}

void track_orphan(BaseSlot* slot)
{
    std::unique_lock<std::mutex> orphan_lock(orphan_mutex);
    orphaned_calls.push_back(slot);
}

namespace {

void run_orphan_gc()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> orphan_lock(orphan_mutex);
            for (auto slot_i = orphaned_calls.begin();
                 slot_i != orphaned_calls.end();) {
                auto& slot = **slot_i;
                std::unique_lock<decltype(slot.owned)>
                    slot_lock(slot.owned, std::try_to_lock);

                if (slot_lock && is_terminal(slot.state)) {
                    slot.dispose();
                    slot_i = orphaned_calls.erase(slot_i);
                } else {
                    ++slot_i;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}

void process_args(int argc, char *argv[])
{
    config.base_dir[0] = '\0';
    config.peers[0] = '\0';
    config.listen_port = 0;
    config.single_node = false;

    int opterr_old = opterr;
    opterr = 0;

    while (true) {
        int optionIdx;
        int c = getopt_long(argc, argv, "d:p:s", LONG_OPTS, &optionIdx);
        if (c == -1)
            break;
        switch (c) {
        case 'd': // Raft dir
            strncpy(config.base_dir, optarg, 255);
            break;
        case 'p': // port
            config.listen_port = atoi(optarg);
            break;
        case 's': // single-node
            config.single_node = true;
            break;
        case 'P': // peers
            strncpy(config.peers, optarg, 255);
            break;
        case '?': // unknown arg
            opterr = opterr_old;
            optind = optind-1;
            return;
        }
    }

    opterr = opterr_old;
}

void shm_init(const char* name, bool create)
{
    if (zlog_init("zlog.conf")) {
        fprintf(stderr, "zlog init failed\n");
    }
    msg_cat = zlog_get_category("raft_msg");
    fsm_cat = zlog_get_category("raft_fsm");
    shm_cat = zlog_get_category("raft_shm");

    // register on-exit callback to call remove()?
    if (create) {
        // client side
        struct stat shm_stat;
        if (stat(SHM_PATH, &shm_stat) == 0) {
            if (unlink(SHM_PATH) == -1) {
                perror("Failed to remove old shared memory file");
                exit(1);
            }
        } else if (errno != ENOENT) {
            perror("Problem with shared memory file");
            exit(1);
        }
        shm = managed_mapped_file(boost::interprocess::create_only, 
                                  SHM_PATH, SHM_SIZE);
        scoreboard = shm.construct<Scoreboard>(unique_instance)();
        init_client_allocators();
        zlog_debug(shm_cat, "Initialized shared memory and scoreboard.");
    } else {
        // Raft side
        shm = managed_mapped_file(boost::interprocess::open_only,
                                  SHM_PATH);
        zlog_debug(shm_cat, "Opened shared memory.");
        // unlink the file after we've mapped it, nobody else will need it
        // XXX: add option to leave it for debugging?
        if (unlink(SHM_PATH) == -1) {
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
    if (orphan_gc_thread.joinable()) {
        if (pthread_cancel(orphan_gc_thread.native_handle())) {
            perror("Failed to cancel orphan GC thread");
        }
        orphan_gc_thread.join();
    }
    
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
        auto argv = build_raft_argv(config);
        int rc = execvp("raft_if", (char * const *)argv.data());
        if (rc) {
            perror("Exec failed");
        }
        exit(1);
    }
}

BaseSlot::BaseSlot(CallTag tag_)
    : ret_ready(false),
      tag(tag_),
      refcount(0),
      state(CallState::Pending),
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
    state = (err == RAFT_SUCCESS) ? CallState::Success : CallState::Error;
    assert(! ret_ready);
    ret_ready = true;
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
    ret_cond.wait(lock, [&] () { return ret_ready; });
}

namespace {

std::vector<const char*> build_raft_argv(RaftConfig cfg)
{
    std::vector<const char*> args;
    args.push_back("raft_if");

    if (*cfg.base_dir) {
        args.push_back("-dir");
        args.push_back(cfg.base_dir);
    }
    if (cfg.listen_port) {
        args.push_back("-port");
        auto s = new std::string(std::to_string(cfg.listen_port));
        args.push_back(s->c_str());
    }
    if (cfg.single_node) {
        args.push_back("-single");
    }
    if (*cfg.peers) {
        args.push_back("-peers");
        args.push_back(cfg.peers);
    }
    args.push_back(nullptr);

    /*
    fprintf(stderr, "Raft argv: ");
    for (const char* arg : args) {
        fprintf(stderr, "%s ", arg);
    }
    fprintf(stderr, "\n");
    */
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
                if (shutdown_requested
                    && WIFEXITED(status)
                    && WEXITSTATUS(status) == 0) {
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
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(entries[i].ts - start).count();
        auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(entries[i].ts - prev).count();
        fprintf(stderr, "%-20s @ %7lld us, delta %7lld us.\n",
                entries[i].tag, elapsed_us, delta_us);
        prev = entries[i].ts;
    }
}

}
