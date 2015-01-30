#include "gtest/gtest.h"

#include "raft_c_if.h"

class DummyFSM {
public:

    void* apply(uint64_t index, uint64_t term, RaftLogType type,
                char *cmd, size_t len)
    {
        (void) index;
        (void) term;
        (void) type;
        (void) cmd;
        (void) len;

        ++count;
        return nullptr;
    }

    void begin_snapshot(const char *path, raft_snapshot_req s)
    {
        (void) path;
        raft_fsm_take_snapshot(s,
                               (raft_fsm_snapshot_handle) new uint32_t(count),
                               &write_snapshot);
    }

    int restore(const char *path)
    {
        FILE *src = fopen(path, "r");
        if (!src) {
            perror("Opening snapshot pipe failed");
            return 1;
        }

        uint32_t val;
        int scanned = fscanf(src, "%u", &val);
        if (fclose(src)) {
            perror("Closing snapshot pipe failed");
            return 1;
        }
        if (scanned == 1) {
            count = val;
            return 0;
        } else {
            return 1;
        }
    }

    static int write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink)
    {
        auto state = std::unique_ptr<uint32_t>((uint32_t*) handle);
        int chars = fprintf(sink, "%u\n", *state);
        if (chars > 0) {
            return 0;
        } else {
            perror("Writing snapshot failed");
            return 1;
        }
    }

    uint32_t count = 0;
};

DummyFSM *fsm_instance;
static void* FSMApply(uint64_t index, uint64_t term, RaftLogType type,
                      char *cmd, size_t len)
{
    return fsm_instance->apply(index, term, type, cmd, len);
}
static void FSMBeginSnapshot(const char *path, raft_snapshot_req s)
{
    fsm_instance->begin_snapshot(path, s);
}
static int FSMRestore(const char *path)
{
    return fsm_instance->restore(path);
}

class RaftFixture : public ::testing::Test {
public:
    RaftFixture()
    {
        fsm_instance = &fsm;
        raft_init(&fsm_rec, nullptr);
    }

    ~RaftFixture()
    {
        raft_future sf = raft_shutdown();
        raft_future_wait(sf);
        raft_cleanup();
    }

    DummyFSM fsm;
    RaftFSM fsm_rec = { &FSMApply, &FSMBeginSnapshot, &FSMRestore };

};

TEST_F(RaftFixture, Simple) {
}
