#ifndef RAFT_C_IF_H
#define RAFT_C_IF_H

/*
 * libraft, C interface to Hashicorp's Raft implementation.
 * Copyright (C) 2015 Clayton Wheeler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

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
#include <stdint.h>

#include "raft_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

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

// Optional snapshot support

typedef void* raft_fsm_snapshot_handle;

/**
 * Callback function for writing a snapshot from the state handle.
 *
 * Must not close the supplied sink.
 */
typedef int (*raft_fsm_snapshot_func)(raft_fsm_snapshot_handle handle, FILE* sink);

void raft_fsm_take_snapshot(raft_snapshot_req req,
                            raft_fsm_snapshot_handle h,
                            raft_fsm_snapshot_func f);

// Top half; client side

void raft_default_config(RaftConfig *cfg);
RaftError raft_parse_argv(int argc, char *argv[], RaftConfig *cfg);
pid_t raft_init(RaftFSM *fsm, const RaftConfig *cfg);
void raft_cleanup();

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

/**
 * Return the state Raft is currently in.
 *
 * Synchronous.
 *
 * @retval State, or 0 (RAFT_INVALID_STATE) on error.
 */
RaftState   raft_state();

/**
 * Return the time of last contact by a leader.
 *
 * This only makes sense if we are currently a follower. Synchronous.
 *
 * @retval Time of last contact; 0 on error.
 */
time_t      raft_last_contact();

/**
 * Return the last index in stable storage.
 *
 * This is either from the last log or from the last
 * snapshot. Synchronous.
 *
 * @retval Index, or 0 on error.
 */
RaftIndex   raft_last_index();

/**
 * Return the current leader of the cluster.
 *
 * @param [out] leader Address of a RaftAddr struct to hold the result.
 *
 * @retval RAFT_SUCCESS on success. RAFT_E_UNKNOWN_PEER if there is no
 * current leader or the leader is unknown.
 */
RaftError   raft_leader(RaftAddr* leader);

raft_future raft_snapshot();

raft_future raft_shutdown();


raft_future raft_add_peer(const char *host, uint16_t port);

raft_future raft_remove_peer(const char *host, uint16_t port);

// TODO: barrier, snapshot, administrative commands, etc.

RaftError raft_future_wait(raft_future f);
//int raft_future_wait_for(raft_future f, uint64_t wait_ms);
RaftError raft_future_get_ptr(raft_future f, void** value_ptr);
uint64_t  raft_future_get_value(raft_future f);
/**
 * Dispose of resources held by a raft_future.
 */
void raft_future_dispose(raft_future f);

char* alloc_raft_buffer(size_t len);
void free_raft_buffer(const char* buf);

const char* raft_err_msg(RaftError err);

#ifdef __cplusplus
}
#endif
    

#endif /* RAFT_C_IF_H */
