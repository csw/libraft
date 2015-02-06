#ifndef RAFT_DEFS_H
#define RAFT_DEFS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     shm_path[256];
    uint64_t shm_size;
    char     backend_type[16];
    char     base_dir[256];
    uint32_t api_workers;
    bool     verbose;

    uint16_t listen_port;
    char     peers[256];

    uint64_t HeartbeatTimeout;
    uint64_t ElectionTimeout;
    uint64_t CommitTimeout;
    uint32_t MaxAppendEntries;
    bool     ShutdownOnRemove;
    bool     DisableBootstrapAfterElect;
    uint64_t TrailingLogs;
    uint64_t SnapshotInterval;
    uint64_t SnapshotThreshold;
    bool     EnableSingleNode;
    uint64_t LeaderLeaseTimeout;
    char     LogOutput[256];

    uint32_t RetainSnapshots;
} RaftConfig;

typedef uint64_t RaftIndex;

typedef enum {
    RAFT_INVALID_STATE,
    RAFT_FOLLOWER,
    RAFT_CANDIDATE,
    RAFT_LEADER,
    RAFT_SHUTDOWN
} RaftState;

typedef enum raft_log_type {
    RAFT_LOG_COMMAND,
    RAFT_LOG_NOOP,
    RAFT_LOG_ADD_PEER,
    RAFT_LOG_REMOVE_PEER,
    RAFT_LOG_BARRIER
} RaftLogType;

typedef enum raft_error {
    RAFT_SUCCESS,
    RAFT_E_LEADER,
    RAFT_E_NOT_LEADER,
    RAFT_E_LEADERSHIP_LOST,
    RAFT_E_SHUTDOWN,
    RAFT_E_ENQUEUE_TIMEOUT,
    RAFT_E_KNOWN_PEER,
    RAFT_E_UNKNOWN_PEER,
    RAFT_E_LOG_NOT_FOUND,
    RAFT_E_PIPELINE_REPLICATION_NOT_SUPP,
    RAFT_E_TRANSPORT_SHUTDOWN,
    RAFT_E_PIPELINE_SHUTDOWN,
    RAFT_E_OTHER,
    RAFT_E_INVALID_OP,
    RAFT_E_INVALID_ADDRESS,
    RAFT_E_RESOLVE,
    RAFT_E_IN_SHUTDOWN,
    N_RAFT_ERRORS // end marker, not an error!
} RaftError;

#ifdef __cplusplus
}
#endif

#endif /* RAFT_DEFS_H */
