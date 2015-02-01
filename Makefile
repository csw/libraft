makefile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
make_dir := $(dir $(makefile_path))

sources = $(wildcard *.cc)
objs := $(patsubst %.cc,%.o,$(sources))

uname := $(shell uname -s)

ifeq ($(uname),Linux)
    LIBS += -pthread
else ifeq ($(uname),Darwin)
    ifeq ($(CXX),c++)
        CXX := clang++
    endif
endif

ifeq ($(CXX),clang++)
    CC           = clang
    CXXFLAGS    += -stdlib=libc++
    CGO_LDFLAGS += -lc++
else ifeq ($(CXX),g++)
    LIBS        += -lstdc++
endif

export CC
export CXX

GOPATH  = $(HOME)/go
GO_LIB  = github.com/csw/raft_if
GO_BIN  = $(GOPATH)/bin
GO_PROG = $(GO_BIN)/raft_if
GO_DIR  = $(GOPATH)/src/$(GO_LIB)

PATH := $(PATH):$(GO_BIN)

export GOPATH PATH

CPPFLAGS += -isystem $(make_dir)/deps

CXXFLAGS += -std=c++11
CXXFLAGS += -Wall -Werror #-pedantic
CXXFLAGS += -Wno-variadic-macros -Wno-gnu-zero-variadic-macro-arguments
CXXFLAGS += -g -MMD

CFLAGS += -Wall -Werror -pedantic -std=c99
CFLAGS += -g -MMD

zlog_dir := zlog/src
zlog_lib := zlog/src/libzlog.a

gtest_dir := googletest
gtest_lib := $(gtest_dir)/make/gtest_main.a

binaries := raft_client test_suite

.PHONY: all clean libclean test run_client go go-deps

all: libraft.a $(binaries) $(GO_PROG)

run_client: raft_client $(GO_PROG)
	./raft_client --single -n 10

CGO_CPPFLAGS  = -I$(make_dir) $(CPPFLAGS)
CGO_LDFLAGS  += -L$(make_dir) -lraft
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

# ensure the submodules get checked out
$(objs): $(zlog_dir)

libraft.a: libraft.a(raft_shm.o) libraft.a(raft_c_if.o) libraft.a(stats.o)
	ranlib $@

raft_client: raft_client.o libraft.a $(zlog_lib)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test.o : CPPFLAGS := $(CPPFLAGS) -I$(gtest_dir)/include

test_suite: test.o libraft.a $(gtest_lib) $(zlog_lib)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_suite $(GO_PROG)
	./test_suite

$(zlog_dir):
	@echo "Checking out Git submodules..."
	git submodule init
	git submodule update

$(zlog_lib): $(zlog_dir)
	cd zlog/src && $(MAKE) libzlog.a

$(gtest_lib):
	cd $(gtest_dir)/make && $(MAKE) "CXXFLAGS=$(CXXFLAGS)" gtest_main.a

clean:
	-rm -rf *.o *.d *.a *.dSYM $(binaries)
	-go clean -i $(GO_LIB)

libclean: clean
	-cd zlog && $(MAKE) clean
	-cd $(gtest_dir)/make && $(MAKE) clean
