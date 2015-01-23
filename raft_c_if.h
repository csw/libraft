#ifndef RAFT_C_IF_H
#define RAFT_C_IF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    void* alloc_raft_buffer(size_t len);
    void free_raft_buffer(void* buf);

    // top half; client side
    bool raft_is_leader();

    // blocking
    void* raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns);

    // bottom half; FSM side

    typedef enum raft_log_type {
        RAFT_LOG_COMMAND,
        RAFT_LOG_NOOP,
        RAFT_LOG_ADD_PEER,
        RAFT_LOG_REMOVE_PEER,
        RAFT_LOG_BARRIER
    } RaftLogType;

    typedef struct raft_fsm {
        void* (*apply)(uint64_t index, uint64_t term, RaftLogType type, void *data, size_t len);
        // TODO: snapshot
        // TODO: restore
    } RaftFSM;

    void raft_init(RaftFSM *fsm);

#ifdef __cplusplus
}
#endif
    

#endif /* RAFT_C_IF_H */
