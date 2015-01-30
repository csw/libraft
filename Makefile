GOPATH  = $(HOME)/go
GO_LIB  = github.com/csw/raft_if
GO_BIN  = $(GOPATH)/bin
GO_PROG = $(GO_BIN)/raft_if
GO_DIR  = $(GOPATH)/src/$(GO_LIB)

PATH := $(PATH):$(GO_BIN)

export GOPATH PATH

CXXFLAGS += -std=c++11 -stdlib=libc++
CXXFLAGS += -Wall -Werror -pedantic
CXXFLAGS += -Wno-variadic-macros -Wno-gnu-zero-variadic-macro-arguments
CXXFLAGS += -g -MMD

CFLAGS += -Wall -Werror -pedantic
CFLAGS += -g -MMD

makefile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
make_dir := $(dir $(makefile_path))

zlog_dir := zlog/src
zlog_lib := zlog/src/libzlog.a

gtest_dir := ../gtest-1.7.0
gtest_lib := $(gtest_dir)/make/gtest_main.a

binaries := raft_client test_suite

.PHONY: all clean test run_client

all: libraft.a $(binaries) $(GO_PROG)

run_client: raft_client $(GO_PROG)
	./raft_client --single -n 10

$(GO_PROG): $(GO_DIR)/raft_if.go $(GO_DIR)/raft_go_if.h $(GO_DIR)/raft_go_if.cc \
           libraft.a
	go clean -i $(GO_LIB)
	CGO_CPPFLAGS=-I$(make_dir) \
	CGO_LDFLAGS="-L$(make_dir) $(make_dir)/$(zlog_dir)/libzlog.a" \
		go install $(GO_LIB)

-include $(patsubst %.cc,%.d,*.cc)

libraft.a: libraft.a(raft_shm.o) libraft.a(raft_c_if.o)

raft_client: raft_client.o raft_shm.o raft_c_if.o $(zlog_lib)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^
	dsymutil $@

test.o : CPPFLAGS := $(CPPFLAGS) -I$(gtest_dir)/include

test_suite: test.o libraft.a $(gtest_lib) $(zlog_lib)
	$(CXX) $(LDFLAGS) -o $@ $^

test: test_suite
	./test_suite

$(zlog_lib):
	cd zlog/src && $(MAKE) libzlog.a

clean:
	-rm -rf *.o *.d *.a *.dSYM $(binaries)
	-go clean -i $(GO_LIB)
