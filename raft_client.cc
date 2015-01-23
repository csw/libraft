#include <cstdio>
#include <cstdint>
#include <unistd.h>

#include "raft_client.h"
#include "raft_shm.h"
#include "raft_c_if.h"

const static uint32_t BUFSIZE = 256;

int main(int argc, char *argv[])
{
    fprintf(stderr, "C client starting.\n");
    raft::init("raft", true);

    while (! raft_is_leader()) {
        sleep(1);
    }

    for (int i = 1; i <= 20; ++i) {
        // oops, need C version
        char* buf = (char*) raft::shm.allocate(BUFSIZE);
        fprintf(stderr, "Allocated cmd buffer at %p.\n", buf);
        snprintf(buf, BUFSIZE, "Raft command #%d", i);
        // ignore return value
        raft_apply(buf, BUFSIZE, 0);
        raft::shm.deallocate(buf);
        sleep(1);
    }

    return 0;
}
