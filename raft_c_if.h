#ifndef RAFT_C_IF_H
#define RAFT_C_IF_H

#include <stdbool.h>

#include "raft_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

    void* alloc_raft_buffer(size_t len);
    void free_raft_buffer(void* buf);

    const char* raft_err_msg(RaftError err);

    // top half; client side
    bool raft_is_leader();

    // blocking
    RaftError raft_apply(void **res, char* cmd, size_t cmd_len, uint64_t timeout_ns);

    // bottom half; FSM side

    typedef struct raft_fsm {
        void* (*apply)(uint64_t index, uint64_t term, RaftLogType type, void *data, size_t len);
        // TODO: snapshot
        // TODO: restore
    } RaftFSM;

    pid_t raft_init(RaftFSM *fsm);

#ifdef __cplusplus
}
#endif
    

#endif /* RAFT_C_IF_H */
