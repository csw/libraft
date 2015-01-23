#ifndef RAFT_DEFS_H
#define RAFT_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

    typedef enum raft_log_type {
        RAFT_LOG_COMMAND,
        RAFT_LOG_NOOP,
        RAFT_LOG_ADD_PEER,
        RAFT_LOG_REMOVE_PEER,
        RAFT_LOG_BARRIER
    } RaftLogType;

#ifdef __cplusplus
}
#endif

#endif /* RAFT_DEFS_H */
