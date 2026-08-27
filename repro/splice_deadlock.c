// Differential test for the f_pos_lock deadlock hazard introduced by
// holding out->f_pos_lock across do_splice_from().
//
// Models the ksh93 `foo | cat` with 2>&1 case that motivated the original
// report: ONE process both feeds the pipe and writes to a dup of the shared
// output file. That is the ingredient splice_race.c lacks (there the feeder
// and the writer are separate processes, so the splicer always makes
// progress and the deadlock is invisible).
//
// Expected results:
//   unpatched kernel      -> completes; may corrupt (that's the known bug)
//   naive f_pos_lock patch-> HANGS: parent stuck in D state in write(),
//                            splicer asleep in pipe_wait_readable() holding
//                            out->f_pos_lock. With CONFIG_DETECT_HUNG_TASK
//                            the blocked task is reported after the timeout.
//   correct patch         -> completes, no corruption
//
// Exits 0 = completed, 3 = timed out (deadlock suspected).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define NCHUNKS 8000
#define CHUNK "abcdefghijk\n"
#define CHUNKLEN 12
#define PREFIX "foobar\n"
#define DIRECT "done\n"
#define TIMEOUT_SECS 25

static pid_t splicer_pid;

static void on_alarm(int sig)
{
	(void)sig;
	const char *m = "\nTIMEOUT: no progress -- deadlock suspected.\n"
	                "Check `ps -eo pid,stat,wchan,comm` for D-state tasks.\n";
	write(2, m, strlen(m));
	if (splicer_pid > 0)
		kill(splicer_pid, SIGKILL);
	_exit(3);
}

/* Directory for the test files; override with TMPDIR to exercise a
 * particular filesystem (the race is filesystem-independent, but it is
 * useful to confirm that). */
static const char *tmpdir(void)
{
	const char *d = getenv("TMPDIR");
	return (d && *d) ? d : "/tmp";
}

int main(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/splice_deadlock_XXXXXX", tmpdir());
	int tmpfd = mkstemp(path);
	if (tmpfd < 0) { perror("mkstemp"); return 1; }
	if (write(tmpfd, PREFIX, strlen(PREFIX)) < 0) { perror("write"); return 1; }

	int srcpipe[2];
	if (pipe(srcpipe)) { perror("pipe"); return 1; }

	// The splicer: plays the role of `cat`, relaying pipe -> shared fd.
	splicer_pid = fork();
	if (splicer_pid == 0) {
		close(srcpipe[1]);
		int outfd = dup(tmpfd);
		close(tmpfd);
		for (;;) {
			ssize_t n = splice(srcpipe[0], NULL, outfd, NULL,
					   1 << 20, 0);
			if (n <= 0)
				break;
		}
		close(srcpipe[0]);
		close(outfd);
		_exit(0);
	}

	// The parent plays the role of `foo`: it holds the pipe write end AND a
	// dup of the shared output file, exactly as a shell function does when
	// the pipeline was set up with 2>&1.
	close(srcpipe[0]);
	int myfd = dup(tmpfd);

	signal(SIGALRM, on_alarm);
	alarm(TIMEOUT_SECS);

	for (int i = 0; i < NCHUNKS; i++) {
		if (write(srcpipe[1], CHUNK, CHUNKLEN) != CHUNKLEN) {
			perror("pipe write");
			return 1;
		}
	}

	// Let the splicer drain the pipe and park in splice() on an empty pipe.
	// That is the state where the naive patch is holding out->f_pos_lock.
	usleep(200000);

	fprintf(stderr, "parent: entering write() on shared fd ...\n");
	ssize_t w = write(myfd, DIRECT, strlen(DIRECT));
	fprintf(stderr, "parent: write() returned %zd\n", w);

	// Only now release the pipe, which is what would let the splicer finish.
	close(srcpipe[1]);
	close(myfd);
	waitpid(splicer_pid, NULL, 0);
	alarm(0);

	struct stat st;
	fstat(tmpfd, &st);
	size_t expect = strlen(PREFIX) + (size_t)NCHUNKS * CHUNKLEN + strlen(DIRECT);
	printf("completed: size=%lld expected=%zu %s\n",
	       (long long)st.st_size, expect,
	       (size_t)st.st_size == expect ? "OK" : "CORRUPT");
	close(tmpfd);
	unlink(path);
	return 0;
}
