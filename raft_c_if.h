#ifndef RAFT_C_IF_H
#define RAFT_C_IF_H

/** @file
 * C interface to Raft consensus system.
 *
 * This wraps Hashicorp's Go implementation of Raft. It runs the Raft
 * code in a child process, using shared memory to for communication.
 *
 * To use Raft, a program must call raft_apply() to send commands to
 * Raft, and implement a finite state machine to be driven by the
 * functions defined in the RaftFSM struct.
 *
 * raft_init() starts the Raft process and registers the FSM code.
 *
 * \sa http://en.wikipedia.org/wiki/Consensus_(computer_science)
 * \sa https://github.com/hashicorp/raft
 * \sa https://ramcloud.stanford.edu/wiki/download/attachments/11370504/raft.pdf
 */

#include <stdbool.h>

#include "raft_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bottom half; FSM side

typedef struct raft_fsm {
    void* (*apply)(uint64_t index, uint64_t term, RaftLogType type,
                   char *cmd, size_t len);
    // TODO: snapshot
    // TODO: restore
} RaftFSM;

// Top half; client side

pid_t raft_init(RaftFSM *fsm);

bool raft_is_leader();

/**
 * Apply a command to the Raft FSM.
 *
 * @param cmd Opaque command buffer, allocated with alloc_raft_buffer().
 * @param cmd_len Length of the command (may be smaller than the buffer
 *                itself).
 * @param timeout_ns Timeout in nanoseconds to wait for Raft.
 * @param res [out] Location to store result pointer.
 * @retval error Error from Raft; 0 on success.
 */
RaftError raft_apply(char* cmd, size_t cmd_len, uint64_t timeout_ns,
                     void **res);

// TODO: barrier, snapshot, administrative commands, etc.

char* alloc_raft_buffer(size_t len);
void free_raft_buffer(char* buf);

const char* raft_err_msg(RaftError err);

#ifdef __cplusplus
}
#endif
    

#endif /* RAFT_C_IF_H */
