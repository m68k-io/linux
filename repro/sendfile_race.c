#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define NCHUNKS 300000
#define CHUNK "abcdefghijk\n"
#define CHUNKLEN 12
#define PREFIX "foobar\n"
#define DIRECT "done\n"

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
	char srcpath[512];
	snprintf(srcpath, sizeof(srcpath), "%s/sendfile_src_XXXXXX", tmpdir());
	int srcfd = mkstemp(srcpath);
	{
		char *big = malloc((size_t)NCHUNKS * CHUNKLEN);
		for (int i = 0; i < NCHUNKS; i++)
			memcpy(big + (size_t)i * CHUNKLEN, CHUNK, CHUNKLEN);
		write(srcfd, big, (size_t)NCHUNKS * CHUNKLEN);
		free(big);
	}
	size_t srclen = (size_t)NCHUNKS * CHUNKLEN;

	char path[512];
	snprintf(path, sizeof(path), "%s/sendfile_race_XXXXXX", tmpdir());
	int tmpfd = mkstemp(path);
	write(tmpfd, PREFIX, strlen(PREFIX));

	volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	*go = 0;

	pid_t relayer = fork();
	if (relayer == 0) {
		int in = open(srcpath, O_RDONLY);
		int outfd = dup(tmpfd); close(tmpfd);
		while (!*go) ;
		off_t off = 0;
		size_t remaining = srclen;
		while (remaining > 0) {
			ssize_t n = sendfile(outfd, in, &off, remaining);
			if (n <= 0) { fprintf(stderr, "relayer: sendfile failed: %s\n", strerror(errno)); break; }
			remaining -= n;
		}
		close(in); close(outfd);
		_exit(0);
	}

	pid_t writer = fork();
	if (writer == 0) {
		int outfd = dup(tmpfd); close(tmpfd);
		while (!*go) ;
		ssize_t w = write(outfd, DIRECT, strlen(DIRECT));
		fprintf(stderr, "writer: write() returned %zd\n", w);
		close(outfd);
		_exit(0);
	}

	close(tmpfd);
	*go = 1;
	waitpid(relayer, NULL, 0);
	waitpid(writer, NULL, 0);
	unlink(srcpath);

	int fd = open(path, O_RDONLY);
	struct stat st; fstat(fd, &st);
	size_t expected = strlen(PREFIX) + srclen + strlen(DIRECT);
	printf("final size=%lld (expected %zu)\n", (long long)st.st_size, expected);
	char *buf = malloc(st.st_size + 1);
	pread(fd, buf, st.st_size, 0);
	buf[st.st_size] = 0;
	printf("contains \"done\": %s\n", strstr(buf, "done") ? "YES" : "NO");
	close(fd);
	unlink(path);
	return 0;
}
