# libraft

This library provides a C interface to Hashicorp's [Go][]
[implementation][hashicorp/raft] of the [Raft][] consensus algorithm
for distributed systems. The other component of the interface is the
Go package [raft_if][], which links against this library and is built
by this Makefile.

**NOTE**: libraft is still very developmental, and should not be used in
production at this point. The API will certainly be undergoing
significant change, and the project could perhaps use a better name as
well.

[Go]: http://golang.org/
[hashicorp/raft]: https://github.com/hashicorp/raft
[Raft]: https://ramcloud.stanford.edu/wiki/download/attachments/11370504/raft.pdf
[raft_if]: https://github.com/csw/raft_if

## Rationale

Production-grade Raft implementations usable outside Go or the JVM
seem to be in short supply. This presents Raft as a C library with a
simple API, to bridge that gap.

Internally, the Raft implementation runs in a child process,
communicating via shared memory to minimize memory copies and
overhead.

## Requirements

 - C++11 compiler; tested with Clang 3.5 and GCC 4.8
 - Go 1.4 (possibly other versions)
 - [Boost](http://www.boost.org/)
 - Linux or Mac OS X; not tested on other Unix platforms

## Installation

If you don't have a Go [workspace](http://golang.org/doc/code.html)
set up, do that first. Then set the `GOPATH` environment variable to
point to it, and add `$GOPATH/bin` to your `PATH`.

Then simply run `make` to check out the libraft submodules and
[raft_if][] and build all of it.

Run a simple demo program with `make run_client`, or the tests with
`make test`.

## Usage

The API is in
[raft_c_if.h](https://github.com/csw/libraft/blob/master/raft_c_if.h)
and
[raft_defs.h](https://github.com/csw/libraft/blob/master/raft_defs.h). It's
meant to mirror the
[Go API](http://godoc.org/github.com/hashicorp/raft) for the
underlying implementation, which doesn't exactly excuse the lack of
documentation... Again, the API is very much a work in progress.

[raft_client.c](https://github.com/csw/libraft/blob/master/raft_client.c)
is a fairly minimal example of a client program, providing a simple
finite state machine that counts letters and a driver that
communicates with it via Raft.

Standalone, non-persistent interactive use:

    $ ./raft_client --single -i

And with persistent state:

    $ ./raft_client --dir /tmp/raft -i

Or, run an actual Raft cluster:

    ./raft_client --dir /tmp/raft_A --port 9001 --peers localhost:9002,localhost:9003 -i
    ./raft_client --dir /tmp/raft_B --port 9002 --peers localhost:9003,localhost:9001 -i
    ./raft_client --dir /tmp/raft_C --port 9003 --peers localhost:9001,localhost:9002 -i
