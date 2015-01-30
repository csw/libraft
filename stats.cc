#include "stats.h"
#include "make_unique.h"

namespace raft {

std::unique_ptr<Stats> stats;

void init_stats() {
    stats = make_unique<Stats>();
}

}
