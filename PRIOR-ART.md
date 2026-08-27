# Prior art: this was decided deliberately in 2014

The gap is **not** an oversight. It was identified and consciously deferred
in the very thread that created `->f_pos_lock`.

## The 2014 exchange

Thread: *Update of file offset on write() etc. is non-atomic with I/O*
(Michael Kerrisk, 2014-02-17 → 2014-05-04, 43 messages) — the thread that
produced `vfs: atomic f_pos accesses as per POSIX`.

[lore thread](https://lore.kernel.org/all/CA+55aFwSr-CnqE6vxykhkNTo7BkFXGD6uGJC00A2vJd10HZxJA@mail.gmail.com/T/)

**Al Viro**, `<20140303233902.GP18016@ZenIV.linux.org.uk>`:

> `do_sendfile()` is also there and this one is even more unpleasant ;-/
> We probably can ignore that one (until POSIX learns of its existence),
> thouhg...

**Linus Torvalds**, `<CA+55aFwSr-CnqE6vxykhkNTo7BkFXGD6uGJC00A2vJd10HZxJA@mail.gmail.com>`:

> Yeah, I saw the `do_sendfile` one and decided we don't care. Not only is
> is out of POSIX spec (so if you break it you get to keep both pieces),
> the whole `sendfile()` thing is a bit of a hack.
>
> But we could take the `f_pos_lock` explicitly in `do_sendfile` for the
> `!ppos` case if we decide we care. The error handling is the main
> annoyance.

**Al Viro**, `<20140303235456.GR18016@ZenIV.linux.org.uk>` — naming the exact
two gaps:

> OK, other than read/write/readv/writev/lseek we have only `sendfile{,64}`
> and `splice`; everything else either deals with struct file that is
> guaranteed to be not accessible by any syscall (e.g. coredump code) or is
> not a regular file. So the only question is whether we care for syscalls
> that are out of scope for POSIX.

**Cedric Blancher**, 2014-03-04: *"I've forwarded your request to the Austin
Group (who manage the POSIX stuff)."*

**No answer ever came back.** The thread's last message (Kerrisk, 2014-05-04)
is him noting he'd been CC-trimmed out. So the condition Viro attached —
*"until POSIX learns of its existence"* — was never resolved either way.

## What this means

Three things follow, and they matter for how the patch is pitched:

1. **Don't claim novelty.** Leading with "nobody noticed" gets an immediate
   "known, see 2014" and burns credibility. Lead with the 2014 decision.
2. **The decision was explicitly provisional** — "*until* POSIX learns",
   "*if* we decide we care", "the only question is *whether* we care". It was
   left open, not closed. That is the opening.
3. **The 2014 premise has expired for splice specifically.** The argument was
   "out of POSIX spec, so you get to keep both pieces" — sound when splice()
   is opted into. GNU coreutils `cat` now uses splice() for pipe-to-file
   copies, verified on coreutils 9.11:

   ```
   $ echo hi | strace -e trace=splice cat > file
   splice(0, NULL, 4, NULL, 524288, 0)     = 3
   splice(3, NULL, 1, NULL, 524288, 0)     = 3
   ```

   Note the NULL `off_out` — that is the `->f_pos` path. Nobody in
   `cmd1 | cmd2 > file 2>&1` chose splice(); the shell and coreutils chose it.
   The "you opted in" framing doesn't survive that, and it's why the case is
   stronger for splice than for sendfile.

4. **The fix shape suggested in 2014 deadlocks for splice.** Linus's "take the
   `f_pos_lock` explicitly ... for the `!ppos` case" is fine for sendfile — no
   pipe wait inside it. In `do_splice()` it puts the lock around
   `pipe_wait_readable()`. See the rejected patch in `patches/` and the hung
   task trace in `logs/run-naive.txt`.

## Search coverage

Four complete lore result sets (not truncated) plus the full mbox archives:

| query | results |
|---|---|
| `f_pos_lock AND splice` | 109 of 109 |
| `b:do_splice AND b:f_pos` | 101 of 101 |
| `dfn:splice.c AND f_pos` | 136 of 136 |
| `sendfile AND f_pos_lock` | 21 of 21 |
| mbox archives of the above | ~8,300 messages |

Scanning all of it for messages that mention `f_pos_lock` *and*
splice/sendfile/copy_file_range substantively (excluding syzbot lockdep dumps,
where `f_pos_lock` appears only in "showing all locks held") yields exactly
one discussion: the 2014 thread. Everything else is either that, or Sasha
Levin's `kernel/api: add API specification for sys_read`/`sys_write` series
(Dec 2025 → May 2026, five revisions) — worth reading for whether it restates
the `f_pos` atomicity guarantee normatively.

## Related, non-decisive

*splice vs O_APPEND* (2008-10-09,
[lore](https://lore.kernel.org/all/alpine.LFD.2.00.0810090830170.3210@nehalem.linux-foundation.org/T/)),
which produced `efc968d450e0` "Don't allow splice() to files opened with
O_APPEND". Miklos Szeredi opens it with *"While looking at the f_pos
corruption thing…"*, so splice and `f_pos` were already adjacent in 2008, but
the thread goes to O_APPEND, not to locking. Linus's commit message there is
still useful colour:

> It's not entirely clear what the semantics of O_APPEND should be… we could
> make up any semantics we want… So disallow O_APPEND entirely for now. I
> doubt anybody cares, and this way we have one less gray area to worry about.

i.e. splice's file-position semantics were an acknowledged gray area handled
by forbidding the awkward case rather than defining it.
