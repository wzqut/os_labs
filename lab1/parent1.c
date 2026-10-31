#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdint.h>
#endif

#define BUF_SIZE 4096
#define PATH_SIZE 1024

static const char CHILD_PROGRAM_NAME[] = "child1";

static bool write_all(int fd, const char *buf, size_t len) {
	while (len > 0) {
		ssize_t written = write(fd, buf, len);
		if (written == -1) {
			if (errno == EINTR)
				continue;
			return false;
		}
		buf += written;
		len -= (size_t)written;
	}
	return true;
}

static void print(int fd, const char *str) {
	write_all(fd, str, strlen(str));
}

_Noreturn static void die(const char *msg) {
	print(STDERR_FILENO, msg);
	exit(EXIT_FAILURE);
}

typedef struct {
	int fd;
	char buf[BUF_SIZE];
	size_t start;
	size_t end;
} LineReader;

static ssize_t read_line(LineReader *r, char *line, size_t cap) {
	for (;;) {
		for (size_t i = r->start; i < r->end; ++i) {
			if (r->buf[i] == '\n') {
				size_t len = i - r->start + 1;
				if (len >= cap)
					return -2;
				memcpy(line, r->buf + r->start, len);
				line[len] = '\0';
				r->start = i + 1;
				return (ssize_t)len;
			}
		}

		size_t rest = r->end - r->start;
		memmove(r->buf, r->buf + r->start, rest);
		r->start = 0;
		r->end = rest;
		if (r->end == sizeof(r->buf))
			return -2;

		ssize_t bytes = read(r->fd, r->buf + r->end, sizeof(r->buf) - r->end);
		if (bytes == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (bytes == 0) {
			if (rest == 0)
				return 0;
			if (rest >= cap)
				return -2;
			memcpy(line, r->buf, rest);
			line[rest] = '\0';
			r->end = 0;
			return (ssize_t)rest;
		}
		r->end += (size_t)bytes;
	}
}

static void trim_newline(char *line) {
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
}

static bool get_program_path(char *buf, size_t size, const char *argv0) {
#ifdef __APPLE__
	uint32_t apple_size = (uint32_t)size;
	if (_NSGetExecutablePath(buf, &apple_size) == 0)
		return true;
#else
	ssize_t n = readlink("/proc/self/exe", buf, size - 1);
	if (n != -1) {
		buf[n] = '\0';
		return true;
	}
#endif
	if (argv0 == NULL || strchr(argv0, '/') == NULL || strlen(argv0) >= size)
		return false;
	strcpy(buf, argv0);
	return true;
}

_Noreturn static void finish(pid_t child, int code) {
	if (waitpid(child, NULL, 0) == -1)
		print(STDERR_FILENO, "error: failed to wait for child process\n");
	exit(code);
}

int main(int argc, char **argv) {
	(void)argc;
	signal(SIGPIPE, SIG_IGN);

	LineReader in = {.fd = STDIN_FILENO, .start = 0, .end = 0};
	char line[BUF_SIZE + 1];

	print(STDOUT_FILENO, "Enter file name: ");
	ssize_t len = read_line(&in, line, BUF_SIZE);
	if (len == -1)
		die("error: failed to read from stdin\n");
	if (len == -2)
		die("error: file name is too long\n");
	trim_newline(line);
	if (len == 0 || line[0] == '\0')
		die("error: file name is empty\n");

	char file_name[BUF_SIZE];
	strcpy(file_name, line);

	char child_path[PATH_SIZE];
	{
		if (!get_program_path(child_path, sizeof(child_path), argv[0]))
			die("error: failed to read full program path\n");
		size_t n = strlen(child_path);
		while (n > 0 && child_path[n] != '/')
			--n;
		child_path[n + 1] = '\0';

		if (strlen(child_path) + sizeof(CHILD_PROGRAM_NAME) > sizeof(child_path))
			die("error: program path is too long\n");
		strcat(child_path, CHILD_PROGRAM_NAME);
	}

	int pipe1[2];
	if (pipe(pipe1) == -1)
		die("error: failed to create pipe1\n");

	int pipe2[2];
	if (pipe(pipe2) == -1)
		die("error: failed to create pipe2\n");

	const pid_t child = fork();
	if (child == -1)
		die("error: failed to spawn new process\n");

	if (child == 0) {
		close(pipe1[1]);
		close(pipe2[0]);

		if (dup2(pipe1[0], STDIN_FILENO) == -1 || dup2(pipe2[1], STDOUT_FILENO) == -1) {
			print(STDERR_FILENO, "error: failed to redirect child streams\n");
			_exit(EXIT_FAILURE);
		}
		close(pipe1[0]);
		close(pipe2[1]);

		char *const args[] = {(char *)CHILD_PROGRAM_NAME, file_name, NULL};
		execv(child_path, args);

		print(STDERR_FILENO, "error: failed to exec child program\n");
		_exit(EXIT_FAILURE);
	}

	close(pipe1[0]);
	close(pipe2[1]);

	LineReader from_child = {.fd = pipe2[0], .start = 0, .end = 0};
	char answer[BUF_SIZE];

	len = read_line(&from_child, answer, sizeof(answer));
	if (len <= 0) {
		print(STDERR_FILENO, "error: child process terminated unexpectedly\n");
		finish(child, EXIT_FAILURE);
	}
	if (answer[0] != 'R') {
		print(STDERR_FILENO, answer + 1);
		finish(child, EXIT_FAILURE);
	}

	print(STDOUT_FILENO, "Enter numbers like \"100 2 5\" (empty line or Ctrl+D to finish):\n");

	int exit_code = EXIT_SUCCESS;
	for (;;) {
		len = read_line(&in, line, BUF_SIZE);
		if (len == 0)
			break;
		if (len == -1) {
			print(STDERR_FILENO, "error: failed to read from stdin\n");
			exit_code = EXIT_FAILURE;
			break;
		}
		if (len == -2) {
			print(STDERR_FILENO, "error: line is too long\n");
			exit_code = EXIT_FAILURE;
			break;
		}
		if (line[0] == '\n')
			break;

		if (line[len - 1] != '\n') {
			line[len++] = '\n';
			line[len] = '\0';
		}

		if (!write_all(pipe1[1], line, (size_t)len)) {
			print(STDERR_FILENO, "error: failed to write to pipe1\n");
			exit_code = EXIT_FAILURE;
			break;
		}

		len = read_line(&from_child, answer, sizeof(answer));
		if (len <= 0) {
			print(STDERR_FILENO, "error: child process terminated unexpectedly\n");
			exit_code = EXIT_FAILURE;
			break;
		}

		if (answer[0] == 'O') {
			print(STDOUT_FILENO, "result: ");
			print(STDOUT_FILENO, answer + 1);
		} else if (answer[0] == 'E') {
			print(STDERR_FILENO, answer + 1);
		} else {
			print(STDERR_FILENO, answer + 1);
			exit_code = EXIT_FAILURE;
			break;
		}
	}

	close(pipe1[1]);
	close(pipe2[0]);
	finish(child, exit_code);
}
