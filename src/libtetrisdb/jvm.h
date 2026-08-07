#ifndef LIBTETRISDB_JVM_H
#define LIBTETRISDB_JVM_H

/*
 * jvm.h - the java binary and the jar, checked once for both spawn paths.
 *
 * ADR 0001 accepted running two runners on the condition that this did not
 * become two of everything: "Resolving and validating the java and jar pair
 * belongs in libtetrisdb, shared by tdb_start() and by whatever launches the
 * SocketRunner, so the version trap is fixed once even though the runners stay
 * separate." This file is that promise, kept in the one place both callers
 * already are.
 *
 * Private on purpose. A caller who wants to know whether the database is
 * usable should try to use it - see the note in runner.h about health checks
 * that nothing may cache.
 */

/*
 * Is there a readable jar, and a java that runs? Returns 0, or -1 with a
 * message on stderr naming which of the two failed, because they are different
 * problems with different fixes (install a JDK / run `ant dist` in db/).
 *
 * WHAT THIS DOES NOT CATCH, and the reason it is worth saying out loud: a jar
 * built by a NEWER JDK than the java on PATH passes both checks and then fails
 * at class-load time, which is the trap this repository has already been bitten
 * by (jar built under 25, PATH resolving 17). Reading the class-file version
 * out of the archive is the real fix and is not done here. What is done instead
 * is at the other end - tdb_runner_wait() reports the child's exit status and
 * names this as the likely cause, so the failure is at least legible.
 */
int tdb_jvm_check(const char *java, const char *jar);

#endif /* LIBTETRISDB_JVM_H */
