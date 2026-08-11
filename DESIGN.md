# Module design, C side

How the C half of ballotbox is cut into modules, and why each seam sits where
it does.
The vocabulary is deliberate and used consistently throughout:

- **Module** - anything with an interface and an implementation, at any scale.
- **Interface** - everything a caller must know to use the module correctly.
  Not just the function signatures: also the invariants, the ordering rules,
  the error modes, and the performance characteristics.
- **Depth** - how much behaviour a caller gets per unit of interface they have
  to learn.
  Deep is good; a module whose interface is nearly as complicated as its
  implementation is shallow and usually not worth having.
- **Seam** - the place where an interface lives, and so the place where
  behaviour can be swapped without editing the code around it.
- **Leverage / locality** - what depth buys.
  Leverage is one implementation paying back across many call sites; locality
  is a change, a bug, or a piece of knowledge staying in one file.

Two rules decide the arguments below.
The **deletion test**: imagine deleting the module, and ask whether complexity
disappears (it was a pass-through) or reappears in every caller (it was
earning its keep).
The **two-adapter rule**: one adapter means a hypothetical seam, two means a
real one.
Do not introduce a seam until something actually varies across it.

## What the names mean

The prefix on a module says who owns it, and it is the first thing to check
before renaming anything.

- **`tetris*` and `htttp*`** are shared with the tetriSH project.
  They are vendored by copy, not by submodule, and the copies are kept
  byte-identical in both trees: `libtetrissh`, `libhtttp`, `libtetrisdb`,
  `libtetrisui`, `tetrislogd`, `tetrish` itself.
  A fix belongs in both repositories, and keeping the files identical is what
  makes "has this landed on the other side yet" a plain `diff`.
- **`ballot*`** is this project only: `libballotbrain`, `libballotclient`,
  `ballotd`, `ballotctl`, `ballotu`.
  Nothing in tetriSH may depend on these.
- **`libtetrisutil`** is shared too, and predates the convention.

So the direction of a rename is decided by ownership, not by which repository
you happen to be sitting in.
Renaming a shared module to `ballot*` would fork it: the two copies stop being
comparable, and the next fix has to be ported by hand instead of copied.

## The modules

| Module | Interface | Depth |
| --- | --- | --- |
| `libtetrisdb` | 8 functions | deep |
| `libtetrisutil` logging | `log_open`, `log_send`, level parsing | deep |
| `libtetrisutil` rc | `rc_load`, `rc_classify_line` | deep |
| `tetrislogd` | 4 functions | deep |
| `libtetrisui` | 11 widgets | moderate |
| `tetrish/lib` | `perms`, daemonising | moderate |
| `libtetrissh`, `libhtttp`, `libballotbrain`, `libballotclient` | out of scope here | - |

### libtetrisdb - the reference case

`include/libtetrisdb/tetrisdb.h` is the deepest module in the tree and the one
worth imitating.
Behind eight functions sit a spawned JVM, two pipes, a line protocol with no
request ids, a lock, a bounded queue, and a worker thread.
The caller learns `db_submit(db, sql)` and gets all of it.

Its interface carries three facts that are not visible in the signatures, and
each one is load-bearing:

1. **Exactly one thread may drive the pipe.**
   The protocol has no request ids, so two writers interleave on the wire.
   Rather than telling every caller to serialise, `db_submit` is a queue push
   callable from anywhere and a private worker is the only thing that ever sees
   the pipe.
2. **A table is owned by one process.**
   Each PipeRunner caches pages in a private BufferPool with no cross-process
   invalidation.
   This is why `db_opts_default` refuses to invent a directory: a shared
   default is a corruption waiting to happen, so naming the directory is the
   caller's decision and an unnamed one is an error.
3. **The submit path never blocks and never fails loudly.**
   A wedged child costs dropped statements, counted in `db_dropped()`, not a
   stalled daemon.

That third point is what makes the module deep rather than merely large.
The alternative interface - expose the pipe, let callers handle back-pressure -
would be smaller to implement and much worse to use, because every caller would
have to re-derive the same policy and some would get it wrong.

`db_start`'s `probe` parameter is the one place the interface bends, and it is
worth naming as a deliberate trade.
It exists because a daemon sometimes needs to *read* the table it is about to
write - a sequence counter, a high-water mark - and the only moment that is
safe is after the handshake and before the worker starts.
Rather than add a general query path that would be unsafe at any other time,
the interface offers exactly one statement at exactly the moment it is sound.

### libtetrisutil rc - one grammar, two questions

`.tetrishrc` is read by three components: the shell, `rc_load`'s callers, and
`tetrislogd`'s config overlay.
They ask two different questions of the same file.
The shell asks "is this line a command I should run?"; the daemons ask "is this
one of my directives?".

Those questions used to live in two modules with two implementations, and they
disagreed.
A keyless `=value` line was a directive to one and a command to the other, so
the shell would try to `execvp("=value")` on a line `rc_load` had silently
dropped.
That is exactly the failure the deletion test predicts for a split like this:
delete either module and the knowledge does not vanish, it reappears in the
other one, slightly different.

Both questions now sit behind `include/libtetrisutil/rc.h`, sharing one definition
of what a directive is.
`rc_classify_line` answers the shell's question, `rc_load` answers the daemons'.
The grammar is stated once.

### tetrislogd - a daemon behind four functions

`logd_run`, `logd_stop`, `logd_reopen`, `logd_load_rc`.
Behind them: a Unix datagram socket, a log file with rotation, level filtering,
malformed-record accounting, and optional mirroring into SimpleDB.

The interesting choice is that `logd_stop` and `logd_reopen` are *not*
parameterised.
They set file-scope flags, because their only legitimate caller is a signal
handler, and a signal handler may not touch much else.
The interface admits this rather than pretending to a generality it cannot
have.

Note also what `sink.c` does *not* have: threads.
The loop is single-threaded and blocking on purpose, because the bottleneck is
the disk and the socket's receive queue already serialises delivery.
Concurrency here would add failure modes and buy nothing.
That reasoning belongs in the module, not in each caller's head.

### tetrish/lib - the system programs' shared floor

Every system program under `src/tetrish/system_programs/` is its own binary,
linked with `src/tetrish/lib/*.c` and no libraries at all.
That link rule is itself a seam: `tetrish/lib` is the only place shared code
can live and still reach a system program.

This is why daemonising lives there rather than in `libtetrisutil`.
`dspawn` and `dspawn2` are both system programs, so a `libtetrisutil` home would
mean giving every trivial system program a library dependency it does not use.
The dependency direction the Makefile already establishes decides where the
code goes.

The floor is deliberately thin, because it is not free: the rule globs
`src/tetrish/lib/*.c` into *every* system program, so `daemonise.c` is compiled
into `find` and `ld` as well, which never call it.
That is dead weight rather than a bug, and it is the price of the no-libraries
rule.
It is also the reason to keep only genuinely shared things here - anything with
one caller belongs where that caller is.

`rc_classify_line` moved the other way for the same reason read backwards: its
only caller is the shell, which does link `libtetrisutil`, so keeping it in
`tetrish/lib` bought nothing and cost a duplicated grammar.

### libtetrisui - moderate depth, and that is correct

Fourteen widget functions is a wide interface for 478 lines of implementation,
which by the ratio test looks shallow.
The ratio test is the wrong test.
Each widget hides a full ncurses interaction - window creation, the key loop,
echo and cursor state, teardown - and `ballotctl` and `ballotu` between them
call these from 56 sites.
Delete the module and 56 call sites grow an ncurses dependency.
It earns its keep.

It is built as `lib/libtetrisui.a` rather than compiled into both binaries as
loose sources, so the seam between "what the screens say" and "how anything is
drawn" is named in the build, not just in the directory layout.

Every widget is MODAL: it owns the terminal and blocks in `wgetch()` until the
user answers.
That is safe only while nothing else needs servicing, which is why the header
carries the warning rather than leaving each caller to rediscover it.

## Two seams that were missing

### Daemonising

`dspawn` and `dspawn2` each carried their own copy of the double-fork
skeleton: fork, `setsid`, ignore `SIGCHLD` and `SIGHUP`, fork again, close
every descriptor, reopen 0/1/2.
Around forty lines, duplicated, differing in three deliberate ways:

| | `dspawn` | `dspawn2` |
| --- | --- | --- |
| working directory | `chdir("/")` | kept |
| `umask` | `umask(0)` | inherited |
| stdout/stderr | `/dev/null` | `var/log/<name>.err` |

Two adapters, so the seam is real by the two-adapter rule, and the differences
are exactly the parameters.
`tetrish/lib/daemonise.h` now states them as a small options struct, and
`daemonise()` holds the skeleton.

The duplication was not theoretical.
The `exit()`-versus-`_exit()` bug in the fork parents - which double-flushes
the caller's stdio buffers - existed in both copies and was fixed in one.
That is the locality argument in its purest form: with one implementation, the
fix lands once and is correct everywhere.

### The rc grammar

Described above.
One module, one definition of "directive", two entry points.

## Testing follows the seams

The interface is the test surface.
Where a test wants to reach past an interface, that is evidence the module is
the wrong shape, not a reason for a back door.

- `tests/test_db.c` drives `libtetrisdb` through `db_*` and spawns a real
  PipeRunner.
  It skips the child-process cases when java or the jar is absent, so the
  module stays testable on a machine with no JVM.
- `tests/test_logd.c` execs the real `bin/tetrislogd` and talks to it over a
  real socket.
  The daemon is tested as a daemon.
- `tests/unit/` substitutes seams by defining them in the test.
  Because the libraries are static archives, a symbol defined in the test keeps
  the real archive member out of the binary - see
  `tests/unit/support/fake_*_seams.h`.
  No mocking framework, no dependency injection, just the linker.

## Open questions

- `logd_opts_t` embeds `db_opts_t`, so `tetrislogd`'s interface leaks
  `libtetrisdb`'s.
  Defensible today, since mirroring is a first-class feature rather than a
  plugin, but if a second sink ever appears the right shape is a sink interface
  with the database behind it.
- `db_quote` is on the public interface because callers build their own SQL.
  A statement builder that made quoting unskippable would be deeper, and would
  remove the one place a caller can still get injection wrong.
- The system programs' "no libraries" link rule keeps them simple and keeps
  their shared floor thin.
  It is worth keeping deliberately rather than by accident, because relaxing it
  once will relax it permanently.
