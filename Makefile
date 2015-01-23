GOPATH  = $(HOME)/go
GO_LIB  = github.com/csw/raft_if
GO_BIN  = $(GOPATH)/bin
GO_PROG = $(GO_BIN)/raft_if
GO_DIR  = $(GOPATH)/src/$(GO_LIB)

PATH := $(PATH):$(GO_BIN)

CXXFLAGS += -std=c++11 -stdlib=libc++
CXXFLAGS += -Wall -Werror -pedantic
CXXFLAGS += -g -MMD

makefile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
make_dir := $(dir $(makefile_path))

.PHONY: all clean run_client

all: libraft.a raft_client $(GO_PROG)

run_client: raft_client $(GO_PROG)
	rm /tmp/boost_interprocess/raft
	./raft_client

$(GO_PROG): $(GO_DIR)/raft_if.go $(GO_DIR)/raft_go_if.h $(GO_DIR)/raft_go_if.cc \
           libraft.a
	CGO_CPPFLAGS=-I$(make_dir) CGO_LDFLAGS=-L$(make_dir) \
		go install $(GO_LIB)

-include $(patsubst %.cc,%.d,*.cc)

libraft.a: libraft.a(raft_shm.o) libraft.a(raft_c_if.o)

raft_client: raft_client.o raft_shm.o raft_c_if.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

clean:
	-rm -f *.o *.d *.a
