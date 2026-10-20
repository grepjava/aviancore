# wepoll

[wepoll](https://github.com/piscisaureus/wepoll) 1.5.8 by Bert Belder, the
`dist` files of tag `v1.5.8` (commit 0598a791bf9cbbf480793d778930fc635b044980),
unmodified. BSD 2-clause; `LICENSE` is its licence.

| file | sha256 |
|---|---|
| wepoll.c | b89a91fa9b3b665c6d6cfaac807ed17831202187738284eb5a08514c2b4eaa3c |
| wepoll.h | 20aa8653cea63caf6f234f278f5681288cd3c5c27aeab6961151b362bf5e47e1 |

It gives Windows epoll's interface -- `epoll_create1`, `epoll_ctl`,
`epoll_wait`, level-triggered by default, `EPOLLONESHOT` and `EPOLLRDHUP` --
by issuing `IOCTL_AFD_POLL` requests against each socket's base handle and
collecting their completions on an I/O completion port. aviancore's poller is
level-triggered epoll on Linux, so its Windows backend is the same calls.

It has no `EPOLLEXCLUSIVE`, so `av_poll_add_exclusive` is a plain add there,
as on kqueue. It polls sockets only: everything aviancore watches on Windows
is a socket, pipes included (they are loopback socket pairs there).

`../../avian_wepoll.c` includes `wepoll.c` on Windows and is empty elsewhere;
this directory is excluded from the target in `Package.swift`. To take a new
release, replace both files from the release's `dist` branch tag, update the
table, and build on Windows.
