#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "raft_c_if.h"

const static uint32_t BUFSIZE = 256;

void* FSMApply(uint64_t index, uint64_t term, RaftLogType type, void *data, size_t len)
{
    printf("FSM: applying command (%lu bytes @ %p): %s\n",
           len, data, (char*)data);
    return NULL;
}

RaftFSM fsm_def = { &FSMApply };

int main(int argc, char *argv[])
{
    fprintf(stderr, "C client starting.\n");

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
        void *result = NULL;
        RaftError err = raft_apply(buf, BUFSIZE, 0, &result);
        if (err) {
            fprintf(stderr, "Raft error: %s\n", raft_err_msg(err));
        }
        free_raft_buffer(buf);
        sleep(1);
    }

    return 0;
}
