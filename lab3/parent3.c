#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <stdint.h>

#define BUF_SIZE 4096

#define MSG_NONE 0
#define MSG_LINE 1
#define MSG_END 2
#define ANS_READY 'R'
#define ANS_RESULT 'O'
#define ANS_ERROR 'E'
#define ANS_ZERO 'Z'
#define ANS_FATAL 'F'

typedef struct {
	int32_t kind;
	uint32_t len;
	char text[BUF_SIZE + 2];
} Shared;

#define PATH_SIZE 1024
#define NAME_SIZE 32

static const char CHILD_PROGRAM_NAME[] = "child3";

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

static volatile sig_atomic_t stop_requested = 0;

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
			if (errno == EINTR) {
				if (stop_requested)
					return -3;
				continue;
			}
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

static void append_uint(char *dst, unsigned long value) {
	char tmp[24];
	size_t n = 0;
	do {
		tmp[n++] = (char)('0' + value % 10);
		value /= 10;
	} while (value > 0);
	size_t len = strlen(dst);
	while (n > 0)
		dst[len++] = tmp[--n];
	dst[len] = '\0';
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

static char shm_name[NAME_SIZE];
static char req_name[NAME_SIZE];
static char ans_name[NAME_SIZE];

static Shared *shared = MAP_FAILED;
static sem_t *sem_req = SEM_FAILED;
static sem_t *sem_ans = SEM_FAILED;
static bool shm_created = false;

static pid_t child = -1;
static volatile sig_atomic_t child_exited = 0;

static void make_name(char *dst, const char *suffix) {
	strcpy(dst, "/lab3-");
	append_uint(dst, (unsigned long)getpid());
	strcat(dst, suffix);
}

static void cleanup(void) {
	if (shared != MAP_FAILED && munmap(shared, sizeof(Shared)) == -1)
		print(STDERR_FILENO, "error: munmap failed\n");
	shared = MAP_FAILED;
	if (shm_created && shm_unlink(shm_name) == -1)
		print(STDERR_FILENO, "error: shm_unlink failed\n");
	shm_created = false;

	if (sem_req != SEM_FAILED) {
		sem_close(sem_req);
		if (sem_unlink(req_name) == -1)
			print(STDERR_FILENO, "error: sem_unlink failed\n");
		sem_req = SEM_FAILED;
	}
	if (sem_ans != SEM_FAILED) {
		sem_close(sem_ans);
		if (sem_unlink(ans_name) == -1)
			print(STDERR_FILENO, "error: sem_unlink failed\n");
		sem_ans = SEM_FAILED;
	}
}

static bool send_request(int kind, const char *text, size_t len);

_Noreturn static void finish(int code) {
	if (child > 0) {
		if (!child_exited)
			send_request(MSG_END, "", 0);
		while (waitpid(child, NULL, 0) == -1) {
			if (errno != EINTR) {
				print(STDERR_FILENO, "error: failed to wait for child process\n");
				break;
			}
		}
	}
	cleanup();
	exit(code);
}

_Noreturn static void die(const char *msg) {
	print(STDERR_FILENO, msg);
	finish(EXIT_FAILURE);
}

static int create_shm(void) {
	for (int attempt = 0; attempt < 2; ++attempt) {
		int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd != -1)
			return fd;
		if (errno != EEXIST)
			return -1;
		shm_unlink(shm_name);
	}
	return -1;
}

static sem_t *create_sem(const char *name) {
	for (int attempt = 0; attempt < 2; ++attempt) {
		sem_t *sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
		if (sem != SEM_FAILED)
			return sem;
		if (errno != EEXIST)
			return SEM_FAILED;
		sem_unlink(name)
	return SEM_FAILED;
}

static void on_child_exit(int sig) {
	(void)sig;
	child_exited = 1;
	if (sem_ans != SEM_FAILED)
		sem_post(sem_ans);
}

static void on_stop(int sig) {
	(void)sig;
	stop_requested = 1;
}

static int wait_answer(void) {
	for (;;) {
		if (sem_wait(sem_ans) == -1) {
			if (errno != EINTR) {
				print(STDERR_FILENO, "error: sem_wait failed\n");
				return 0;
			}
			if (stop_requested)
				return -1;
			continue;
		}
		int kind = shared->kind;
		if (kind != MSG_NONE && kind != MSG_LINE && kind != MSG_END)
			return kind;
		if (child_exited)
			return 0;
	}
}

static bool send_request(int kind, const char *text, size_t len) {
	shared->kind = kind;
	shared->len = (uint32_t)len;
	memcpy(shared->text, text, len);
	shared->text[len] = '\0';
	return sem_post(sem_req) == 0;
}

int main(int argc, char **argv) {
	(void)argc;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = on_child_exit;
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	LineReader in = {.fd = STDIN_FILENO, .start = 0, .end = 0};
	char line[BUF_SIZE + 1];

	print(STDOUT_FILENO, "Enter file name: ");
	ssize_t len = read_line(&in, line, BUF_SIZE);
	if (len == -3)
		die("\ninterrupted\n");
	if (len == -1)
		die("error: failed to read from stdin\n");
	if (len == -2)
		die("error: file name is too long\n");
	if (len > 0)
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

	make_name(shm_name, "-shm");
	make_name(req_name, "-req");
	make_name(ans_name, "-ans");

	int shm_fd = create_shm();
	if (shm_fd == -1)
		die("error: failed to create shared memory (shm_open)\n");
	shm_created = true;

	if (ftruncate(shm_fd, (off_t)sizeof(Shared)) == -1) {
		close(shm_fd);
		die("error: failed to set shared memory size (ftruncate)\n");
	}

	shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (shared == MAP_FAILED)
		die("error: failed to map shared memory (mmap)\n");
	shared->kind = MSG_NONE;
	shared->len = 0;

	sem_req = create_sem(req_name);
	if (sem_req == SEM_FAILED)
		die("error: failed to create semaphore (sem_open)\n");
	sem_ans = create_sem(ans_name);
	if (sem_ans == SEM_FAILED)
		die("error: failed to create semaphore (sem_open)\n");

	child = fork();
	if (child == -1)
		die("error: failed to spawn new process\n");

	if (child == 0) {
		char *const args[] = {(char *)CHILD_PROGRAM_NAME, file_name, shm_name, req_name, ans_name, NULL};
		execv(child_path, args);

		print(STDERR_FILENO, "error: failed to exec child program\n");
		_exit(EXIT_FAILURE);
	}

	int answer = wait_answer();
	if (answer == -1)
		die("\ninterrupted\n");
	if (answer == 0)
		die("error: child process terminated unexpectedly\n");
	if (answer != ANS_READY) {
		print(STDERR_FILENO, shared->text);
		finish(EXIT_FAILURE);
	}

	print(STDOUT_FILENO, "Enter numbers like \"100 2 5\" (empty line or Ctrl+D to finish):\n");

	int exit_code = EXIT_SUCCESS;
	for (;;) {
		len = read_line(&in, line, BUF_SIZE);
		if (len == 0)
			break;
		if (len == -3) {
			print(STDERR_FILENO, "\ninterrupted\n");
			exit_code = EXIT_FAILURE;
			break;
		}
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

		if (child_exited) {
			print(STDERR_FILENO, "error: child process terminated unexpectedly\n");
			exit_code = EXIT_FAILURE;
			break;
		}
		if (!send_request(MSG_LINE, line, (size_t)len)) {
			print(STDERR_FILENO, "error: sem_post failed\n");
			exit_code = EXIT_FAILURE;
			break;
		}

		answer = wait_answer();
		if (answer == -1) {
			print(STDERR_FILENO, "\ninterrupted\n");
			exit_code = EXIT_FAILURE;
			break;
		}
		if (answer == 0) {
			print(STDERR_FILENO, "error: child process terminated unexpectedly\n");
			exit_code = EXIT_FAILURE;
			break;
		}

		if (answer == ANS_RESULT) {
			print(STDOUT_FILENO, "result: ");
			print(STDOUT_FILENO, shared->text);
		} else if (answer == ANS_ERROR) {
			print(STDERR_FILENO, shared->text);
		} else {
			print(STDERR_FILENO, shared->text);
			exit_code = EXIT_FAILURE;
			break;
		}
	}

	finish(exit_code);
}
