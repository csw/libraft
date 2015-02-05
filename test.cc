// mkdtemp(3) please
#ifdef __linux
#  define _POSIX_C_SOURCE 200809L
#endif

#include <cassert>
#include <chrono>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "gtest/gtest.h"

#include "raft_shm.h"
#include "raft_if.h"
#include "stats.h"

using std::string;

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
            ++restores;
            return 0;
        } else {
            return 1;
        }
    }

    int write_snapshot(raft_fsm_snapshot_handle handle, FILE* sink)
    {
        auto state = std::unique_ptr<uint32_t>((uint32_t*) handle);
        std::this_thread::sleep_for(delay_us);
        int chars = fprintf(sink, "%u\n", *state);
        if (chars > 0) {
            ++snapshots;
            return 0;
        } else {
            perror("Writing snapshot failed");
            return 1;
        }
    }

    uint32_t count = 0;
    uint32_t snapshots = 0;
    uint32_t restores = 0;
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

class RaftFixtureBase {
public:
    RaftFixtureBase()
        : running(false)
    {
        fsm_instance = &fsm;
        raft_default_config(&config);
        config.EnableSingleNode = true;
    }

    ~RaftFixtureBase()
    {
        if (running)
            stop();
    }

    void start()
    {
        raft_init(&fsm_rec, &config);
        running = true;
    }

    void stop()
    {
        //fprintf(stderr, "RaftFixture::stop()\n");
        assert(running);
        raft_future sf = raft_shutdown();
        raft_future_wait(sf);
        raft_cleanup();
        running = false;
    }

    bool       running;
    DummyFSM   fsm;
    RaftConfig config;
    RaftFSM    fsm_rec = { &FSMApply, &FSMBeginSnapshot, &FSMRestore };
};

class RaftFixture : public ::testing::Test, public RaftFixtureBase {};

void wait_until_leader()
{
    while (! raft_is_leader()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void run_simple_op()
{
    char* buf = alloc_raft_buffer(256);
    strncpy(buf, "Raft test suite op my spoon is too big", 256);
    raft_future f = raft_apply(buf, 256, 0);
    RaftError err = raft_future_wait(f);
    ASSERT_EQ(RAFT_SUCCESS, err);
    free_raft_buffer(buf);
    raft_future_dispose(f);
}

//const static time_t RECENT = 1423090205;

TEST_F(RaftFixture, Simple) {
    start();
    wait_until_leader();
    EXPECT_EQ(RAFT_LEADER, raft_state());
    char *leader = nullptr;
    RaftError err = raft_leader(&leader);
    EXPECT_EQ(RAFT_SUCCESS, err);
    if (leader)
        EXPECT_STREQ("127.0.0.1:9001", leader);

    // XXX: getting 3 seconds past the epoch... :p
    EXPECT_LT(raft_last_contact(), 86400);
    // XXX: this would work in a functioning cluster, I guess...

    // time_t now = time(nullptr);
    // time_t last_contact = raft_last_contact();
    // printf("Last contact at: (%ld) %s",
    //        last_contact, ctime(&last_contact));
    // EXPECT_GT(last_contact, RECENT);
    // int64_t delta = abs(now - last_contact);
    // EXPECT_LT(delta, 20);

    const uint32_t ops = 5;
    const auto start_idx = raft_last_index();
    for (uint32_t i = 0; i < ops; i++) {
        EXPECT_EQ(i, fsm.count);
        run_simple_op();
    }
    EXPECT_EQ(ops, fsm.count);
    EXPECT_GE(raft_last_index(), start_idx + ops);

    ASSERT_EQ(0, fsm.snapshots);
    raft_future f = raft_snapshot();
    err = raft_future_wait(f);
    EXPECT_EQ(RAFT_SUCCESS, err);
    EXPECT_EQ(true, raft_future_poll(f));
    EXPECT_EQ(true, raft_future_wait_for(f, 0));
    EXPECT_EQ(true, raft_future_wait_for(f, 5));
    EXPECT_EQ(1, fsm.snapshots);
    raft_future_dispose(f);

    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}

TEST_F(RaftFixture, DoubleWait) {
    start();
    wait_until_leader();

    char* buf = alloc_raft_buffer(256);
    strncpy(buf, "Raft test suite op my spoon is too big", 256);
    raft_future f = raft_apply(buf, 256, 0);
    RaftError err = raft_future_wait(f);
    ASSERT_EQ(RAFT_SUCCESS, err);
    ASSERT_EQ(RAFT_SUCCESS, raft_future_wait(f));
    free_raft_buffer(buf);
    raft_future_dispose(f);
}

TEST_F(RaftFixture, NoCluster) {
    config.EnableSingleNode = false;
    start();
    EXPECT_EQ(RAFT_FOLLOWER, raft_state());
}

TEST_F(RaftFixture, OnClose) {
    start();
    wait_until_leader();

    raft_future f;
    void* ptr;
    char* buf = alloc_raft_buffer(256);
    strncpy(buf, "oops", 255);
    raft::scoreboard->api_queue.close();

    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_apply_sync(buf, 256, 0, &ptr));
    f = raft_apply(buf, 256, 0);
    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_future_wait(f));
    f = raft_barrier(0);
    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_future_wait(f));
    f = raft_verify_leader();
    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_future_wait(f));
    f = raft_snapshot();
    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_future_wait(f));
    f = raft_shutdown();
    EXPECT_EQ(RAFT_E_IN_SHUTDOWN, raft_future_wait(f));

    // because the API queue is shut down, we can't send a normal
    // shutdown request...
    int res = raft::kill_raft_();
    EXPECT_EQ(0, res);
}

TEST(RaftProcs, KillRaftDeathTest) {
    ASSERT_DEATH({
            RaftFixtureBase f;
            f.start();
            wait_until_leader();
            raft::kill_raft_();
            sleep(5);
        },
        "exited");
}

TEST(RaftProcs, BadSHMPathDeathTest) {
    ASSERT_DEATH({
            RaftFixtureBase f;
            strncpy(f.config.shm_path, "/dev/whatever", 255);
            f.start();
        },
        "shared memory");
}

TEST_F(RaftFixture, Restore) {
    char raft_dir[256];
    const char* tmpdir = getenv("TMPDIR");
    snprintf(raft_dir, 255, "%s/raft_test_XXXXXX",
             tmpdir ? tmpdir : "/tmp");
    if (!mkdtemp(raft_dir)) {
        perror("failed to create temp dir");
        exit(1);
    }
    strncpy(config.base_dir, raft_dir, 255);

    start();
    wait_until_leader();

    const uint32_t ops = 5;
    for (uint32_t i = 0; i < ops; i++) {
        EXPECT_EQ(i, fsm.count);
        run_simple_op();
    }
    EXPECT_EQ(ops, fsm.count);

    raft_future f = raft_snapshot();
    RaftError err = raft_future_wait(f);
    EXPECT_EQ(err, RAFT_SUCCESS);
    EXPECT_EQ(1, fsm.snapshots);
    raft_future_dispose(f);

    stop();
    fsm = DummyFSM();
    start();
    wait_until_leader();

    EXPECT_EQ(ops, fsm.count);
    EXPECT_EQ(1, fsm.restores);
}

TEST_F(RaftFixture, BadApply) {
    start();
    wait_until_leader();

    raft_future f;
    RaftError err;
    char* buf = alloc_raft_buffer(256);
    char* badbuf = new char[256];
    void* result = nullptr;
    ASSERT_NE(nullptr, badbuf);
    strncpy(buf,    "hello", 255);
    strncpy(badbuf, "hello", 255);

    f = raft_apply(badbuf, 256, 0);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, raft_future_wait(f));
    raft_future_dispose(f);

    f = raft_apply(nullptr, 256, 0);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, raft_future_wait(f));
    raft_future_dispose(f);

    err = raft_apply_sync(badbuf, 256, 0, &result);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);
    EXPECT_EQ(nullptr, result);

    err = raft_apply_sync(nullptr, 256, 0, &result);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);
    EXPECT_EQ(nullptr, result);

    err = raft_apply_sync(buf, 256, 0, nullptr);
    EXPECT_EQ(RAFT_E_INVALID_ADDRESS, err);

    free_raft_buffer(buf);
    delete[] badbuf;

    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}

TEST_F(RaftFixture, NotLeaderYet) {
    start();
    char* buf = alloc_raft_buffer(256);
    raft_future f = raft_apply(buf, 256, 0);
    RaftError err = raft_future_wait(f);
    ASSERT_NE(RAFT_SUCCESS, err);
}

TEST_F(RaftFixture, AddRemovePeer) {
    const char localhost[] = "localhost";
    const char bogus[] = "snuffleupagus.example.com";
    raft_future f;
    RaftError err;

    start();
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
    start();
    wait_until_leader();
    fsm.delay_us = std::chrono::milliseconds(50);

    const int passes = 20;
    for (int i = 0; i < passes; i++) {
        char* buf = alloc_raft_buffer(256);
        raft_future f = raft_apply(buf, 256, 0);
        raft_future_dispose(f);
    }

    char* buf = alloc_raft_buffer(256);
    raft_future f = raft_apply(buf, 256, 0);
    EXPECT_EQ(RAFT_SUCCESS, raft_future_wait(f));
    raft_future_dispose(f);
    free_raft_buffer(buf);
    
    EXPECT_NE(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_NE(raft::stats->call_alloc, raft::stats->call_free);
    
    std::this_thread::sleep_for((passes+20)*fsm.delay_us);

    EXPECT_EQ(raft::stats->buffer_alloc, raft::stats->buffer_free);
    EXPECT_EQ(raft::stats->call_alloc, raft::stats->call_free);
}
