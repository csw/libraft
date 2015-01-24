#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

#include "raft_shm.h"

namespace raft {

using boost::interprocess::unique_instance;

managed_mapped_file shm;
Scoreboard* scoreboard;

Scoreboard::Scoreboard()
    : is_leader(false)
{}

void Scoreboard::wait_for_raft(pid_t raft_pid)
{
    while (! is_raft_running) {
        int raft_stat;
        int rc = waitpid(raft_pid, &raft_stat, WNOHANG);
        if (rc == 0) {
            // nothing to report
            usleep(100000); // 100 ms
        } else if (rc > 0) {
            // Raft process is terminated or stopped
            if (WIFEXITED(raft_stat)) {
                fprintf(stderr, "Raft process exited with status %d.\n",
                        WEXITSTATUS(raft_stat));
            } else if (WIFSIGNALED(raft_stat)) {
                fprintf(stderr, "Raft process terminated by signal %d.\n",
                        WTERMSIG(raft_stat));
            } else if (WIFSTOPPED(raft_stat)) {
                fprintf(stderr, "Raft process stopped by signal %d.\n",
                        WSTOPSIG(raft_stat));
            }
            abort();
        } else {
            perror("waitpid error");
            abort();
        }

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
        shm = managed_mapped_file(boost::interprocess::open_only,
                                  SHM_PATH);
        fprintf(stderr, "[%d]: Opened shared memory.\n",
                getpid());
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
