#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

static Shared *shared = MAP_FAILED;
static sem_t *sem_req = SEM_FAILED;
static sem_t *sem_ans = SEM_FAILED;
static int file = -1;

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

static void print_err(const char *msg) {
	write_all(STDERR_FILENO, msg, strlen(msg));
}

_Noreturn static void finish(int code) {
	if (file != -1 && close(file) == -1) {
		print_err("error: failed to close output file\n");
		code = EXIT_FAILURE;
	}
	if (shared != MAP_FAILED)
		munmap(shared, sizeof(Shared));
	if (sem_req != SEM_FAILED)
		sem_close(sem_req);
	if (sem_ans != SEM_FAILED)
		sem_close(sem_ans);
	exit(code);
}

static void reply(char code, const char *text) {
	size_t len = strlen(text);
	if (len > BUF_SIZE)
		len = BUF_SIZE;
	memcpy(shared->text, text, len);
	shared->text[len] = '\0';
	shared->len = (uint32_t)len;
	shared->kind = code;
	if (sem_post(sem_ans) == -1) {
		print_err("error: child failed to post semaphore\n");
		finish(EXIT_FAILURE);
	}
}

static bool wait_request(char *line, size_t cap) {
	while (sem_wait(sem_req) == -1) {
		if (errno != EINTR) {
			print_err("error: child failed to wait for semaphore\n");
			finish(EXIT_FAILURE);
		}
	}
	if (shared->kind != MSG_LINE)
		return false;
	size_t len = shared->len;
	if (len >= cap) {
		reply(ANS_FATAL, "error: bad message in shared memory\n");
		finish(EXIT_FAILURE);
	}
	memcpy(line, shared->text, len);
	line[len] = '\0';
	return true;
}

static bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool parse_int(const char *s, size_t *pos, int *out) {
	size_t i = *pos;
	bool negative = false;
	if (s[i] == '-' || s[i] == '+') {
		negative = (s[i] == '-');
		++i;
	}
	if (s[i] < '0' || s[i] > '9')
		return false;

	long long value = 0;
	while (s[i] >= '0' && s[i] <= '9') {
		value = value * 10 + (s[i] - '0');
		if (value > (long long)INT_MAX + 1)
			return false;
		++i;
	}
	if (!is_space(s[i]) && s[i] != '\0')
		return false;

	if (negative)
		value = -value;
	if (value > INT_MAX || value < INT_MIN)
		return false;

	*out = (int)value;
	*pos = i;
	return true;
}

static size_t int_to_str(int value, char *out) {
	char tmp[16];
	size_t n = 0;
	long long v = value;
	bool negative = v < 0;
	if (negative)
		v = -v;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v > 0);

	size_t len = 0;
	if (negative)
		out[len++] = '-';
	while (n > 0)
		out[len++] = tmp[--n];
	out[len] = '\0';
	return len;
}

int main(int argc, char **argv) {
	if (argc != 5) {
		print_err("usage: child3 <file name> <shm name> <request sem name> <answer sem name>\n"
		          "child3 is started by parent3, run ./parent3 instead\n");
		exit(EXIT_FAILURE);
	}

	int shm_fd = shm_open(argv[2], O_RDWR, 0);
	if (shm_fd == -1) {
		print_err("error: child failed to open shared memory (shm_open)\n");
		exit(EXIT_FAILURE);
	}
	shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (shared == MAP_FAILED) {
		print_err("error: child failed to map shared memory (mmap)\n");
		exit(EXIT_FAILURE);
	}

	sem_req = sem_open(argv[3], 0);
	sem_ans = sem_open(argv[4], 0);
	if (sem_req == SEM_FAILED || sem_ans == SEM_FAILED) {
		print_err("error: child failed to open semaphore (sem_open)\n");
		finish(EXIT_FAILURE);
	}

	file = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file == -1) {
		reply(ANS_FATAL, "error: failed to open output file\n");
		finish(EXIT_FAILURE);
	}
	reply(ANS_READY, "ready\n");

	char line[BUF_SIZE + 2];

	while (wait_request(line, sizeof(line))) {
		size_t pos = 0;
		int count = 0;
		int result = 0;
		bool ok = true;
		bool zero = false;
		bool overflow = false;

		char out[BUF_SIZE * 2 + 32];
		size_t out_len = 0;
		char num[16];

		for (;;) {
			while (is_space(line[pos]))
				++pos;
			if (line[pos] == '\0')
				break;

			int value;
			if (!parse_int(line, &pos, &value)) {
				ok = false;
				break;
			}

			if (count == 0) {
				result = value;
			} else {
				if (value == 0) {
					zero = true;
					break;
				}
				if (result == INT_MIN && value == -1) {
					overflow = true;
					break;
				}
				result /= value;
				memcpy(out + out_len, " / ", 3);
				out_len += 3;
			}
			size_t n = int_to_str(value, num);
			memcpy(out + out_len, num, n);
			out_len += n;
			++count;
		}

		if (zero) {
			reply(ANS_ZERO, "error: division by zero, child and parent are terminating\n");
			finish(EXIT_FAILURE);
		}
		if (!ok) {
			reply(ANS_ERROR, "error: line must contain only int numbers\n");
			continue;
		}
		if (overflow) {
			reply(ANS_ERROR, "error: result does not fit into int\n");
			continue;
		}
		if (count == 0) {
			reply(ANS_ERROR, "error: empty line\n");
			continue;
		}

		size_t n = int_to_str(result, num);
		memcpy(out + out_len, " = ", 3);
		out_len += 3;
		memcpy(out + out_len, num, n);
		out_len += n;
		out[out_len++] = '\n';

		if (!write_all(file, out, out_len)) {
			reply(ANS_FATAL, "error: failed to write to output file\n");
			finish(EXIT_FAILURE);
		}

		num[n] = '\n';
		num[n + 1] = '\0';
		reply(ANS_RESULT, num);
	}

	finish(EXIT_SUCCESS);
}
