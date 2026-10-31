#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 4096

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

static void reply(char code, const char *text) {
	char msg[BUF_SIZE];
	size_t len = strlen(text);
	if (len > sizeof(msg) - 1)
		len = sizeof(msg) - 1;
	msg[0] = code;
	memcpy(msg + 1, text, len);
	if (!write_all(STDOUT_FILENO, msg, len + 1)) {
		const char err[] = "error: child failed to write to pipe2\n";
		write_all(STDERR_FILENO, err, sizeof(err) - 1);
		exit(EXIT_FAILURE);
	}
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
	if (argc != 2) {
		const char msg[] = "usage: child1 <file name>\n";
		write_all(STDERR_FILENO, msg, sizeof(msg) - 1);
		exit(EXIT_FAILURE);
	}

	int file = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file == -1) {
		reply('F', "error: failed to open output file\n");
		exit(EXIT_FAILURE);
	}
	reply('R', "ready\n");

	LineReader in = {.fd = STDIN_FILENO, .start = 0, .end = 0};
	char line[BUF_SIZE];
	ssize_t len;

	while ((len = read_line(&in, line, sizeof(line))) > 0) {
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
			reply('Z', "error: division by zero, child and parent are terminating\n");
			close(file);
			exit(EXIT_FAILURE);
		}
		if (!ok) {
			reply('E', "error: line must contain only int numbers\n");
			continue;
		}
		if (overflow) {
			reply('E', "error: result does not fit into int\n");
			continue;
		}
		if (count == 0) {
			reply('E', "error: empty line\n");
			continue;
		}

		size_t n = int_to_str(result, num);
		memcpy(out + out_len, " = ", 3);
		out_len += 3;
		memcpy(out + out_len, num, n);
		out_len += n;
		out[out_len++] = '\n';

		if (!write_all(file, out, out_len)) {
			reply('F', "error: failed to write to output file\n");
			close(file);
			exit(EXIT_FAILURE);
		}

		num[n] = '\n';
		num[n + 1] = '\0';
		reply('O', num);
	}

	if (len == -1) {
		reply('F', "error: child failed to read from pipe1\n");
		close(file);
		exit(EXIT_FAILURE);
	}
	if (len == -2) {
		reply('F', "error: line is too long\n");
		close(file);
		exit(EXIT_FAILURE);
	}

	if (close(file) == -1) {
		const char msg[] = "error: failed to close output file\n";
		write_all(STDERR_FILENO, msg, sizeof(msg) - 1);
		exit(EXIT_FAILURE);
	}
	return EXIT_SUCCESS;
}
