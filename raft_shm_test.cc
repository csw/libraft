#include "raft_shm.h"
#include "gtest/gtest.h"

TEST(RaftLib, TagNames) {
    EXPECT_STREQ("Apply", raft::tag_name(raft::CallTag::Apply));
    EXPECT_STREQ("FSMApply", raft::tag_name(raft::CallTag::FSMApply));
    EXPECT_STREQ("*invalid*", raft::tag_name((raft::CallTag) 255));
}
