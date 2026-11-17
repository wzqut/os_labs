#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define MAX_THREADS 4096
#define MAX_N 1000000000ULL
#define MAX_REPEATS 100
#define CUTOFF 32

typedef unsigned long long ull;

static void print(int fd, const char *s) {
	size_t len = 0;
	while (s[len] != '\0')
		++len;
	while (len > 0) {
		ssize_t w = write(fd, s, len);
		if (w == -1 && errno == EINTR)
			continue;
		if (w <= 0)
			return;
		s += w;
		len -= (size_t)w;
	}
}

static bool str_eq(const char *a, const char *b) {
	while (*a != '\0' && *a == *b)
		++a, ++b;
	return *a == *b;
}

static void put_num(int fd, ull v, int digits) {
	char b[32] = {0};
	int i = 31;
	for (int k = 0; k < digits; ++k, v /= 10)
		b[--i] = (char)('0' + v % 10);
	if (digits > 0)
		b[--i] = '.';
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v > 0);
	print(fd, b + i);
}

_Noreturn static void die(const char *msg) {
	print(STDERR_FILENO, "error: ");
	print(STDERR_FILENO, msg);
	print(STDERR_FILENO, "\n");
	_exit(1);
}

static long long now_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		die("clock_gettime failed");
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int *alloc_ints(size_t n) {
	void *p = mmap(NULL, (n ? n : 1) * sizeof(int), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		die("not enough memory");
	return (int *)p;
}

static void copy_ints(int *dst, const int *src, size_t n) {
	for (size_t i = 0; i < n; ++i)
		dst[i] = src[i];
}

static void insertion_sort(int *a, size_t n) {
	for (size_t i = 1; i < n; ++i) {
		int x = a[i];
		size_t j = i;
		for (; j > 0 && a[j - 1] > x; --j)
			a[j] = a[j - 1];
		a[j] = x;
	}
}

static void merge(const int *a, size_t na, const int *b, size_t nb, int *out) {
	size_t i = 0, j = 0, k = 0;
	while (i < na && j < nb)
		out[k++] = (a[i] <= b[j]) ? a[i++] : b[j++];
	while (i < na)
		out[k++] = a[i++];
	while (j < nb)
		out[k++] = b[j++];
}

static void merge_sort(int *a, int *tmp, size_t n) {
	if (n <= CUTOFF) {
		insertion_sort(a, n);
		return;
	}
	size_t half = n / 2;
	merge_sort(a, tmp, half);
	merge_sort(a + half, tmp + half, n - half);
	if (a[half - 1] <= a[half])
		return;
	merge(a, half, a + half, n - half, tmp);
	copy_ints(a, tmp, n);
}

static size_t co_rank(size_t k, const int *a, size_t na, const int *b, size_t nb) {
	size_t lo = k > nb ? k - nb : 0, hi = k < na ? k : na;
	while (lo < hi) {
		size_t i = lo + (hi - lo) / 2;
		if (a[i] <= b[k - i - 1])
			lo = i + 1;
		else
			hi = i;
	}
	return lo;
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static size_t barrier_total, barrier_waiting, barrier_phase;

static void barrier
_wait(void) {
	pthread_mutex_lock(&mutex);
	size_t phase = barrier_phase;
	if (++barrier_waiting == barrier_total) {
		barrier_waiting = 0;
		barrier_phase++;
		pthread_cond_broadcast(&cond);
	}
	while (phase == barrier_phase)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);
}

static int *g_a, *g_tmp;
static size_t g_threads;
static size_t bounds[MAX_THREADS + 1];
static bool g_abort;

static size_t bound(size_t block) {
	return bounds[block < g_threads ? block : g_threads];
}

static void *worker(void *arg) {
	size_t id = (size_t)(uintptr_t)arg;
	barrier_wait();
	if (g_abort)
		return NULL;
	size_t lo = bounds[id], hi = bounds[id + 1];

	merge_sort(g_a + lo, g_tmp + lo, hi - lo);
	barrier_wait();

	int *src = g_a, *dst = g_tmp;
	for (size_t width = 1; width < g_threads; width *= 2) {
		size_t pair = id / (2 * width);
		size_t start = bound(2 * pair * width), mid = bound((2 * pair + 1) * width);
		size_t end = bound((2 * pair + 2) * width);
		size_t nl = mid - start, nr = end - mid;
		size_t k_lo = lo - start, k_hi = hi - start;
		size_t i_lo = co_rank(k_lo, src + start, nl, src + mid, nr);
		size_t i_hi = co_rank(k_hi, src + start, nl, src + mid, nr);
		merge(src + start + i_lo, i_hi - i_lo, src + mid + (k_lo - i_lo), (k_hi - i_hi) - (k_lo - i_lo), dst + lo);
		barrier_wait();
		int *t = src;
		src = dst;
		dst = t;
	}
	if (src != g_a)
		copy_ints(g_a + lo, src + lo, hi - lo);
	return NULL;
}

static bool parallel_merge_sort(int *a, int *tmp, size_t n, size_t threads) {
	static pthread_t ids[MAX_THREADS];
	g_a = a, g_tmp = tmp, g_threads = threads, g_abort = false;
	barrier_total = threads;
	for (size_t i = 0; i <= threads; ++i)
		bounds[i] = (size_t)((ull)n * i / threads);

	size_t created = 1;
	while (created < threads && pthread_create(&ids[created], NULL, worker, (void *)(uintptr_t)created) == 0)
		created++;
	if (created < threads) {
		pthread_mutex_lock(&mutex);
		g_abort = true;
		barrier_total = created;
		pthread_mutex_unlock(&mutex);
	}
	worker(NULL);

	bool ok = (created == threads);
	for (size_t i = 1; i < created; ++i)
		ok &= (pthread_join(ids[i], NULL) == 0);
	return ok;
}

static bool parse_size(const char *s, ull max, ull *res) {
	ull v = 0;
	if (*s == '\0')
		return false;
	for (; *s != '\0'; ++s) {
		if (*s < '0' || *s > '9' || v > (max - (ull)(*s - '0')) / 10)
			return false;
		v = v * 10 + (ull)(*s - '0');
	}
	*res = v;
	return true;
}

static uint64_t rng = 88172645463325252ULL;

static uint64_t rng_next(void) {
	rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
	return rng * 2685821657736338717ULL;
}

static void generate(int *a, size_t n, int mode) {
	for (size_t i = 0; i < n; ++i) {
		long long half = (long long)(n / 2), pos = (long long)i;
		a[i] = mode == 0 ? (int)(uint32_t)(rng_next() >> 32)
		     : mode == 1 ? (int)(pos - half)
		     : mode == 2 ? (int)(half - pos)
		                 : (int)((rng_next() >> 32) % 10);
	}
}

static bool is_space(char ch) {
	return ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
}

static int *read_file(const char *path, size_t *out_n) {
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd == -1)
		die("cannot open input file");
	if (fstat(fd, &st) == -1)
		die("cannot stat input file");
	size_t len = (size_t)st.st_size, n = 0, pos = 0;
	const char *text = len ? mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0) : "";
	if (text == MAP_FAILED)
		die("cannot read input file");
	int *a = alloc_ints(len / 2 + 1);

	for (;;) {
		while (pos < len && is_space(text[pos]))
			++pos;
		if (pos == len)
			break;
		bool neg = text[pos] == '-';
		if (neg || text[pos] == '+')
			++pos;
		long long v = 0;
		size_t digits = 0;
		for (; pos < len && text[pos] >= '0' && text[pos] <= '9'; ++pos, ++digits) {
			v = v * 10 + (text[pos] - '0');
			if (v > (long long)INT_MAX + 1)
				die("number in input file does not fit into int");
		}
		if (digits == 0 || (pos < len && !is_space(text[pos])))
			die("input file must contain only int numbers");
		if (neg)
			v = -v;
		if (v > INT_MAX)
			die("number in input file does not fit into int");
		a[n++] = (int)v;
	}
	close(fd);
	*out_n = n;
	return a;
}

static void usage(void) {
	print(STDERR_FILENO,
	      "usage: ./lab2 -t THREADS [-n N] [-g random|sorted|reversed|few] [-i FILE] [-r REPEATS] [-p]\n"
	      "  -t  max number of threads working at the same time (1..4096)\n"
	      "  -n  array size (default 10000000)\n"
	      "  -g  kind of generated data (default random)\n"
	      "  -i  read int numbers from FILE instead of generating\n"
	      "  -r  repeat each sort REPEATS times, print the median time (default 1)\n"
	      "  -p  print the sorted array (statistics go to stderr)\n");
	_exit(1);
}

static void sort_times(ull *t, size_t n) {
	for (size_t i = 1; i < n; ++i) {
		ull x = t[i];
		size_t j = i;
		for (; j > 0 && t[j - 1] > x; --j)
			t[j] = t[j - 1];
		t[j] = x;
	}
}

int main(int argc, char *argv[]) {
	static const char *const kinds[] = {"random", "sorted", "reversed", "few"};
	ull threads = 0, n = 10000000, repeats = 1;
	int mode = 0;
	const char *input = NULL, *mode_name = kinds[0];
	bool print_sorted = false;

	for (int i = 1; i < argc; ++i) {
		const char *key = argv[i];
		if (str_eq(key, "-p")) {
			print_sorted = true;
			continue;
		}
		if (i + 1 >= argc)
			usage();
		const char *val = argv[++i];
		if (str_eq(key, "-t")) {
			if (!parse_size(val, MAX_THREADS, &threads) || threads == 0)
				die("-t must be a number from 1 to 4096");
		} else if (str_eq(key, "-n")) {
			if (!parse_size(val, MAX_N, &n))
				die("-n must be a number from 0 to 1000000000");
		} else if (str_eq(key, "-r")) {
			if (!parse_size(val, MAX_REPEATS, &repeats) || repeats == 0)
				die("-r must be a number from 1 to 100");
		} else if (str_eq(key, "-i")) {
			input = val;
		} else if (str_eq(key, "-g")) {
			for (mode = 0; mode < 4 && !str_eq(val, kinds[mode]); ++mode)
				;
			if (mode == 4)
				die("-g must be random, sorted, reversed or few");
			mode_name = val;
		} else {
			usage();
		}
	}
	if (threads == 0)
		usage();

	size_t count = (size_t)n;
	int *data;
	if (input != NULL) {
		data = read_file(input, &count);
		mode_name = "file";
	} else {
		data = alloc_ints(count);
		generate(data, count, mode);
	}
	int *seq = alloc_ints(count), *par = alloc_ints(count), *tmp = alloc_ints(count);
	for (size_t i = 0; i < count; ++i)
		seq[i] = par[i] = tmp[i] = 0;

	ull seq_times[MAX_REPEATS], par_times[MAX_REPEATS];
	for (size_t r = 0; r < repeats; ++r) {
		copy_ints(seq, data, count);
		long long t0 = now_ns();
		merge_sort(seq, tmp, count);
		long long t1 = now_ns();
		copy_ints(par, data, count);
		long long t2 = now_ns();
		if (!parallel_merge_sort(par, tmp, count, (size_t)threads))
			die("failed to create threads (try fewer threads with -t)");
		long long t3 = now_ns();
		seq_times[r] = (ull)(t1 - t0);
		par_times[r] = (ull)(t3 - t2);
	}
	for (size_t i = 0; i < count; ++i)
		if ((i > 0 && seq[i - 1] > seq[i]) || seq[i] != par[i])
			die("check failed: parallel result differs from sequential");

	sort_times(seq_times, (size_t)repeats);
	sort_times(par_times, (size_t)repeats);
	ull ts = seq_times[repeats / 2], tp = par_times[repeats / 2];
	if (tp == 0)
		tp = 1;

	int out = print_sorted ? STDERR_FILENO : STDOUT_FILENO;
	print(out, "logical_cpus=");
	put_num(out, (ull)sysconf(_SC_NPROCESSORS_ONLN), 0);
	print(out, " n=");
	put_num(out, count, 0);
	print(out, " input=");
	print(out, mode_name);
	print(out, " repeats=");
	put_num(out, repeats, 0);
	print(out, "\nsequential: time_ms=");
	put_num(out, ts / 1000, 3);
	print(out, "\nparallel: threads=");
	put_num(out, threads, 0);
	print(out, " time_ms=");
	put_num(out, tp / 1000, 3);
	print(out, " speedup=");
	put_num(out, ts * 1000 / tp, 3);
	print(out, " efficiency=");
	put_num(out, ts * 10000 / (tp * threads), 4);
	print(out, "\ncheck: OK\n");

	if (print_sorted)
		for (size_t i = 0; i < count; ++i) {
			if (par[i] < 0)
				print(STDOUT_FILENO, "-");
			put_num(STDOUT_FILENO, par[i] < 0 ? (ull)(-(long long)par[i]) : (ull)par[i], 0);
			print(STDOUT_FILENO, "\n");
		}
	return 0;
}
