CXXFLAGS += -std=c++11 -stdlib=libc++
CXXFLAGS += -Wall -Werror -pedantic
CXXFLAGS += -g -MMD

.PHONY: all clean

-include $(patsubst %.cc,%.d,*.cc)

all: libraft.a raft_client

libraft.a: libraft.a(raft_shm.o) libraft.a(raft_c_if.o)

raft_client: raft_client.o raft_shm.o raft_c_if.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

clean:
	-rm -f *.o *.d *.a
