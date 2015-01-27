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

typedef struct {
    char base_dir[256];

    bool single_node;
} RaftConfig;

// Bottom half; FSM side

typedef void* raft_future;
typedef void* raft_snapshot_req;

typedef struct raft_fsm {
    void* (*apply)(uint64_t index, uint64_t term, RaftLogType type,
                   char *cmd, size_t len);
    void (*begin_snapshot)(const char *path, raft_snapshot_req s);
    int (*restore)(const char *path);
} RaftFSM;

void raft_fsm_snapshot_complete(raft_snapshot_req s, bool success);

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

raft_future raft_apply_async(char* cmd, size_t cmd_len, uint64_t timeout_ns);

raft_future raft_barrier(uint64_t timeout_ns);

raft_future raft_verify_leader();

raft_future raft_snapshot();


raft_future raft_add_peer(const char *host, uint16_t port);

raft_future raft_remove_peer(const char *host, uint16_t port);

// TODO: barrier, snapshot, administrative commands, etc.

RaftError raft_future_wait(raft_future f);
//int raft_future_wait_for(raft_future f, uint64_t wait_ms);
RaftError raft_future_get_ptr(raft_future f, void** value_ptr);
void raft_future_dispose(raft_future f);

char* alloc_raft_buffer(size_t len);
void free_raft_buffer(char* buf);

const char* raft_err_msg(RaftError err);

#ifdef __cplusplus
}
#endif
    

#endif /* RAFT_C_IF_H */
