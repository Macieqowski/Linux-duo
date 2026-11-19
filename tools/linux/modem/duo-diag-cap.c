// SPDX-License-Identifier: GPL-2.0-or-later
// duo-diag-cap: capture the Duo modem's DIAG stream (F3 debug messages, events, optional log codes) through
// linux-msm diag-router, to see what the modem's RF code does just before the QLink assertion
// ("rflm_qlnk_ls_retry_cnt < 2") that kills it on every network search on mainline.
// diag-router talks to the modem over QRTR (service 4097) and serves local clients on the abstract unix socket
// "\0diag" (SOCK_SEQPACKET, one raw DIAG packet per message, no HDLC). This client sets the masks there (the router
// keeps them and pushes them to the modem when it connects), then writes every packet it gets as an HDLC frame to a
// .qmdl file (the format QXDM/SCAT/QCSuper read) and prints plain-text F3 messages (0x79) as they come. QShrink
// messages (0x92/0x99, hashed format strings) need the modem's qdsp6m.qdb offline.
// usage: duo-diag-cap [-m msgmask] [-l equip[,equip...]] [-t seconds] out.qmdl
//   -m  F3 level mask for all SSIDs (default 0x1f = low..fatal; 0 = no F3)
//   -l  enable every log code of these equipment ids (e.g. 0xb = LTE); default none
//   -t  stop after this many seconds (default: until SIGINT/SIGTERM)
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static FILE *out;
static unsigned long npkt, nf3, nqsr, nevt, nlog, nother, nbytes;

static void onsig(int s)
{
	stop = 1;
}

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t c = 0xffff;
	while (n--) {
		c ^= *p++;
		for (int i = 0; i < 8; i++)
			c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
	}
	return ~c;
}

static void put_esc(uint8_t b)
{
	if (b == 0x7e || b == 0x7d) {
		fputc(0x7d, out);
		b ^= 0x20;
	}
	fputc(b, out);
}

static void write_hdlc(const uint8_t *p, size_t n)
{
	uint16_t c = crc16(p, n);
	for (size_t i = 0; i < n; i++)
		put_esc(p[i]);
	put_esc(c & 0xff);
	put_esc(c >> 8);
	fputc(0x7e, out);
}

static int sendpkt(int fd, const void *p, size_t n, const char *what)
{
	if (send(fd, p, n, 0) != (ssize_t)n) {
		fprintf(stderr, "duo-diag-cap: send %s: %s\n", what, strerror(errno));
		return -1;
	}
	return 0;
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// modem timestamp: upper 48 bits count 1.25 ms
static double mts(const uint8_t *p)
{
	uint64_t t = 0;
	for (int i = 7; i >= 0; i--)
		t = t << 8 | p[i];
	return (t >> 16) * 1.25e-3;
}

static void show_f3(const uint8_t *p, size_t n, double now)
{
	if (n < 20)
		return;
	unsigned na = p[2], drop = p[3], line = p[12] | p[13] << 8, ssid = p[14] | p[15] << 8;
	uint32_t lvl = rd32(p + 16);
	size_t o = 20 + 4 * (size_t)na;
	if (o >= n)
		return;
	const char *fmt = (const char *)p + o;
	size_t fl = strnlen(fmt, n - o);
	const char *file = o + fl + 1 < n ? fmt + fl + 1 : "";
	char msg[512];
	// format with up to 9 integer args (F3 args are 32-bit words; %s args are not supported in F3)
	uint32_t a[9] = { 0 };
	for (unsigned i = 0; i < na && i < 9; i++)
		a[i] = rd32(p + 20 + 4 * i);
	char safe[400];
	size_t k = 0;
	for (size_t i = 0; i < fl && k + 2 < sizeof(safe); i++) {
		if (fmt[i] == '%' && i + 1 < fl && fmt[i + 1] == 's') { // never deref a modem pointer
			safe[k++] = '%';
			safe[k++] = 'x';
			i++;
			continue;
		}
		safe[k++] = fmt[i];
	}
	safe[k] = 0;
	snprintf(msg, sizeof(msg), safe, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
	printf("%10.3f m%10.3f ssid %5u l%02x%s %.*s:%u  %s\n", now, mts(p + 4), ssid, lvl, drop ? " DROP" : "",
	       (int)strnlen(file, 40), file, line, msg);
}

// 0x92 (QShrink 2): the 0x79 header, then a 32-bit hash and 32-bit args.
// 0x99 (QShrink 4, layout as SCAT parses it): cmd ts_type num/size drop ts(8) hash(4) unk(2), then args packed
// (num = high nibble, bytes per arg = low nibble); the hash is an offset into the modem's qdsp6m.qdb.
static void show_qsr(const uint8_t *p, size_t n, double now)
{
	if (p[0] == 0x92) {
		if (n < 24)
			return;
		unsigned na = p[2], line = p[12] | p[13] << 8, ssid = p[14] | p[15] << 8;
		printf("%10.3f m%10.3f ssid %5u qsr2 line %u hash 0x%08x", now, mts(p + 4), ssid, line, rd32(p + 20));
		for (unsigned i = 0; i < na && 24 + 4 * i + 4 <= n; i++)
			printf(" %x", rd32(p + 24 + 4 * i));
		printf("\n");
		return;
	}
	if (n < 18)
		return;
	unsigned na = p[2] >> 4, sz = p[2] & 15;
	printf("%10.3f m%10.3f qsr4 hash 0x%08x", now, mts(p + 4), rd32(p + 12));
	for (unsigned i = 0; i < na && sz && 18 + sz * (i + 1) <= n; i++) {
		uint32_t v = 0;
		for (unsigned b = 0; b < sz && b < 4; b++)
			v |= (uint32_t)p[18 + sz * i + b] << (8 * b);
		printf(" %x", v);
	}
	printf("\n");
}

// The router keeps the masks and pushes them to each peripheral when its control channel comes up; they are sent
// again at 15 s and 30 s anyway, in case the modem (started after us) dropped masks that arrived before its feature
// mask exchange.
static void set_masks(int fd, uint32_t msgmask, const char *logs)
{
	if (msgmask) {
		uint8_t m[7] = { 0x7d, 0x05, 0, msgmask, msgmask >> 8, msgmask >> 16, msgmask >> 24 };
		sendpkt(fd, m, sizeof(m), "msg mask");
	}
	uint8_t ev[2] = { 0x60, 0x01 };
	sendpkt(fd, ev, sizeof(ev), "event enable");
	if (!logs)
		return;
	char *copy = strdup(logs), *save = NULL;
	for (char *s = strtok_r(copy, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
		uint32_t eq = strtoul(s, NULL, 0), items = 4096; // router caps num_items to the equip's range
		uint8_t pkt[16 + 512] = { 0x73, 0, 0, 0, 3, 0, 0, 0 };
		memcpy(pkt + 8, &eq, 4);
		memcpy(pkt + 12, &items, 4);
		memset(pkt + 16, 0xff, 512);
		sendpkt(fd, pkt, sizeof(pkt), "log mask");
	}
	free(copy);
}

int main(int argc, char **argv)
{
	uint32_t msgmask = 0x1f;
	int limit = 0, opt;
	char *logs = NULL;
	while ((opt = getopt(argc, argv, "m:l:t:")) != -1) {
		if (opt == 'm')
			msgmask = strtoul(optarg, NULL, 0);
		else if (opt == 'l')
			logs = optarg;
		else if (opt == 't')
			limit = atoi(optarg);
		else
			goto usage;
	}
	if (optind + 1 != argc) {
usage:
		fprintf(stderr, "usage: duo-diag-cap [-m msgmask] [-l equip,...] [-t seconds] out.qmdl\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	out = fopen(argv[optind], "ab");
	if (!out) {
		perror("duo-diag-cap: open output");
		return 1;
	}
	int fd = -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, "\0diag", 5);
	for (int i = 0; i < 100 && !stop; i++) { // wait up to 10 s for diag-router
		fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
			break;
		if (fd >= 0)
			close(fd);
		fd = -1;
		usleep(100000);
	}
	if (fd < 0) {
		perror("duo-diag-cap: connect to diag-router");
		return 1;
	}
	struct sigaction sig = { .sa_handler = onsig };
	sigaction(SIGINT, &sig, NULL);
	sigaction(SIGTERM, &sig, NULL);

	set_masks(fd, msgmask, logs);
	double t0 = mono(), last = t0;
	int resent = 0;
	printf("duo-diag-cap: connected, msg mask 0x%x, events on, logs %s -> %s\n", msgmask, logs ? "set" : "off",
	       argv[optind]);
	while (!stop && (!limit || mono() - t0 < limit)) {
		struct pollfd pf = { fd, POLLIN };
		if (poll(&pf, 1, 1000) < 0 && errno != EINTR)
			break;
		double now = mono();
		if ((resent == 0 && now - t0 >= 15) || (resent == 1 && now - t0 >= 30)) {
			set_masks(fd, msgmask, logs);
			resent++;
			fprintf(stderr, "duo-diag-cap: %.0f s: masks sent again\n", now - t0);
		}
		if (now - last >= 10) {
			fflush(out);
			fprintf(stderr, "duo-diag-cap: %.0f s: %lu pkts %lu B (f3 %lu qsr %lu evt %lu log %lu other %lu)\n",
				now - t0, npkt, nbytes, nf3, nqsr, nevt, nlog, nother);
			last = now;
		}
		if (!(pf.revents & (POLLIN | POLLHUP)))
			continue;
		uint8_t buf[65536];
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n == 0) {
			fprintf(stderr, "duo-diag-cap: diag-router closed the socket\n");
			break;
		}
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("duo-diag-cap: recv");
			break;
		}
		npkt++;
		nbytes += n;
		write_hdlc(buf, n);
		switch (buf[0]) {
		case 0x79:
			nf3++;
			show_f3(buf, n, now - t0);
			break;
		case 0x92:
		case 0x99:
			nqsr++;
			show_qsr(buf, n, now - t0);
			break;
		case 0x60:
			nevt++;
			break;
		case 0x10:
			nlog++;
			break;
		default:
			nother++;
			printf("%10.3f pkt 0x%02x len %zd\n", now - t0, buf[0], n);
		}
	}
	fflush(out);
	fsync(fileno(out));
	fprintf(stderr, "duo-diag-cap: done: %lu pkts %lu B (f3 %lu qsr %lu evt %lu log %lu other %lu)\n", npkt, nbytes, nf3,
		nqsr, nevt, nlog, nother);
	return 0;
}
