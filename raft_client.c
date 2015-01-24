#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "raft_c_if.h"

typedef uint32_t* fsm_result_t;

const static uint32_t BUFSIZE = 256;

static uint32_t letter_count = 0;

fsm_result_t update_count(const char *buf, size_t len)
{
    const char * const endp = buf+len;
    while (buf < endp) {
        char c = *buf++;
        if (isalpha(*buf++)) {
            ++letter_count;
        } else if (c == '\0') {
            break;
        }
    }

    uint32_t *result = malloc(sizeof(uint32_t)); // yeah, yeah, it's 4
    assert(result);
    fprintf(stderr, "Allocated result object @ %p\n", result);
    *result = letter_count;
    return result;
}

void* FSMApply(uint64_t index, uint64_t term, RaftLogType type, char *cmd, size_t len)
{
    printf("FSM: applying command (%lu bytes @ %p): %s\n",
           len, cmd, cmd);
    return update_count(cmd, len);
}

RaftFSM fsm_def = { &FSMApply };

int main(int argc, char *argv[])
{
    fprintf(stderr, "Raft client starting.\n");

    int runs = 20;

    if (argc > 1) {
        runs = atoi(argv[1]);
    }
    
    raft_init(&fsm_def);

    while (! raft_is_leader()) {
        sleep(1);
    }

    for (int i = 1; i <= runs; ++i) {
        char* buf = alloc_raft_buffer(BUFSIZE);
        fprintf(stderr, "Allocated cmd buffer at %p.\n", buf);
        snprintf(buf, BUFSIZE, "Raft command #%d", i);
        // ignore return value
        fsm_result_t cur_count_p = NULL;
        RaftError err = raft_apply(buf, BUFSIZE, 0, (void**)&cur_count_p);
        if (!err) {
            assert(cur_count_p);
            printf("FSM state: letter count %u.\n", *cur_count_p);
            free(cur_count_p);
        } else {
            fprintf(stderr, "Raft error: %s\n", raft_err_msg(err));
        }
        free_raft_buffer(buf);
        sleep(1);
    }

    return 0;
}
