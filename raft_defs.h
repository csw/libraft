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
        N_RAFT_ERRORS // end marker, not an error!
    } RaftError;

#ifdef __cplusplus
}
#endif

#endif /* RAFT_DEFS_H */
