// -*- c++ -*-
#ifndef CONFIG_H
#define CONFIG_H

#include <getopt.h>

#include "raft_defs.h"

namespace raft {

namespace arg {

enum Getopt {
    ShmPath=0xb0000, ShmSize, Dir, Port, Single, Peers,
    END_OPTS
};

bool is_valid(int optkey);

extern const struct option LONG_OPTS[];

int apply(RaftConfig& config, Getopt option, const char *arg);

} // end namespace arg

RaftConfig default_config();

}
#endif /* CONFIG_H */
