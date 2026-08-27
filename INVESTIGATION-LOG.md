# The ksh "race condition" that turned out to live in the kernel

A walkthrough of how "ksh sometimes drops output in a subshell" turned into a
kernel bug in `do_splice()`, written up so the reasoning is reusable next time
something like this shows up.

---

## TL;DR

- **Symptom**: `out=$(eval "foo | cat" 2>&1)` in ksh93 intermittently comes back
  a few bytes short.
- **First theory** (wrong, but reasonable): a timing bug in ksh's own subshell
  job-wait logic (`sh_subshell()`/`job_wait()` in `subshell.c`).
- **What it actually is**: the Linux kernel's `splice()` implementation
  reads-modifies-writes a file's position (`f_pos`) *without* the lock that
  every other syscall touching `f_pos` uses. When `splice()` races a plain
  `write()` on an fd that's shared between two processes (via `dup()` +
  `fork()`), one of them can silently clobber the other's data.
- **ksh's only role**: it does something completely ordinary (`2>&1` before
  forking a pipeline) that happens to create exactly the fd-sharing situation
  the kernel bug needs. Coreutils' `cat` is also innocent — it just uses a
  documented, legitimate `splice()` fast path.
- **Where things stand**: root cause identified down to the exact lines of
  kernel source, a fix drafted and compiled clean, a bug report drafted for
  `linux-fsdevel@vger.kernel.org`. Not yet runtime-tested (no boot
  environment available here) — that's the one open item.

Files in this folder:
- `splice-fpos-race-report.txt` — the bug report, ready to send.
- `splice-fpos-race.patch` — the fix, `git am`-ready (needs your
  `Signed-off-by`).
- `splice_race.c` — the minimal, no-shell, no-ksh reproducer.

---

## Part 1 — The symptom, and why it looked like a ksh bug

The starting point was project notes for an unrelated OpenZFS-on-Alpine CI
effort, which had already tracked ksh93 crashes down to two real ksh bugs
(both fixed, both about a `sh.save_env[]` pointer into `environ` going
stale). Along the way, a *third*, unrelated, already-upstream-known issue
turned up: ksh93 issue [#975](https://github.com/ksh93/ksh/issues/975),
"intermittent failure causing `print -u2` breakage." The repro:

```ksh
function foo
{
	print -u2 foobar
	for ((i=0; i < 8000; i++)); do print abcdefghijk; done
	print -u2 done
}
out=$(eval "foo | cat" 2>&1)
print "${#out}"
```

Expected output: `96011`. Intermittently: `96006` — exactly
`strlen("done\n")` short. The upstream maintainer had already spent time on
this and had nothing better than "we really need to find a way to solve this
bug... I have no idea where to even begin looking."

The natural first suspect is ksh's own subshell/job-control code, because
that's the layer visibly responsible for turning `$(...)` into "run this,
wait for it, capture the output." `subshell.c`'s `sh_subshell()` sets up a
temp file to capture output, forks the pipeline, and — critically —
`job_wait()` waits for **one tracked pid** (`sh.spid`, the last process in
the pipeline) and then immediately proceeds to swap the temp file in as the
substitution's value. If a *different* process in the pipeline (`foo`, not
`cat`) was still writing when that happened, you'd get exactly this kind of
"finalized before everything actually landed" bug. That's a completely
sensible hypothesis for a shell to have this class of bug in — job control
and subshell capture is genuinely intricate, and ksh has real, similar bugs
elsewhere in the same file (see the `sh.save_env[]` fixes from the same
project). It was worth taking seriously.

It turned out to be a red herring — but a useful one, because ruling it out
is what led to the real bug.

---

## Part 2 — Reproducing it locally, and a strace filter that hid the answer

Before touching kernel internals at all, the obvious move was: can this be
reproduced without CI? Unlike the earlier `sh.save_env[]` bugs (which needed
a musl environment variable with an invalid name, present only in CI's
container images), this one reproduced locally at roughly 20–30% per run —
no special setup needed. That's a huge win: it means `strace` alone can
answer questions that would otherwise need real CI runs, core dumps, and
`addr2line`.

The first `strace` pass used a narrow filter —
`trace=write,close,wait4,pipe2,dup2,clone,fork,vfork` — chosen to focus on
process lifecycle and I/O around the crash. It caught the failure, and
showed something suggestive:

```
write(2, "done\n", 5) = 5      <- succeeds; this is foo's direct write
... foo exits, reaped via wait4 ...
... parent does cleanup close()s ...
write(1, "96006\n", 6)          <- final result: 5 bytes short
```

This *looked* consistent with the ksh-side theory: the write succeeds, the
process exits, gets reaped, and *then* something finalizes the capture —
maybe before the data is really flushed. Except: `write(2, ...)` returning 5
already means the kernel accepted those bytes into the file. There's no
"flush" step missing for a plain `write(2)` to a regular file — it's
synchronous from the calling process's point of view. That should have been
the tell that the narrow trace was hiding the real mechanism rather than
confirming a hypothesis, and it was a good reminder for next time: **a
trace that's consistent with your hypothesis isn't the same as a trace that
rules out the alternatives you didn't filter for.**

Re-running with `splice`, `sendfile`, and `copy_file_range` added to the
filter changed the picture completely:

```
12401 (cat) splice(3, NULL, 1, NULL, 524288, 0 <unfinished ...>
12400 (foo) write(2, "done\n", 5 <unfinished ...>
12401       <... splice resumed>) = 24
12400       <... write resumed>) = 5
12400       fstat(2, {st_size=96007, ...})   <- 5 bytes short of 96012
```

`cat` was never calling `read()`/`write()` on the piped data at all — modern
GNU coreutils `cat` uses `splice()` as a zero-copy fast path whenever its
input is a pipe. Since `splice()` needs at least one side to be a pipe, and
here the *output* is a regular file, `cat` bounces through a small internal
pipe: `splice(stdin_pipe → internal_pipe)` then `splice(internal_pipe →
stdout)`. That second call is the one racing `foo`'s direct `write(2)`.

---

## Part 3 — Why `foo`'s write and `cat`'s splice land on the *same* file

This only makes sense once you see how `2>&1` interacts with a pipeline
inside a subshell capture. The full command is:

```sh
out=$(eval "foo | cat" 2>&1)
```

`2>&1` here applies to the *whole* `eval "foo | cat"`, before the pipeline
is forked. So both children inherit fd 1 and fd 2 already pointing at the
same place: fd 1 (stdout) is the pipe to `cat` for `foo`, and unchanged for
`cat` itself (its stdout is the actual capture file, since it's the last
stage); fd 2 is redirected to whatever fd 1 was pointing to *before the
pipe was set up* — i.e. the comsub's capture temp file — for both processes.

So inside the pipeline:
- `foo`'s fd 1 → pipe into `cat` (its 8000 `print abcdefghijk` lines go here).
- `foo`'s fd 2 → the capture file directly, bypassing `cat` entirely (its
  two `print -u2` lines go here).
- `cat`'s fd 0 → read end of the pipe from `foo`.
- `cat`'s fd 1 → the capture file (same file as `foo`'s fd 2).

`foo`'s fd 2 and `cat`'s fd 1 are **`dup()`'d from the same original
`open()`** of the capture file (that single `open()` happened once, before
either process was forked, in ksh's `sh_subtmpfile()`). That's the key fact:
they don't just point at the same *path* — they share the same *open file
description* in the kernel, including its file offset. That sharing is
completely ordinary and intentional; it's exactly how `2>&1` is supposed to
work, and it's exactly how two independent `write()` calls into that shared
fd from two different processes are normally *safe* (more on why in Part 5).

The bug is that `splice()` doesn't honor that safety mechanism the way
`write()` does.

---

## Part 4 — Proving it has nothing to do with ksh

At this point there were two live possibilities:
1. Something ksh-specific about how it sets up this shared fd is unsafe.
2. The unsafety is in how the *kernel* handles two processes racing on a
   shared file offset when one side uses `splice()` — nothing to do with
   ksh at all.

Two experiments settled it, both cheap because the bug reproduces locally:

**Experiment A — swap the innocent-looking suspect.** Replace `cat` with
`dd bs=64k` (which strace confirmed makes zero `splice`/`sendfile`/
`copy_file_range` calls) in the exact same ksh script. Same fd-sharing setup,
same ksh code path, only the *last pipeline command's I/O implementation*
changes.

Result: **0 failures in 40 runs**, versus ~20–30% with `cat`. Causally, not
just correlationally — this isolates the trigger to something specific
about `cat`'s implementation choice, not the shell script around it.

**Experiment B — remove ksh from the picture entirely.** A ~70-line C
program (`splice_race.c` in this folder) with three plain `fork()`ed
processes and no shell involved at all:
- a **feeder**, writing 8000×12-byte chunks into a pipe;
- a **splicer**, relaying that pipe into a `dup()`'d copy of a shared fd via
  a `splice()` loop — deliberately mimicking `cat`'s fast path;
- a **writer**, spinning on a shared counter until the feeder is nearly
  done, then doing one plain `write()` into its *own* `dup()`'d copy of the
  same fd.

No ksh, no `cat`, no pipes-inside-pipelines — just the specific fd-sharing
+ concurrent-splice-and-write pattern, isolated to its essence.

Result: **200/200 runs corrupted.** Not just short — inspecting the file
content showed the writer's 5 bytes (`"done\n"`) really do land in the file,
but *mid-stream*, clobbering part of a chunk the splicer had relayed,
instead of being safely appended after it. Once isolated like this, the bug
is deterministic enough (in this environment) to hit on nearly every attempt,
because the synthetic setup maximizes the chance the `write()` lands while a
`splice()` call is actually in flight — the real-world 20–30% rate with ksh
is just this same race with much less deliberate timing.

**This is the pattern worth remembering**: once you have a hypothesis about
*where* a bug lives, the fastest way to gain confidence isn't more staring at
the original repro — it's building the smallest possible program that keeps
only the mechanism you suspect and discards everything else (the shell, the
specific script, the specific coreutils binary). If the bug survives that
reduction, you've found where it actually lives. If it doesn't, you've
learned something too.

---

## Part 5 — Some background: how can two *processes* share a file offset?

This is worth being explicit about, because it's the linchpin of the whole
bug and easy to get hazy on.

A file descriptor (the small integer, `1`, `2`, `3`, ...) is just an index
into a per-process table. Each entry in that table points to a kernel object
called an **open file description** (this is the thing `struct file`
represents in kernel source) — and *that* object is what actually holds the
current read/write offset (`f_pos`), the access mode, etc.

Normally, `open()` creates a brand new open file description, so each fd
from a separate `open()` call has its own independent offset — writing
through one doesn't move the other. But `dup()`, `dup2()`, and inheriting
fds across `fork()` do **not** create a new open file description — they
create a new *fd table entry* that points at the *same* one. That's exactly
what "shared offset" means: two fds, possibly in two different processes
after a `fork()`, both referencing one `struct file`, one `f_pos`.

This is completely normal and used constantly — it's precisely the
mechanism behind shell redirection tricks like `2>&1`, and behind `foo >
log 2>&1` writing both streams to the same file without one overwriting the
other's data, *because* they share a position that advances correctly no
matter which fd's `write()` call happens first.

The reason that "no matter which one happens first" guarantee holds for
`write()` vs. `write()`, but not for `write()` vs. `splice()`, is the actual
bug — and that's the kernel source dive in Part 6.

---

## Part 6 — The kernel source: `fdget_pos()` vs. `do_splice()`

(All line references are against this tree's `/data/linux`, currently
`v7.2-14827-g45c13f3f9e3b`.)

### How ordinary read/write protects a shared offset

Every `read(2)`/`write(2)`/`lseek(2)` syscall on a real fd goes through
`fdget_pos()` in `fs/file.c` to resolve that fd into a `struct file *`:

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

`file_needs_f_pos_lock()` decides whether this particular fd access needs
protecting:

```c
static inline bool file_needs_f_pos_lock(struct file *file)
{
	if (!(file->f_mode & FMODE_ATOMIC_POS))
		return false;
	if (__file_ref_read_raw(&file->f_ref) != FILE_REF_ONEREF)
		return true;
	if (file->f_op->iterate_shared)
		return true;
	return false;
}
```

In plain terms: *if this is a regular/seekable file, and its reference count
is more than 1* — meaning some other fd, in this process or another, via
`dup()`/`fork()`, is also pointing at the same open file description — *then
lock it before touching `f_pos`.* If the file is only reachable through one
fd (`FILE_REF_ONEREF`), there's no one to race against, so the lock is
skipped for speed. This is a nice bit of kernel engineering: pay the mutex
cost only when there's actually something to protect against.

So a normal `write(2)` on a shared fd does, atomically: lock → read current
`f_pos` → write the data at that offset → advance `f_pos` by however many
bytes were written → unlock. Two processes hammering `write()` on fds
sharing one open file description will never race each other's *position
bookkeeping* — the mutex serializes the whole read-use-advance sequence.

### What `do_splice()` does instead

`fs/splice.c`, in the branch used when the input is a pipe and the output is
a regular file (exactly `cat`'s `splice(internal_pipe → stdout)` call):

```c
} else if (ipipe) {
	...
	} else {
		offset = out->f_pos;                                  /* (1) */
	}
	...
	ret = do_splice_from(ipipe, out, &offset, len, flags);        /* (2) */
	...
	if (!off_out)
		out->f_pos = offset;                                  /* (3) */
```

Steps (1)–(3) are the exact same "read current position, use it, write back
the advanced position" sequence as `write(2)` — but with **no lock around
any of it**. It doesn't go through `fdget_pos()` at all; `do_splice()`
resolves its `struct file *` arguments a different way and just touches
`->f_pos` directly.

There's even a comment elsewhere in the tree, in `fs/nfsd/vfs.c`, spelling
out the contract this is supposed to follow:

```c
/*
 * NB: normal system calls hold file->f_pos_lock when calling
 * ->iterate_shared and ->llseek, but nfsd_readdir() does not.
 * Because the struct file acquired here is not visible to other
 * threads, it's internal state does not need mutex protection.
 */
```

NFSD gets away without the lock because *its* `struct file` genuinely isn't
shared with anything else. `do_splice()`'s output file very much can be —
it's just whatever fd userspace handed it, which (as established in Part 5)
might be `dup()`'d across several processes. Nothing in `do_splice()` checks
for that.

### Why the actual bytes get clobbered, not just the bookkeeping

You might expect an unprotected offset race to just produce a wrong *final
size*, with the data itself intact but possibly reordered. That's not quite
what happens, and it's worth being precise about why.

The actual byte-level write in both paths eventually goes through the same
inode-level write machinery, which *does* lock at that level (e.g.
`inode_lock`/`i_rwsem` for a regular file's buffered write path) — so any
individual write's bytes land correctly, atomically, wherever they're told
to go. The bug is entirely in what each side is *told*: both `write()`'s
locked sequence and `do_splice()`'s unlocked sequence independently do
"read current `f_pos` → use it as my target offset." If they interleave so
that both read `f_pos` *before* either has written its new value back, both
computed the *same* target offset — and both then perform a real, correctly
executed write starting at that same spot. Whichever one's actual data
transfer finishes second simply overwrites the tail of what the first one
wrote, because from the filesystem's point of view they were never told
they overlapped.

That's exactly the observed symptom: `"done\n"` (5 bytes) really does get
written, correctly, as 5 contiguous bytes — just at the same offset `cat`'s
splice call was still in the middle of using for one of its own chunks,
so it stomps 5 bytes of that chunk instead of landing after it. The final
file is short by exactly the size of whichever write "lost."

---

## Part 7 — The fix

The fix is the minimal, obvious one once the asymmetry is named: give
`do_splice()` the same protection `fdget_pos()` already gives `read()`/
`write()`.

Three pieces (see `splice-fpos-race.patch`):

1. **`file_needs_f_pos_lock()` moves from `fs/file.c` to
   `include/linux/fs.h`.** It was `static inline`, private to one file. To
   reuse the *exact same* "does this file need protecting" check in
   `fs/splice.c`, it needs a shared home. (It has to go specifically *after*
   `struct file_operations` is fully defined in the header, since it
   dereferences `file->f_op->iterate_shared` — an ordering detail that
   caused the first compile attempt to fail with "invalid use of undefined
   type," a good reminder that header placement in a codebase this size is
   its own small puzzle.)

2. **`do_splice()` takes the lock conditionally, around the same
   read/transfer/write-back sequence that was already there**, for both the
   pipe→file branch (`out->f_pos`) and the file→pipe branch (`in->f_pos`):

   ```c
   if (file_needs_f_pos_lock(out)) {
       pos_file = out;
       mutex_lock(&pos_file->f_pos_lock);
   }
   offset = out->f_pos;
   ```

3. **Early-return paths inside that now-locked region become `goto done`**,
   with a single unlock site:

   ```c
   done:
       if (pos_file)
           mutex_unlock(&pos_file->f_pos_lock);
   ```

   `pos_file` (not a plain boolean) tracks *which* of `in`/`out` got locked,
   since the function has two separate branches that can each lock a
   different file, but never both in the same call.

This is deliberately conservative: it doesn't change behavior at all for the
unshared-file case (the existing `file_needs_f_pos_lock()` check already
skips locking there), and it doesn't touch the pipe-to-pipe branch, which
never touches a regular file's `f_pos` in the first place.

**Validation so far**: compiles clean against this tree's `defconfig`,
including under `W=1` (extra warnings enabled) — no new warnings. **Not yet
runtime-tested**: there's no boot environment available here to build, boot,
and rerun `splice_race.c` against the patched kernel. That's the one
concrete next step, and it belongs on real hardware you can actually boot —
which you already have, since it's the same tree and build pipeline you used
to cross-confirm the bug in the first place.

---

## Part 8 — Where this leaves the original ksh issue

Upstream ksh93 issue #975 names ksh, but nothing in ksh needs to change for
correctness — it's doing something completely unremarkable (`2>&1` before
forking a pipeline) that any shell does. Two things are still worth deciding,
independently of each other:

- **Report the kernel bug upstream** (`splice-fpos-race-report.txt`, drafted
  and ready, pending your `Signed-off-by` on the patch and your call on
  timing relative to the boot test).
- **Whether to also add a defensive workaround in ksh** (e.g. having
  `sh_subtmpfile()` avoid the shared-offset `dup()` setup, giving each
  redirected fd its own independent `open()` by path) so ksh stops being
  exposed to this specific kernel bug regardless of if/when it's fixed
  upstream, and regardless of what any future pipeline command's I/O
  implementation does internally. This wasn't pursued further once the bug
  was traced to the kernel, but it's still on the table if you want
  ksh insulated from this before a kernel fix lands.
- Whether it's worth a comment on the still-open `ksh93/ksh#975` pointing
  future readers away from ksh and at this instead, since anyone else
  hitting that issue is currently being pointed in the wrong direction.

None of these are decided — they're just the options this investigation
leaves on the table.

---

## Part 9 — How sure is this, actually?

Worth splitting into two separate questions, because they have different
answers: *is the bug real*, and *is the patch ready to submit*.

### Is the bug real: high confidence

This part doesn't rest on trusting any analysis — it's a program you can
read end to end in a few minutes (`splice_race.c`) and a diff you can read
yourself (`fs/splice.c` vs. `fs/file.c`'s `fdget_pos()`). It reproduced
200/200 on two independent machines, including a real from-source
`linux-7.2.y` build, and the corruption shape (data landing mid-file, not
truncated) matches the mechanism exactly. The right move here isn't to take
my word for it — it's to read those two functions side by side yourself:

```
grep -n "f_pos_lock\|out->f_pos\|in->f_pos" fs/splice.c fs/file.c
```

### Is the patch submission-ready: no, and the report now says so

Going and actually searching for prior art (rather than just asserting
confidence) turned up something worth knowing: in 2023, Christian Brauner
submitted ["file: always lock position"](https://www.spinics.net/lists/linux-fsdevel/msg245393.html),
fixing a related bug in the *exact same* locking mechanism — `fdget_pos()`'s
own fast path (skip the lock when a file's refcount is 1) turned out to be
unsafe against io_uring fixed files and `pidfd_getfd()` fd-stealing tricks.
That's real, recent, serious maintainer attention on "who needs
`f_pos_lock` and when" — this isn't an obscure corner nobody would care
about. But that fix was scoped to `->iterate_shared` (directory iteration /
`getdents`); from what's readable of that thread, `splice()` wasn't
mentioned. The tree here, a few days old, still has the gap. That's either
a real still-open hole that fix didn't cover, or something already known
for a reason not found here — and the two most authoritative places to
check that (`lore.kernel.org`, `bugzilla.kernel.org`) both blocked automated
fetches (an anti-bot wall), so this genuinely wasn't ruled out, not just
skipped.

Beyond that open question, there are two concrete unverified risks, both
now called out explicitly in the report and patch rather than left implicit:

1. **Lock ordering / deadlock risk under io_uring.** `io_uring/splice.c`
   calls `do_splice()` directly, so the patch's new `f_pos_lock` acquisition
   is reachable from io_uring's async context — precisely the subsystem
   where this same lock has caused real hangs before (different root cause,
   but a documented pattern: see the `__fdget_pos` syzbot hang Al Viro
   handled). No lockdep-enabled stress test of concurrent io_uring splices
   was done here.
2. **`do_sendfile()` not checked.** It's a structurally similar syscall and
   may have the identical unlocked-`->f_pos` pattern. If so, this patch
   fixes one entry point but not a twin sibling — a maintainer's likely
   first question.

Net: strong confidence in the diagnosis, real but unaudited risk in the
specific fix. The report and patch commit message were both updated to
say this plainly and frame the submission as an RFC rather than a finished
fix — better to be told "already known, here's why" or "here's what you
missed" than to be person #10,000 posting an AI-shaped patch that reads
more finished than it is.

---

## Part 10 — Actually doing the audits: what changed

Two of the "unaudited risks" from Part 9 got followed up on directly, with
concrete results — one made the picture bigger, one made it smaller.

### `sendfile()` and `copy_file_range()` have the same bug, independently

Checking "does `do_sendfile()` have the same pattern" meant reading it, not
guessing. It does — and it doesn't even share code with `do_splice()`:

```c
CLASS(fd, out)(out_fd);
...
out_pos = fd_file(out)->f_pos;             /* read, unlocked */
...
retval = do_splice_direct(fd_file(in), &pos, fd_file(out), &out_pos, count, fl);
...
fd_file(out)->f_pos = out_pos;             /* write back, unlocked */
```

`do_splice_direct()` was the key thing to check here, because if it
internally called `do_splice()`, the existing patch might already cover
this for free. It doesn't — it calls a separate `do_splice_direct_actor()`
that only ever touches the `&pos`/`&out_pos` pointers `do_sendfile()` hands
it, never `file->f_pos` directly. So the responsibility for the initial
read and final write-back of `->f_pos` sits entirely in `do_sendfile()`
itself, with its own independent copy of the identical unlocked
read-modify-write bug. `copy_file_range(2)`'s syscall body has the exact
same shape a second time.

This was worth actually reproducing, not just reading — source that *looks*
identical can still behave differently once real syscalls and real
scheduling are involved. A `sendfile()`-based analog of the original
reproducer (`sendfile_race.c`) confirmed it directly: a relayer process
`sendfile()`s a large pre-filled file into a shared/dup'd output fd while a
sibling process does one direct `write()` into its own dup, synchronized to
start at the same instant. **26 corrupted runs out of 30**, same exact
signature as splice — short by precisely the direct write's length, with
that write's bytes present in the file but not at the tail.

(Getting there took a wrong turn worth keeping, in the spirit of Part 2:
the first version of this reproducer fed `sendfile()` from a pipe, which
hung immediately — a 5-second `timeout` on a *minimal*, one-call isolation
of just that piece showed `sendfile()` returning `EINVAL` for a pipe input
on this kernel, not a hang. The real hang was downstream of that: the
relayer exited immediately after the failed call without draining the
pipe, so the feeder blocked forever on a full pipe buffer, and the
threshold-based writer spun forever waiting for a counter that had frozen.
None of that had anything to do with the bug under test — always isolate a
new failure mode down to its smallest form before concluding it's the
thing you're actually looking for.)

**What this means for the finding**: it's not "`do_splice()` has a bug" —
it's "the kernel's fast-data-movement syscall family (`splice`, `sendfile`,
`copy_file_range`) all independently reimplement the same
read-modify-write-`f_pos` sequence `fdget_pos()` protects for plain
`read`/`write`, and none of the three protect it." The patch as it
currently stands only fixes one of the three. The report now says this
plainly, and recommends a complete fix touch all three — a materially
different (larger) scope than where this started.

### The io_uring lock-ordering worry: checked, and it's not the hazard it looked like

The other open item was whether this patch's new `f_pos_lock` acquisition,
reachable from `io_uring/splice.c` (which calls `do_splice()` directly),
could deadlock against some io_uring-internal lock — flagged as a real risk
specifically because this same mutex has hung before in an io_uring
context (a different bug, fixed by Christian Brauner's 2023 "always lock
position" patch).

Reading `io_splice()`/`io_tee()` in `io_uring/splice.c` settled it: both
release `ctx`'s submission lock (`io_ring_submit_unlock()`) *before* ever
calling `do_splice()`/`do_tee()`, and both assert
`WARN_ON_ONCE(issue_flags & IO_URING_F_NONBLOCK)` — meaning they only ever
execute on an io-wq worker thread where blocking is expected, never on
io_uring's inline fast-completion path. No io_uring lock is held at the
point `do_splice()` — and therefore this patch's new lock — gets reached.
The specific ABBA shape that made this worth worrying about doesn't
appear to exist on this call path.

This doesn't retire the concern entirely (a single read of the call path
isn't the same as a lockdep-enabled stress run under real concurrency), but
it moves it from "flagged as unaudited, treat as a real open risk" to
"checked, no hazard found on this path, worth a real stress test before
calling it closed." The report and patch reflect that update.
