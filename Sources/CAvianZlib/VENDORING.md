# zlib

[zlib](https://zlib.net) 1.3.2 by Jean-loup Gailly and Mark Adler, from
`zlib-1.3.2.tar.gz` (SHA-256
`bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`, as zlib.net
publishes it; the GitHub release asset is the same file). zlib licence:
`LICENSE`.

Only the library proper: deflate, inflate, CRC-32, Adler-32 and their
helpers. The gzip file functions (gz*.c) and infback.c are left out; nothing in
aviancore reads or writes .gz files. `gzguts.h` stays because `zutil.c`
includes it. `zlib.h` and `zconf.h` are under `include/`, unmodified.

This target is a dependency of CAvian on Windows only, where there is no
system zlib. Linux and macOS go on linking the system's libz. To take a new
release, replace the same files from its tarball, check its hash against
zlib.net, and update this note.
