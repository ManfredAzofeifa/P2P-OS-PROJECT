# Agent Guide

Read `README.md` first. It contains the project objective, current implementation state, pending components, protocol summary, and test commands.

Important constraints:

- Project implementation code must be C.
- Use POSIX sockets and `pthread`.
- Do not add external libraries.
- Work one component at a time.
- Keep `make test` passing when possible.
- The next planned component is client-side downloads: make `request <S> <H>` fetch from one candidate peer and save the file into the shared folder.
