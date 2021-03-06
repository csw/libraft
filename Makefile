makefile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
make_dir := $(dir $(makefile_path))

sources = raft_shm.cc raft_c_if.cc stats.cc config.cc
objs := $(patsubst %.cc,%.o,$(sources))

test_sources = test.cc $(wildcard *_test.cc)
test_objs := $(patsubst %.cc,%.o,$(test_sources))

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

GOPATH ?= $(HOME)/go
GO_LIB  = github.com/csw/raft_if
GO_BIN  = $(GOPATH)/bin
GO_PROG = $(GO_BIN)/raft_if
GO_DIR  = $(GOPATH)/src/$(GO_LIB)

PATH := $(PATH):$(GO_BIN)

export GOPATH PATH

CPPFLAGS += -isystem $(make_dir)/header_deps

CXXFLAGS += -std=c++11
CXXFLAGS += -Wall -Werror #-pedantic
CXXFLAGS += -Wno-variadic-macros -Wno-gnu-zero-variadic-macro-arguments
CXXFLAGS += -g -MMD

CFLAGS += -Wall -Werror -pedantic -std=c99
CFLAGS += -g -MMD

zlog_dir := $(make_dir)/zlog/src
zlog_lib := $(zlog_dir)/libzlog.a

gtest_dir := googletest
gtest_lib := $(gtest_dir)/make/gtest_main.a

jsmn_dir := $(make_dir)/zserge-jsmn
jsmn_lib := $(jsmn_dir)/libjsmn.a

local_libs := $(zlog_lib) $(jsmn_lib)

binaries := raft_client test_suite

.PHONY: all clean libclean test run_client deps go go-deps gtest zlog jsmn

all: libraft.a $(binaries) $(GO_PROG) test_suite

run_client: raft_client $(GO_PROG)
	./raft_client --single -n 10

CGO_CPPFLAGS  = -I$(make_dir) $(CPPFLAGS)
CGO_LDFLAGS  += $(LDFLAGS) -L$(make_dir) -lraft
CGO_LDFLAGS  += $(LIBS) $(local_libs)

$(GOPATH)/src:
	@echo "Go source directory not found for GOPATH=$(GOPATH)!" >&2
	@exit 1

$(GO_DIR): $(GOPATH)/src
	go get -d $(GO_LIB)

$(GO_PROG): $(GO_DIR) $(GO_DIR)/raft_if.go $(GO_DIR)/raft_go_if.h \
		$(GO_DIR)/raft_go_if.cc libraft.a $(local_libs)
	go clean -i $(GO_LIB)
	LD=$(CXX) \
	CGO_CPPFLAGS="$(CGO_CPPFLAGS)" \
	CXXFLAGS="$(CXXFLAGS)" \
	CGO_CXXFLAGS="$(CXXFLAGS)" \
	CGO_LDFLAGS="$(CGO_LDFLAGS)" \
	    go install -ldflags="-extld=$(CXX)" $(GO) $(GO_LIB)

go: $(GO_PROG)

go-deps:
	go get $(GO_LIB)

-include $(patsubst %.cc,%.d,*.cc)

# ensure the submodules get checked out
$(objs): $(zlog_dir)

lib_objs := $(patsubst %,libraft.a(%),$(objs))

libraft.a: $(lib_objs)
	ranlib $@

raft_client: raft_client.o libraft.a $(local_libs)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(test_objs) : CPPFLAGS := $(CPPFLAGS) -I$(gtest_dir)/include

test_suite: $(test_objs) libraft.a $(gtest_lib) $(local_libs)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: test_suite $(GO_PROG)
	./test_suite

$(zlog_dir):
	@echo "Checking out Git submodules..."
	git submodule init
	git submodule update

$(zlog_lib): $(zlog_dir)
	cd zlog/src && $(MAKE) libzlog.a

zlog: $(zlog_lib)

$(gtest_lib):
	cd $(gtest_dir)/make && $(MAKE) "CXXFLAGS=$(CXXFLAGS)" gtest_main.a

gtest: $(gtest_lib)

$(jsmn_lib):
	cd $(jsmn_dir) && $(MAKE)

jsmn: $(jsmn_lib)

deps: $(zlog_lib) $(gtest_lib) $(jsmn_lib)

clean:
	-rm -rf *.o *.d *.a *.dSYM $(binaries)
	-go clean -i $(GO_LIB)

libclean: clean
	-cd zlog && $(MAKE) clean
	-cd $(gtest_dir)/make && $(MAKE) clean
