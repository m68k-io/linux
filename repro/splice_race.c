#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>

#define NCHUNKS 8000
#define CHUNK "abcdefghijk\n"
#define CHUNKLEN 12
#define PREFIX "foobar\n"
#define DIRECT "done\n"
#define THRESHOLD (NCHUNKS - 50)

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
	volatile long *sent = mmap(NULL, sizeof(long), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	*sent = 0;

	char path[512];
	snprintf(path, sizeof(path), "%s/splice_race_dump_XXXXXX", tmpdir());
	int tmpfd = mkstemp(path);
	write(tmpfd, PREFIX, strlen(PREFIX));

	int srcpipe[2];
	pipe(srcpipe);

	pid_t feeder = fork();
	if (feeder == 0) {
		close(srcpipe[0]); close(tmpfd);
		for (int i = 0; i < NCHUNKS; i++) { write(srcpipe[1], CHUNK, CHUNKLEN); (*sent)++; }
		close(srcpipe[1]);
		_exit(0);
	}
	pid_t splicer = fork();
	if (splicer == 0) {
		close(srcpipe[1]);
		int outfd = dup(tmpfd); close(tmpfd);
		for (;;) { ssize_t n = splice(srcpipe[0], NULL, outfd, NULL, 1<<20, 0); if (n <= 0) break; }
		close(srcpipe[0]); close(outfd);
		_exit(0);
	}
	pid_t writer = fork();
	if (writer == 0) {
		close(srcpipe[0]); close(srcpipe[1]);
		int outfd = dup(tmpfd); close(tmpfd);
		while (*sent < THRESHOLD) ;
		ssize_t w = write(outfd, DIRECT, strlen(DIRECT));
		fprintf(stderr, "writer: write() returned %zd\n", w);
		close(outfd);
		_exit(0);
	}
	close(srcpipe[0]); close(srcpipe[1]);
	waitpid(feeder, NULL, 0);
	waitpid(splicer, NULL, 0);
	waitpid(writer, NULL, 0);

	struct stat st; fstat(tmpfd, &st);
	printf("final size=%lld\n", (long long)st.st_size);
	char *buf = malloc(st.st_size + 1);
	pread(tmpfd, buf, st.st_size, 0);
	buf[st.st_size] = 0;
	printf("contains \"done\": %s\n", strstr(buf, "done") ? "YES" : "NO");
	printf("last 40 bytes: %.*s\n", 40, buf + (st.st_size > 40 ? st.st_size - 40 : 0));
	close(tmpfd);
	unlink(path);
	return 0;
}
