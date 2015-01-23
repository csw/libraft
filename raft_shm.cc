#include <cstdio>
#include <unistd.h>

#include "raft_shm.h"

namespace raft {

using boost::interprocess::unique_instance;

managed_shared_memory shm;
Scoreboard* scoreboard;

Scoreboard::Scoreboard()
    : is_leader(false)
{}

CallSlot& Scoreboard::grab_slot()
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

SlotHandle::SlotHandle(Scoreboard& sb)
    : slot(sb.grab_slot()),
      slot_lock(slot.slot_busy, std::adopt_lock)
{
    ++slot.refcount;
}

SlotHandle::~SlotHandle()
{
    --slot.refcount;
}

void init(const char* name, bool create)
{
    // [create]
    // register on-exit callback to call remove()
    if (create) {
        shm = managed_shared_memory(boost::interprocess::open_or_create_t{}, 
                                    "raft", SHM_SIZE);
        scoreboard = shm.construct<Scoreboard>(unique_instance)();
        fprintf(stderr, "[%d]: Initialized shared memory and scoreboard.\n",
                getpid());
    } else {
        shm = managed_shared_memory(boost::interprocess::open_only_t{},
                                    "raft");
        fprintf(stderr, "[%d]: Opened shared memory.\n",
                getpid());
        while (!scoreboard) {
            sleep(1);
            auto ret = shm.find<Scoreboard>(unique_instance);
            scoreboard = ret.first;
        }
        fprintf(stderr, "[%d]: Found scoreboard.\n",
                getpid());
    }
    fprintf(stderr, "[%d]: Mapped shared memory at base address %p.\n",
            getpid(), raft::shm.get_address());
}

}
