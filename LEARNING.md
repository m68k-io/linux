# Understanding the splice() `f_pos` patch

A walk through what the patch changes and the VFS background you need to
read it. Assumes you can read C and know roughly what a file descriptor is,
but not that you know the VFS internals.

Everything here refers to `fs/splice.c` in a v7.2 tree, with the patch on
branch `claude/splice-fpos-race`.

> The narrative of how this was originally found (starting from a ksh bug
> report) is preserved separately in `INVESTIGATION-LOG.md`. Note that its
> "Part 7 — The fix" describes an **earlier** patch that turned out to
> deadlock; this document describes the one that replaced it.

---

## 1. The one-paragraph version

`splice()` moves data between a pipe and a file. When you don't pass it an
explicit offset, it reads the file's current position, transfers some bytes,
and writes the position back — and it does all of that **without taking the
lock that `read()`, `write()` and `lseek()` use for exactly the same field**.
So a `splice()` and a `write()` on two file descriptors that share one open
file can both decide to start at byte 95400, both write there, and one
silently destroys the other's data. The patch makes `splice()` take that
lock — but it can't do so naively, because the obvious placement deadlocks.

To understand why, you need four pieces of background.

---

## 2. Background: descriptors vs. *open file descriptions*

This is the concept everything else rests on. There are two distinct objects:

- A **file descriptor** is the small integer, private to a process. `int fd = 3`.
- An **open file description** is the kernel object behind it: `struct file`.
  It holds the current file position, the open flags, the file mode.

The mapping is many-to-one. Several descriptors — in the *same* process or in
*different* processes — can point at one `struct file`:

```
  process A            process B
  ┌────────┐           ┌────────┐
  │ fd 1 ──┼───┐   ┌───┼── fd 1 │
  │ fd 2 ──┼───┤   │   │ fd 2   │
  └────────┘   │   │   └────────┘
               ▼   ▼
        ┌──────────────────┐
        │   struct file    │
        │   f_pos = 95400  │   ← ONE shared position
        │   f_pos_lock     │
        └──────────────────┘
```

Two ways to get there:

- **`dup()` / `dup2()`** — including the shell's `2>&1`, which is literally
  "make fd 2 a dup of fd 1".
- **`fork()`** — the child inherits the parent's descriptors, and they keep
  pointing at the *same* `struct file`, not a copy.

Contrast with two independent `open()` calls on the same path: those produce
two separate `struct file`s with two independent positions, and none of this
applies.

So when a shell runs

```sh
cmd1 | cmd2 > file 2>&1
```

`cmd1`'s fd 2 and `cmd2`'s fd 1 are descriptors in *different processes*
pointing at *one* `struct file`, sharing *one* `f_pos`. That's the whole
setup for the bug. Nothing exotic — it's how shell redirection has always
worked.

---

## 3. Background: `f_pos` and the POSIX rule

`f_pos` is just a `loff_t` inside `struct file` ([include/linux/fs.h][fs_h]):

```c
union {
        /* regular files (with FMODE_ATOMIC_POS) and directories */
        struct mutex            f_pos_lock;
        /* pipes */
        u64                     f_pipe;
};
loff_t                          f_pos;
```

Two things to notice right away:

1. There's a dedicated mutex, `f_pos_lock`, sitting right next to it.
2. **It's in a union with `f_pipe`.** For a pipe, that memory is *not* a
   mutex — it's a `u64`. Calling `mutex_lock(&file->f_pos_lock)` on a pipe
   would corrupt it. Remember this; it comes back in §8.

POSIX.1-2008 §XSI 2.9.7 requires that `read()`, `write()`, `lseek()` and
friends be **atomic with respect to each other** on regular files, and the
file-position update is explicitly one of the effects covered. Two processes
sharing a position may have their writes *interleave* in any order, but a
write must never land on top of another's bytes.

Linux didn't honour this until 2014, when Michael Kerrisk reported it and
Linus added `f_pos_lock` in *"vfs: atomic f_pos accesses as per POSIX"*.

---

## 4. Background: how `read`/`write` get it right

Every syscall that uses the implicit position goes through `fdget_pos()`
instead of plain `fdget()` ([fs/file.c][file_c]):

```c
struct fd fdget_pos(unsigned int fd)
{
        struct fd f = fdget(fd);
        struct file *file = fd_file(f);

        if (likely(file) && file_needs_f_pos_lock(file)) {
                f.word |= FDPUT_POS_UNLOCK;
                mutex_lock(&file->f_pos_lock);
        }
        return f;
}
```

The lock is held for the whole syscall and dropped by `fdput_pos()`. So
`write()` is: *lock → read `f_pos` → write bytes → advance `f_pos` → unlock*.
Atomic, as required.

Crucially the lock is **conditional**, via `file_needs_f_pos_lock()`:

```c
static inline bool file_needs_f_pos_lock(struct file *file)
{
        if (!(file->f_mode & FMODE_ATOMIC_POS))
                return false;          /* not a seekable/regular file */
        if (__file_ref_read_raw(&file->f_ref) != FILE_REF_ONEREF)
                return true;           /* shared: dup()'d or fork()'d */
        if (file->f_op->iterate_shared)
                return true;           /* directory */
        return false;
}
```

Read that as: *"only bother if this is a regular file **and** somebody else
can actually see it."* The overwhelmingly common case — one process, one
descriptor — takes no lock at all. That conditionality matters both for
performance and, as you'll see, for safety around that `f_pipe` union.

---

## 5. Background: what `splice()` actually is

`splice()` moves data between two descriptors without copying through
userspace. **At least one end must be a pipe** — that's the design
constraint, because the pipe's buffers are the vehicle.

```c
ssize_t splice(int fd_in,  loff_t *off_in,
               int fd_out, loff_t *off_out,
               size_t len, unsigned int flags);
```

The offset arguments are the part that matters here:

- Pass a non-NULL `off_out` → splice uses *that* offset and leaves `f_pos`
  alone. Like `pwrite()`. **No sharing problem.**
- Pass **`NULL`** → splice uses and updates the file's `f_pos`. Like
  `write()`. **This is the path with the bug.**

You might think "who calls splice with a NULL offset?" Answer: `cat`.

```
$ echo hi | strace -e trace=splice cat > file
splice(0, NULL, 4, NULL, 524288, 0)     = 3
splice(3, NULL, 1, NULL, 524288, 0)     = 3
                    ^^^^
```

GNU coreutils `cat` uses splice for pipe-to-file copies, with a NULL offset.
So `cmd | cat > file` reaches this code path with nobody having chosen splice
deliberately.

`do_splice()` has three branches, dispatched on which end is a pipe. We care
about two:

| branch | direction | which `f_pos` is used |
|---|---|---|
| `ipipe && opipe` | pipe → pipe | none (pipes have no position) |
| **`ipipe`** | **pipe → file** | **`out->f_pos`** |
| **`opipe`** | **file → pipe** | **`in->f_pos`** |

Since one end is always a pipe, **at most one** `f_pos` is ever in play per
call. That's why the patch only ever needs a single lock.

---

## 6. The bug

Here is the original `ipipe` branch, stripped to the essentials:

```c
offset = out->f_pos;                                   /* (1) READ    */
ret = do_splice_from(ipipe, out, &offset, len, flags); /* (2) TRANSFER */
if (!off_out)
        out->f_pos = offset;                           /* (3) WRITE BACK */
```

Read-modify-write on a field shared across processes, with no lock. Meanwhile
a concurrent `write()` on a sibling descriptor is doing its own
lock-protected read-modify-write on the *same* field. One participant honours
the lock; the other doesn't. So the lock protects nothing.

### 6.1 Why the window is enormous

This is the part that surprises people, and it's why the reproducer isn't
flaky.

You'd assume the race window is the handful of instructions between (1) and
(3). It isn't. Step (2) descends into `iter_file_splice_write()` →
`splice_from_pipe_next()`, which does:

```c
while (pipe_is_empty(pipe)) {
        ...
        pipe_wait_readable(pipe);      /* ← SLEEPS HERE */
}
```

The offset was latched at (1), and now the thread **goes to sleep** holding
that stale value, waiting for the pipe's writer to produce data. A
`cat`-style splice loop spends nearly all of its wall-clock time parked right
there.

So the window isn't nanoseconds. It's *"for as long as the pipe happens to be
empty"* — often milliseconds or seconds. That's why the reproducer corrupts
**10 runs out of 10**, on both tmpfs and ext4.

### 6.2 What the damage looks like

Both parties compute the same starting offset and both write there:

```
splicer (fd A)                        writer (fd B)              f_pos
─────────────────────────────────────────────────────────────────────
splice(): offset = f_pos  ────────────────────────────────────►  95400
   sleeps in pipe_wait_readable()
   (holding offset = 95400)
                                      write(): fdget_pos()
                                        takes f_pos_lock
                                        reads f_pos ──────────►  95400
                                                                  ^^^^^
                                                          same offset!
   pipe fills, writes 12B @ 95400
                                        writes 5B @ 95400
                                        ── clobbers the first
                                           5 bytes just written
```

The measured result, from `splice_race.c`:

```
expected size 96012, got 96007          (5 bytes gone)
...abcdefghijk\ndone\nfghijk\nabcdefghijk\n...
                     ^^^^^^ ^^^^^^^
                     the 5-byte write  the surviving tail of the
                     landed here       record it overwrote
7999 intact records instead of 8000
```

Note the distinction that matters: **interleaving would be fine.** POSIX
allows `done\n` to appear in the middle of the file. What's forbidden is
bytes *disappearing*. The file getting shorter is the bug.

---

## 7. The trap: why the obvious fix deadlocks

The natural first patch — and the one Linus sketched back in 2014 — is to
wrap the sequence in the lock:

```c
mutex_lock(&out->f_pos_lock);          /* ← naive */
offset = out->f_pos;
ret = do_splice_from(ipipe, out, &offset, len, flags);
out->f_pos = offset;
mutex_unlock(&out->f_pos_lock);
```

This fixes the corruption. It also hangs your shell.

Look again at §6.1: the transfer **sleeps on the pipe**. Under this patch it
sleeps *holding `f_pos_lock`*. And who is the pipe's writer? In our exact
motivating case, it's the same process that shares the file position:

```
cat  (the splicer)                      foo  (feeds pipe AND shares fd 2)
──────────────────────────────────────────────────────────────────────────
splice()
  mutex_lock(&out->f_pos_lock)   ← acquired
  offset = out->f_pos
  do_splice_from()
    pipe empty →
    pipe_wait_readable()  ▸▸ SLEEP, still holding the lock
                                        print -u2 "done"
                                          write() → fdget_pos()
                                            mutex_lock(f_pos_lock)
                                            ▸▸ BLOCKS (uninterruptible)

                                        never reaches its next
                                        write to the pipe...
  ...so no data ever arrives, so cat never wakes,
     so the lock is never released
                       ══ DEADLOCK ══
```

Confirmed on a booted kernel carrying that patch:

```
INFO: task splice_deadlock:301 blocked for more than 20 seconds.
INFO: task splice_deadlock:301 is blocked on a mutex likely owned
      by task splice_deadlock:304.

locks held by splice_deadlock/301: 1, on CPU#0:
 #0: ffff9447c235d5f0 (&f->f_pos_lock), at: fdget_pos+0x7f/0xc0
locks held by splice_deadlock/304: 2, on CPU#3:
 #0: ffff9447c235d5f0 (&f->f_pos_lock), at: do_splice+0x96c/0xc40
                                             ^^^^^^^^^^^^^^^^^^^^
      same lock address — one waiting in write(), one holding it in splice()
```

Two details worth internalising:

- The waiter is in **`D` state** (`mutex_lock` is uninterruptible), so you
  can't even `SIGKILL` it. You have to kill the splicer.
- **lockdep does not catch this.** Lockdep finds cycles among *kernel* locks
  (A→B and B→A). Here the cycle runs `f_pos_lock` → *userspace* → pipe data →
  back. There's only one kernel lock involved. A lockdep stress run comes back
  perfectly clean while the machine hangs.

The mirror branch has the same shape: `splice_file_to_pipe()` →
`wait_for_space()` → `pipe_wait_writable()` sleeps on the pipe's *reader*.

---

## 8. What the patch actually does

The insight: **do the sleeping outside the lock, and stop the transfer from
sleeping at all.**

The kernel already has the primitive for the first half.
`ipipe_prep()` ([fs/splice.c:1721][splice_c]) waits for the pipe to become
readable and returns holding no locks. It's already used by
`splice_pipe_to_pipe()` and `do_tee()`. `opipe_prep()` is its mirror for pipe
space.

Here's the patched `ipipe` branch, annotated:

```c
} else if (ipipe) {
        bool pos_locked;

        if (off_in)
                return -ESPIPE;
        if (off_out && !(out->f_mode & FMODE_PWRITE))
                return -EINVAL;
        if (unlikely(out->f_flags & O_APPEND))
                return -EINVAL;
        if (in->f_flags & O_NONBLOCK)
                flags |= SPLICE_F_NONBLOCK;

        /* [A] Only lock when there is an implicit offset AND the file
         *     is genuinely shared. */
        pos_locked = !off_out && file_needs_f_pos_lock(out);

        for (;;) {
                unsigned int xflags = flags;

                if (pos_locked) {
                        /* [B] Block HERE, with no lock held. */
                        ret = ipipe_prep(ipipe, flags);
                        if (ret)
                                break;

                        /* [C] Make the transfer refuse to sleep. */
                        xflags |= SPLICE_F_NONBLOCK;

                        mutex_lock(&out->f_pos_lock);
                }

                offset = off_out ? *off_out : out->f_pos;

                ret = rw_verify_area(WRITE, out, &offset, len);
                if (likely(ret >= 0)) {
                        file_start_write(out);
                        /* [D] Only the file write happens under the lock. */
                        ret = do_splice_from(ipipe, out, &offset, len, xflags);
                        file_end_write(out);

                        if (!off_out)
                                out->f_pos = offset;
                        else
                                *off_out = offset;
                }

                if (!pos_locked)
                        break;

                mutex_unlock(&out->f_pos_lock);

                /* [E] Lost a race for the data — go wait again. */
                if (ret != -EAGAIN || (flags & SPLICE_F_NONBLOCK))
                        break;
        }
}
```

### Walking the five marked points

**[A] The guard.** Two conditions. `!off_out` means we're on the implicit-
offset path — an explicit offset doesn't touch `f_pos`, so it needs nothing.
`file_needs_f_pos_lock(out)` is the *same* predicate `fdget_pos()` uses, so
splice now agrees with `read`/`write`/`lseek` about exactly which files need
protecting. This is also what keeps us away from that `f_pipe` union from §3:
a pipe has no `FMODE_ATOMIC_POS`, so `pos_locked` is never true for one.

**[B] The wait moves out.** `ipipe_prep()` does the potentially-long sleep
*before* we own anything. If we sleep for a minute here, no one else is
blocked.

**[C] The transfer is made non-blocking.** `SPLICE_F_NONBLOCK` affects
**only the pipe side** — that's its documented meaning in `splice(2)`. So
`splice_from_pipe_next()` returns `-EAGAIN` instead of calling
`pipe_wait_readable()`. The deadlock is structurally impossible: the code
holding the lock can no longer wait on a pipe.

**[D] What's left inside.** Only the actual write to the file. That can still
block — page allocation, writeback throttling — but that is *precisely* what
`write(2)` already does under this same lock. No new hazard class.

**[E] The retry.** Between [B] (pipe has data) and the transfer, another
consumer may have drained it, giving a spurious `-EAGAIN`. Rather than pass
that to a caller who never asked for non-blocking, we loop and wait again.

### Why the retry loop is safe

Three properties, worth checking yourself:

1. **It can't spin.** Each iteration re-enters `ipipe_prep()`, which *blocks*
   until data is available. No busy-waiting.
2. **It terminates at EOF.** When the last pipe writer closes,
   `ipipe_prep()` returns `0` (not `-EAGAIN`) — look at its `if
   (!pipe->writers) break;` with `ret` still 0. The transfer then returns 0,
   and we exit normally with EOF.
3. **`-EAGAIN` can only mean "pipe drained".** Inside `pos_locked`, `out` is
   necessarily a regular file — that's what `FMODE_ATOMIC_POS` tests. Regular
   buffered writes don't return `-EAGAIN` here. So the retry condition can't
   be triggered by the output side and loop forever.

And a caller who genuinely asked for `SPLICE_F_NONBLOCK` still gets its
`-EAGAIN` — that's the `(flags & SPLICE_F_NONBLOCK)` half of [E].

### The `opipe` branch

Structurally identical, mirrored: `opipe_prep()` instead of `ipipe_prep()`
(waiting for pipe *space* rather than pipe *data*), `in->f_pos_lock` instead
of `out->f_pos_lock`.

### The header move

`file_needs_f_pos_lock()` was `static` in `fs/file.c`. The patch moves it
verbatim to `include/linux/fs.h` so `fdget_pos()` and `do_splice()` share one
definition rather than drifting apart. It must sit *after* `struct
file_operations`, since it dereferences `f_op->iterate_shared`.

---

## 9. What it costs

Worth knowing, because recent VFS work has gone the other way — Brauner's
*"fs: don't needlessly acquire f_lock"* and Guzik's *"fs: reduce work in
fdget_pos()"* are both about making this path cheaper.

The answer: for the common case, **one predicate call and a not-taken
branch.** `file_needs_f_pos_lock()` returns false for any file that isn't both
seekable and actually shared, so a plain single-process `splice()` acquires no
lock. The extra work only appears when there genuinely is a shared position to
protect — which is exactly when correctness demands it.

---

## 10. What it deliberately does *not* fix

`do_sendfile()` and the `copy_file_range(2)` syscall body have the identical
unlocked pattern, in their own separate code. sendfile is confirmed to corrupt
and **still does** after this patch. That's intentional:

- Neither has a user-visible pipe, so there's no `*_prep()` primitive to
  reuse — the whole technique doesn't transfer.
- `sendfile`'s `out_fd` is frequently a socket, which can block indefinitely
  on a peer that never reads. Holding the *input* file's `f_pos_lock` across
  that recreates the same category of deadlock.
- `copy_file_range()` reads `f_pos` on **both** files, so it needs two locks
  and a real ordering rule (including the `in == out` case). The single-lock
  structure here doesn't express that.

---

## 11. Verifying it yourself

Three reproducers, all honouring `$TMPDIR` so you can point them at any
filesystem:

| program | tests | on an unpatched kernel |
|---|---|---|
| `splice_race.c` | corruption via splice | fails 10/10 |
| `sendfile_race.c` | corruption via sendfile | fails often (timing-sensitive) |
| `splice_deadlock.c` | **liveness** | passes |

That last row is the important one. `splice_deadlock.c` exists solely to catch
the §7 deadlock, and it's built so that one process both feeds the pipe *and*
writes to the shared descriptor — the shell-pipeline shape.
`splice_race.c` can't catch it, because there the feeder and writer are
separate processes, so the splicer always eventually makes progress.

The expected matrix:

| kernel | corruption | liveness |
|---|---|---|
| unpatched | 10/10 corrupt | completes |
| naive `f_pos_lock` | fixed | **hangs** |
| this patch | fixed | completes |

Build with `CONFIG_DETECT_HUNG_TASK=y`. Without it the deadlock is silent —
and remember from §7 that `CONFIG_PROVE_LOCKING` will *not* save you here.

---

## 12. The historical context

This gap is not news to the VFS. In March 2014, in the thread that created
`f_pos_lock`, Al Viro enumerated exactly what was being left out:

> OK, other than read/write/readv/writev/lseek we have only `sendfile{,64}`
> and `splice`; everything else either deals with struct file that is
> guaranteed to be not accessible by any syscall (e.g. coredump code) or is
> not a regular file. So the only question is whether we care for syscalls
> that are out of scope for POSIX.

and Linus answered:

> Yeah, I saw the `do_sendfile` one and decided we don't care. Not only is is
> out of POSIX spec (so if you break it you get to keep both pieces), the
> whole `sendfile()` thing is a bit of a hack.
>
> But we could take the `f_pos_lock` explicitly in `do_sendfile` for the
> `!ppos` case if we decide we care.

So the patch isn't fixing an oversight — it's arguing that the premise has
expired. "You opted into non-POSIX behaviour" was fair when `splice()` was a
specialist interface. It isn't fair now that `cat` calls it on your behalf.

Two footnotes worth carrying:

- The 2014 decision was explicitly provisional. Viro's "*until POSIX learns
  of its existence*" was forwarded to the Austin Group and never answered.
- Linus's suggested fix shape — take the lock around the `!ppos` case — is
  fine for `sendfile` (no pipe wait inside it) and is *exactly* the thing that
  deadlocks in `splice`. See §7.

Full quotes, message-IDs and search coverage are in `PRIOR-ART.md` on the
`claude-meta` branch.

---

## 13. Where to look in the source

| what | where |
|---|---|
| `do_splice()`, the two patched branches | `fs/splice.c:1329` (ipipe), `:1406` (opipe) |
| `ipipe_prep()` / `opipe_prep()` | `fs/splice.c:1721` / `:1757` |
| the blocking wait that causes it all | `splice_from_pipe_next()`, `fs/splice.c:515` |
| `iter_file_splice_write()` | `fs/splice.c:662` |
| `fdget_pos()` | `fs/file.c:1231` |
| `file_needs_f_pos_lock()` | `include/linux/fs.h:1981` (moved there by the patch) |
| `f_pos` / `f_pos_lock` / `f_pipe` union | `include/linux/fs.h:1274` |

[fs_h]: https://github.com/m68k-io/linux/blob/claude/splice-fpos-race/include/linux/fs.h
[file_c]: https://github.com/m68k-io/linux/blob/claude/splice-fpos-race/fs/file.c
[splice_c]: https://github.com/m68k-io/linux/blob/claude/splice-fpos-race/fs/splice.c
