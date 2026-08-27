# splice()/sendfile() shared-`f_pos` data corruption — working notes

Investigation notes and evidence for the `->f_pos` race in `do_splice()`.
This is a notes-only orphan branch: it carries no kernel source. The fix
itself lives on `claude/splice-fpos-race` as a single commit.

Base for all of this: `v7.2-14827` (`45c13f3f9e3b`).

## Layout

| Path | What it is |
|---|---|
| `LEARNING.md` | Tutorial: what the patch does, with the VFS background |
| `REPORT.txt` | The linux-fsdevel posting, ready to send |
| `patches/0001-splice-serialize-f_pos.patch` | The fix, as formatted for submission |
| `patches/rejected-naive-f_pos_lock.diff` | The first attempt — **deadlocks**, kept as a record |
| `repro/splice_race.c` | Corruption reproducer (splice), deterministic |
| `repro/sendfile_race.c` | Corruption reproducer (sendfile), timing-sensitive |
| `repro/splice_deadlock.c` | Liveness reproducer — catches the naive patch's hang |
| `PRIOR-ART.md` | The 2014 decision, with quotes, and the lore search coverage |
| `testbed/` | QEMU harness: guest init script, runner, config fragment |
| `logs/` | Raw guest output for each of the three kernels |

## Prior art — read this first

**This was decided deliberately in 2014, not overlooked.** Al Viro named
`splice` and `sendfile` as exactly the two syscalls left outside `f_pos_lock`;
Linus said "decided we don't care" because they are outside POSIX. The
decision was explicitly provisional and the POSIX question it hinged on was
never answered. See `PRIOR-ART.md` — it also covers why the premise has
expired for splice specifically (coreutils `cat` now uses splice, so nobody
in a shell pipeline opts in), and why the fix shape suggested in 2014
deadlocks here.

## The finding in one paragraph

`do_splice()` reads a shared `struct file`'s `->f_pos`, uses it for the
transfer and writes it back, with no locking. `read(2)`/`write(2)`/
`lseek(2)` all take `->f_pos_lock` for this via `fdget_pos()`. So a
`splice()` racing a `write()` on two fds sharing one open file
description both read the same stale offset and clobber each other. The
exposure is not a narrow window: `do_splice()` latches the offset and
then *sleeps* in `pipe_wait_readable()`, so a `cat`-style splice loop
holds a stale offset nearly all the time. It corrupts on every run.

## The trap

Taking `->f_pos_lock` around the existing sequence — the obvious fix —
deadlocks, because the lock is then held across that same sleep. The
task feeding the pipe is very often the same task sharing the file
offset (any `cmd1 | cmd2 > file 2>&1` pipeline), so it blocks in
`write()` and never produces the data the splicer is waiting for.

**lockdep does not catch this.** The cycle closes through userspace, not
through two kernel locks. Only `CONFIG_DETECT_HUNG_TASK` reports it. Any
future attempt here needs the liveness reproducer, not just a lockdep
stress run.

The fix waits on the pipe *before* taking the lock (reusing the existing
`ipipe_prep()`/`opipe_prep()`) and makes the transfer itself
pipe-non-blocking, so only the file write happens under the lock — which
is what `write(2)` already does.

## Results

Three kernels, same config, each run against tmpfs and ext4:

| kernel | splice corruption | sendfile | liveness |
|---|---|---|---|
| unpatched | 10/10 runs corrupt | corrupt | completes |
| naive `f_pos_lock` | 0/10 corrupt | corrupt | **hangs** |
| this fix | 0/10 corrupt | corrupt | completes |

`sendfile` still corrupting under the fix is intentional — `do_sendfile()`
has its own independent copy of the pattern and is out of scope. See
`REPORT.txt` for why the fix does not transfer to it.

## Reproducing

```sh
# host: build the three static test binaries into an initramfs, then
gcc -O2 -static -no-pie -o splice_race     repro/splice_race.c
gcc -O2 -static -no-pie -o sendfile_race   repro/sendfile_race.c
gcc -O2 -static -no-pie -o splice_deadlock repro/splice_deadlock.c
```

All three honour `$TMPDIR`, so the same binary exercises whichever
filesystem you point it at. `testbed/runtest.sh` boots a given `bzImage`
under QEMU with `testbed/init.sh` as `rdinit` and runs the full matrix;
`testbed/config-fragment` lists the config options that matter (notably
`CONFIG_DETECT_HUNG_TASK`, without which the deadlock is silent).

## Open

- `do_sendfile()` and `copy_file_range(2)` still carry the bug; the fix does
  not transfer to them (see `REPORT.txt`).
- Whether the Austin Group ever responded in 2014 — no follow-up found on-list.
- Only tmpfs and ext4, x86_64 only, no xfstests run.
- Sasha Levin's `kernel/api` sys_read/sys_write spec series may state the
  `f_pos` atomicity guarantee normatively; not yet read.
