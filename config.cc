#include <cstdlib>
#include <cstring>

#include "config.h"
#include "raft_shm.h"

namespace raft {

const struct option arg::LONG_OPTS[] = {
    { "shm-path",      required_argument,  NULL,  arg::ShmPath },
    { "shm-size",      required_argument,  NULL,  arg::ShmSize },
    { "backend-type",  required_argument,  NULL,  arg::Backend },
    { "dir",           required_argument,  NULL,  arg::Dir     },
    { "port",          required_argument,  NULL,  arg::Port    },
    { "single",        no_argument,        NULL,  arg::Single  },
    { "peers",         required_argument,  NULL,  arg::Peers   },
    { "verbose",       no_argument,        NULL,  arg::Verbose },
    { "",              0,                  NULL,  0            }
};

namespace {

uint64_t parse_size(const char *spec);

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
    strncpy(cfg.backend_type, "boltdb", 15);
    cfg.base_dir[0] = '\0';
    cfg.api_workers = 4;
    cfg.verbose = false;

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

bool arg::is_valid(int option)
{
    return (option >= ShmPath && option < END_OPTS);
}


int arg::apply(RaftConfig& cfg, Getopt option, const char *arg)
{
    switch (option) {
    case arg::ShmPath:
        assert(arg);
        strncpy(cfg.shm_path, arg, 255);
        break;
    case arg::ShmSize:
        assert(arg);
        cfg.shm_size = parse_size(arg);
        break;
    case arg::Backend:
        assert(arg);
        strncpy(cfg.backend_type, arg, sizeof(cfg.backend_type)-1);
        break;
    case arg::Dir:
        assert(arg);
        strncpy(cfg.base_dir, arg, 255);
        break;
    case arg::Port:
        assert(arg);
        cfg.listen_port = atoi(arg);
        break;
    case arg::Single:
        cfg.EnableSingleNode = true;
        break;
    case arg::Peers:
        assert(arg);
        strncpy(cfg.peers, arg, 255);
        break;
    case arg::Verbose:
        cfg.verbose = true;
        break;
    default:
        assert(false && "unhandled config flag");
    }
    return 0;
}

namespace {

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

} // end anon namespace

} // end namespace raft
