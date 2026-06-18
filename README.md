# P2P File-Sharing Simulator

Operating Systems course project for simulating peer-to-peer file discovery and transfer on standard Linux using C, POSIX threads, and sockets.

This repository should stay in C for the project implementation. Shell scripts are allowed only as test harnesses, and the documentation may stay in LaTeX.

## Project Objective

Build a P2P file-sharing simulator where clients register local files, discover peers that own a file, and download file contents from one or more peers.

The simulator must support:

- A custom content hash implemented in this repository, without hash libraries.
- A central P2P server that stores metadata: hash, size, owner IP, and owner transfer port.
- P2P clients that register shared-folder files at startup.
- Console commands:
  - `find -s <name>` for centralized server search.
  - `find -d <name>` for distributed neighbor search.
  - `find <name>` to try server search first, then distributed search on timeout.
  - `request <S> <H>` to fetch a file by size and hash.
- File transfer by byte ranges, split across multiple peers when possible.
- Distributed flood search with TTL, query IDs, direct replies to the originator, duplicate-query dropping, and seen-query expiration.
- Resilience when a shared folder or mounted device disappears while a client is running.

## Implementation Rules

- Use C, not C++.
- Use standard Linux APIs, sockets, and `pthread`.
- Do not add external libraries.
- Keep the socket protocol line-oriented and documented in `protocol/protocol.h`.
- Keep shared protocol constants and structs in headers rather than duplicating literals across modules.
- Prefer small component tests before full integration tests.

## Current State

Implemented:

- Shared protocol constants and message contract in `protocol/protocol.h`.
- Shared endpoint and file metadata structs in `server/metadata.h`.
- Custom content hash in `server/hash.c` and `server/hash.h`.
- Threaded TCP server in `server/server.c`.
- Server-side metadata registry protected by a mutex.
- Server protocol support for:
  - `REGISTER <transfer_port> <file_count>`
  - `FILE <size> <hash> <name>`
  - `END`
  - `FIND <name>`
  - `LOOKUP <size> <hash>`
- Server returns recent active clients as neighbors during registration.
- Client startup in `client/client.c`.
- Client CLI parsing for:
  ```bash
  ./bin/client <server_ip> <server_port> <transfer_port> <shared_folder>
  ```
- Shared-folder scan for regular files.
- Client-side file size and custom-hash calculation.
- Client registration with the server.
- Client parsing and in-memory storage for returned neighbors.
- Minimal console loop in `client/console.c` with:
  - `files`
  - `neighbors`
  - `find -s <name>`
  - `request <S> <H>`
  - `quit`
  - `exit`
- Centralized client search using `FIND`, including peer response parsing and no-match reporting.
- Request lookup setup using `LOOKUP`, including candidate peer parsing and no-match reporting.
- Client download requests from one candidate peer transfer port.
- Multi-peer segmented download and reassembly when multiple `LOOKUP` candidates are available.
- Segmented downloads split the file into byte ranges, fetch ranges concurrently, retry failed ranges against remaining peers, and reassemble into `<shared_folder>/<hash>`.
- `request <S> <H>` saves successful downloads as `<shared_folder>/<hash>` because the command does not include a file name.
- Existing matching downloads are reused instead of overwritten; mismatched existing files fail the download.
- Partial download files use a `.part` suffix and are removed on transfer failure.
- Peer file serving in `client/transfer.c`.
- Client transfer listener on `<transfer_port>`.
- Peer transfer support for:
  - `GET <size> <hash> <offset> <length>`
  - `DATA <length>` followed by raw bytes
  - `ERROR <text>`
- One detached thread per incoming transfer request.
- Byte-range reads from local files by size and hash.
- Startup resilience for missing shared folders: the client warns, registers zero files, and does not crash.
- Tests for hash behavior, server protocol behavior, client registration, centralized client search, request lookup, single-peer downloads, segmented downloads, reassembly, peer failover, unavailable peers, failed transfers, and peer range serving.

Pending:

- Distributed search neighbor protocol.
- Query ID cache, TTL forwarding, duplicate dropping, and expiration.
- Full multi-client integration tests.

## Recommended Next Work

The next component should be the distributed search neighbor protocol:

1. Add client-to-client distributed search request handling using `DSEARCH`.
2. Search local shared-file metadata for matching names.
3. Return matching results to the originator using `DRESULT`.
4. Keep query ID caching, TTL forwarding, duplicate dropping, and expiration for later components.

## Socket Protocol

The protocol is documented in `protocol/protocol.h`.

Server-facing messages:

```text
REGISTER <transfer_port> <file_count>
FILE <size> <hash> <name>
END
FIND <name>
LOOKUP <size> <hash>
```

Server responses:

```text
OK <text>
ERROR <text>
PEERS <count>
PEER <ip> <port>
NEIGHBORS <count>
NEIGHBOR <ip> <port>
END
```

Peer transfer messages:

```text
GET <size> <hash> <offset> <length>
DATA <length>
<length raw bytes>
ERROR <text>
```

Distributed-search messages are reserved in the protocol header and should be implemented in later components.

## Build

Build the server:

```bash
make server
```

Build every currently implemented binary:

```bash
make
```

At this point, `make` builds both the server and the client.

Clean generated binaries:

```bash
make clean
```

## Tests

Run all current tests:

```bash
make test
```

Current test targets:

- `make test-hash`
  - Builds `tests/test_hash.c` with `server/hash.c`.
  - Verifies same file contents produce the same hash even with different names.
  - Verifies different contents produce a different hash.
- `make test-server`
  - Builds `bin/server`.
  - Starts the server on localhost.
  - Registers one file through the socket protocol.
  - Verifies `FIND` returns the registering peer.
  - Verifies `LOOKUP` returns the registering peer by size and hash.
- `make test-client`
  - Builds `bin/server` and `bin/client`.
  - Starts the server on localhost.
  - Runs the client against a temporary shared folder.
  - Verifies the client registers two files.
  - Sends `find -s` through the client console and verifies a matching peer is printed.
  - Sends `find -s` for a missing file and verifies no-match reporting.
  - Sends `request <S> <H>` and verifies candidate peers are printed.
  - Verifies `request <S> <H>` downloads from one candidate peer into `<shared_folder>/<hash>`.
  - Verifies an existing matching downloaded file is reused instead of overwritten.
  - Verifies segmented downloads fetch ranges from multiple peers and reassemble the original file.
  - Verifies a failed segmented peer can be retried against an available peer.
  - Sends `request` for missing metadata and verifies no-match reporting.
  - Verifies unavailable peers and failed transfers report failure without leaving output or `.part` files.
  - Sends `GET` directly to the client's transfer port and verifies a byte range is returned.
  - Verifies the server can find one of those files afterward.
  - Verifies a missing shared folder does not crash the client and registers zero files.

The server test binds a local TCP port. If a sandbox blocks listening sockets, run it in a normal terminal or allow the test command to bind localhost.

## Repository Layout

```text
.
├── client/        # Client startup, console, and transfer code
├── distributed/   # Flood search, neighbors, TTL, and seen-query tracking
├── docs/          # Course documentation in LaTeX
├── protocol/      # Shared socket protocol constants and structs
├── server/        # P2P server, metadata, and custom hash
└── tests/         # C tests and shell harnesses
```

## Notes For Future Agents

- Start by reading this file, `protocol/protocol.h`, and `Makefile`.
- Check `git status --short` before editing.
- Work one component at a time.
- Keep behavior covered by `make test` as each component becomes available.
- Do not replace C code with C++ or external dependencies.
- Do not remove existing user changes unless explicitly asked.
