#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

#include "raft_shm.h"

namespace raft {

using boost::interprocess::unique_instance;

namespace {

std::thread raft_watcher;

void watch_raft_proc(pid_t raft_pid);
void report_process_status(const char *desc, pid_t pid, int status);

}

pid_t               raft_pid;
managed_mapped_file shm;
Scoreboard*         scoreboard;

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

RaftCallSlot& Scoreboard::grab_slot()
{
    for (;;) {
        for (int i = 0; i < 16; ++i) {
            if (slots[i].slot_busy.try_lock()) {
                return slots[i];
            }
        }
        usleep(5000); // 5 ms
    }
}

template <typename CT>
SlotHandle<CT>::SlotHandle(CallSlot<CT>& slot_)
    : slot(slot_),
      slot_lock(slot.slot_busy)
{
    ++slot.refcount;
}

template <typename CT>
SlotHandle<CT>::SlotHandle(CallSlot<CT>& slot_, std::adopt_lock_t _t)
    : slot(slot_),
      slot_lock(slot.slot_busy, std::adopt_lock)
{
    ++slot.refcount;
}

template <typename CT>
SlotHandle<CT>::~SlotHandle()
{
    --slot.refcount;
}

template class SlotHandle<APICall>;
template class SlotHandle<FSMOp>;

bool in_shm_bounds(void* ptr)
{
    char* base = (char*) shm.get_address();
    char* cptr = (char*) ptr;
    return (cptr >= base) && (cptr < base + shm.get_size());
}

void shm_init(const char* name, bool create)
{
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
        fprintf(stderr, "[%d]: Initialized shared memory and scoreboard.\n",
                getpid());
    } else {
        // Raft side
        shm = managed_mapped_file(boost::interprocess::open_only,
                                  SHM_PATH);
        fprintf(stderr, "[%d]: Opened shared memory.\n",
                getpid());
        // unlink the file after we've mapped it, nobody else will need it
        // XXX: add option to leave it for debugging?
        if (unlink(SHM_PATH) == -1) {
            perror("Failed to unlink shared memory file");
            exit(1);
        }

        auto ret = shm.find<Scoreboard>(unique_instance);
        scoreboard = ret.first;
        assert(scoreboard);
        fprintf(stderr, "[%d]: Found scoreboard.\n",
                getpid());
    }
    fprintf(stderr, "[%d]: Mapped shared memory at base address %p.\n",
            getpid(), raft::shm.get_address());
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
        assert(raft_watcher.get_id() == std::thread::id());
        raft_watcher = std::thread(watch_raft_proc, raft_pid);
        return kidpid;
    } else {
        // child
        
        int rc = execlp("raft_if", "raft_if", "-single", nullptr);
        if (rc) {
            perror("Exec failed");
        }
        exit(1);
    }
}

namespace {

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
                exit(1);
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
    if (n_entries <= 1) {
        fprintf(stderr, "No timing data!\n");
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
