#include <chrono>
#include <thread>
#include <memory>

#include "gtest/gtest.h"

#include "raft_c_if.h"
#include "stats.h"

class FSM {
public:
    virtual ~FSM() = default;

    virtual void* apply(uint64_t index, uint64_t term, RaftLogType type,
                        char *cmd, size_t len) = 0;
    virtual void begin_snapshot(const char *path, raft_snapshot_req s) = 0;
    virtual int restore(const char *path) = 0;
    virtual int write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink) = 0;
};

static int FSMWriteSnapshot(raft_fsm_snapshot_handle handle, FILE* sink);

class DummyFSM : public FSM {
public:
    DummyFSM()
        : delay_us(0)
    {}

    void* apply(uint64_t index, uint64_t term, RaftLogType type,
                char *cmd, size_t len)
    {
        (void) index;
        (void) term;
        (void) type;
        (void) cmd;
        (void) len;

        ++count;

        std::this_thread::sleep_for(delay_us);

        return nullptr;
    }

    void begin_snapshot(const char *path, raft_snapshot_req s)
    {
        (void) path;
        raft_fsm_take_snapshot(s,
                               (raft_fsm_snapshot_handle) new uint32_t(count),
                               &FSMWriteSnapshot);
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

    int write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink)
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
    std::chrono::microseconds delay_us;
};

FSM* fsm_instance;

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
static int FSMWriteSnapshot(raft_fsm_snapshot_handle handle, FILE* sink)
{
    return fsm_instance->write_snapshot(handle, sink);
}

class RaftFixture : public ::testing::Test {
public:
    RaftFixture()
    {
        fsm_instance = &fsm;
        raft_default_config(&config);
        config.EnableSingleNode = true;
        raft_init(&fsm_rec, &config);
    }

    ~RaftFixture()
    {
        raft_future sf = raft_shutdown();
        raft_future_wait(sf);
        raft_cleanup();
    }

    DummyFSM fsm;
    RaftConfig config;
    RaftFSM fsm_rec = { &FSMApply, &FSMBeginSnapshot, &FSMRestore };

};

void wait_until_leader()
{
    while (! raft_is_leader()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST_F(RaftFixture, Simple) {
    wait_until_leader();

    for (int i = 0; i < 5; i++) {
        char* buf = alloc_raft_buffer(256);
        raft_future f = raft_apply_async(buf, 256, 0);
        RaftError err = raft_future_wait(f);
        ASSERT_EQ(err, RAFT_SUCCESS);
        free_raft_buffer(buf);
        raft_future_dispose(f);
    }
    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}

TEST_F(RaftFixture, BadApply) {
    wait_until_leader();

    raft_future f;
    RaftError err;
    char* buf = alloc_raft_buffer(256);
    char* badbuf = new char[256];
    void* result = nullptr;
    ASSERT_NE(nullptr, badbuf);
    strncpy(buf,    "hello", 255);
    strncpy(badbuf, "hello", 255);

    f = raft_apply_async(badbuf, 256, 0);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, raft_future_wait(f));
    raft_future_dispose(f);

    f = raft_apply_async(nullptr, 256, 0);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, raft_future_wait(f));
    raft_future_dispose(f);

    err = raft_apply(badbuf, 256, 0, &result);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);
    EXPECT_EQ(nullptr, result);

    err = raft_apply(nullptr, 256, 0, &result);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);
    EXPECT_EQ(nullptr, result);

    err = raft_apply(buf, 256, 0, nullptr);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);

    free_raft_buffer(buf);
    delete[] badbuf;

    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}

TEST_F(RaftFixture, NotLeaderYet) {
    char* buf = alloc_raft_buffer(256);
    raft_future f = raft_apply_async(buf, 256, 0);
    RaftError err = raft_future_wait(f);
    ASSERT_NE(RAFT_SUCCESS, err);
}

TEST_F(RaftFixture, AddRemovePeer) {
    const char localhost[] = "localhost";
    const char bogus[] = "snuffleupagus.example.com";
    raft_future f;
    RaftError err;

    wait_until_leader();

    f = raft_add_peer(bogus, 21064);
    err = raft_future_wait(f);
    EXPECT_EQ(RAFT_E_RESOLVE, err);
    raft_future_dispose(f);

    f = raft_remove_peer(bogus, 21064);
    err = raft_future_wait(f);
    EXPECT_EQ(RAFT_E_RESOLVE, err);
    raft_future_dispose(f);

    f = raft_remove_peer(localhost, 21064);
    err = raft_future_wait(f);
    ASSERT_EQ(RAFT_E_UNKNOWN_PEER, err);
    raft_future_dispose(f);

    f = raft_add_peer(localhost, 9001);
    err = raft_future_wait(f);
    ASSERT_EQ(RAFT_E_KNOWN_PEER, err);
    raft_future_dispose(f);

    f = raft_add_peer(localhost, 21064);
    err = raft_future_wait(f);
    // this is a bit problematic
    // we lose quorum if we successfully add a peer...
    ASSERT_EQ(RAFT_E_LEADERSHIP_LOST, err);
    raft_future_dispose(f);
}

TEST_F(RaftFixture, OrphanCleanup) {
    wait_until_leader();
    fsm.delay_us = std::chrono::milliseconds(50);

    const int passes = 20;
    for (int i = 0; i < passes; i++) {
        char* buf = alloc_raft_buffer(256);
        raft_future f = raft_apply_async(buf, 256, 0);
        raft_future_dispose(f);
    }
    
    EXPECT_NE(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_NE(raft::stats->call_alloc, raft::stats->call_free);
    
    std::this_thread::sleep_for((passes+20)*fsm.delay_us);

    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}
