makefile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
make_dir := $(dir $(makefile_path))

uname := $(shell uname -s)
clang_pp := $(shell which clang++)

ifdef clang_pp
    CC  = clang
    CXX = clang++
    export CC
    export CXX
endif

GOPATH  = $(HOME)/go
GO_LIB  = github.com/csw/raft_if
GO_BIN  = $(GOPATH)/bin
GO_PROG = $(GO_BIN)/raft_if
GO_DIR  = $(GOPATH)/src/$(GO_LIB)

PATH := $(PATH):$(GO_BIN)

export GOPATH PATH

CPPFLAGS += -isystem $(make_dir)/deps

CXXFLAGS += -std=c++11 -stdlib=libc++
CXXFLAGS += -Wall -Werror -pedantic
CXXFLAGS += -Wno-variadic-macros -Wno-gnu-zero-variadic-macro-arguments
CXXFLAGS += -g -MMD

CFLAGS += -Wall -Werror -pedantic -std=c99
CFLAGS += -g -MMD

ifeq ($(uname),Linux)
    LIBS += -pthread -lstdc++
endif

zlog_dir := zlog/src
zlog_lib := zlog/src/libzlog.a

gtest_dir := googletest
gtest_lib := $(gtest_dir)/make/gtest_main.a

binaries := raft_client test_suite

.PHONY: all clean libclean test run_client go go-deps

all: libraft.a $(binaries) $(GO_PROG)

run_client: raft_client $(GO_PROG)
	./raft_client --single -n 10

CGO_CPPFLAGS  = -I$(make_dir)
CGO_CPPFLAGS += -isystem $(make_dir)/deps
CGO_LDFLAGS   = -L$(make_dir) -lraft
# a Clang-ism, revisit for gcc
CGO_LDFLAGS  += -lc++
CGO_LDFLAGS  += $(make_dir)/$(zlog_dir)/libzlog.a $(LIBS)

$(GO_PROG): $(GO_DIR)/raft_if.go $(GO_DIR)/raft_go_if.h \
		$(GO_DIR)/raft_go_if.cc libraft.a
	go clean -i $(GO_LIB)
	CGO_CPPFLAGS="$(CGO_CPPFLAGS)" \
	CXXFLAGS="$(CXXFLAGS)" \
	CGO_CXXFLAGS="$(CXXFLAGS)" \
	CGO_LDFLAGS="$(CGO_LDFLAGS)" \
	  go install $(GO_LIB)

go: $(GO_PROG)

go-deps:
	go get $(GO_LIB)

-include $(patsubst %.cc,%.d,*.cc)

libraft.a: libraft.a(raft_shm.o) libraft.a(raft_c_if.o) libraft.a(stats.o)
	ranlib $@

raft_client: raft_client.o libraft.a $(zlog_lib)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test.o : CPPFLAGS := $(CPPFLAGS) -I$(gtest_dir)/include

test_suite: test.o libraft.a $(gtest_lib) $(zlog_lib)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_suite
	./test_suite

$(zlog_lib):
	cd zlog/src && $(MAKE) libzlog.a

$(gtest_lib):
	cd $(gtest_dir)/make && $(MAKE) "CXXFLAGS=$(CXXFLAGS)"

clean:
	-rm -rf *.o *.d *.a *.dSYM $(binaries)
	-go clean -i $(GO_LIB)

libclean: clean
	-cd zlog && $(MAKE) clean
	-cd $(gtest_dir)/make && $(MAKE) clean
