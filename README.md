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
