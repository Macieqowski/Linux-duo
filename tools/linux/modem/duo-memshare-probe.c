// SPDX-License-Identifier: GPL-2.0-or-later
// duo-memshare-probe: does the Duo's modem ask Linux for memory over the QMI "memshare" service (0x34)?
// Downstream (Android) kernels run that service (drivers/soc/qcom/memshare: clients GPS, FTM, DIAG; sm8150 reserves
// 5 MiB at boot for client id 1); mainline has none. This publishes service 0x34 v1 instance 1 on QRTR and logs every
// request the modem sends. It never hands out memory. Default: log only (the modem's request gets no answer).
// --nack: answer each request with QMI failure, error 2 (no memory), to see whether the modem copes with a refusal.
// Plain AF_QIPCRTR sockets, no libqrtr: builds static with any aarch64 gcc.
// usage: duo-memshare-probe [--nack] [seconds (0 = forever)]
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define SVC 0x34
#define VER 1
#define INS 1

struct sockaddr_qrtr {
	unsigned short sq_family;
	uint32_t sq_node;
	uint32_t sq_port;
};

static int fd;
static struct sockaddr_qrtr me;
static volatile sig_atomic_t stop;

static void logts(void)
{
	char b[16];
	time_t t = time(NULL);
	strftime(b, sizeof(b), "%H:%M:%S", localtime(&t));
	printf("%s ", b);
}

static void ctrl(uint32_t cmd)
{
	uint32_t pkt[5] = { cmd, SVC, INS << 8 | VER, me.sq_node, me.sq_port };
	struct sockaddr_qrtr to = { AF_QIPCRTR, me.sq_node, QRTR_PORT_CTRL };
	if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0)
		perror("duo-memshare-probe: ctrl sendto");
}

static void onsig(int s)
{
	stop = 1;
}

static const char *name(unsigned m)
{
	switch (m) {
	case 0x20: return "alloc";
	case 0x21: return "free";
	case 0x22: return "alloc_generic";
	case 0x23: return "free_generic";
	case 0x24: return "query_size";
	}
	return "?";
}

int main(int argc, char **argv)
{
	int nack = 0, limit = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nack"))
			nack = 1;
		else
			limit = atoi(argv[i]);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("duo-memshare-probe: socket");
		return 1;
	}
	socklen_t sl = sizeof(me);
	if (getsockname(fd, (struct sockaddr *)&me, &sl) < 0) {
		perror("duo-memshare-probe: getsockname");
		return 1;
	}
	struct sigaction sa = { .sa_handler = onsig };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	struct timeval tv = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	ctrl(QRTR_TYPE_NEW_SERVER);
	logts();
	printf("published memshare 0x%x v%u inst %u at node %u port %u (%s)\n", SVC, VER, INS, me.sq_node, me.sq_port,
	       nack ? "nack" : "log only");
	time_t end = time(NULL) + limit;
	while (!stop && (!limit || time(NULL) < end)) {
		unsigned char buf[65536];
		struct sockaddr_qrtr from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("duo-memshare-probe: recvfrom");
			break;
		}
		logts();
		if (n < 7) {
			printf("short packet (%zd bytes) from node %u port %u\n", n, from.sq_node, from.sq_port);
			continue;
		}
		unsigned typ = buf[0], txn = buf[1] | buf[2] << 8, msg = buf[3] | buf[4] << 8, len = buf[5] | buf[6] << 8;
		printf("from node %u port %u: type %u txn %u msg 0x%04x (%s) len %u:", from.sq_node, from.sq_port, typ, txn,
		       msg, name(msg), len);
		for (unsigned o = 7; o + 3 <= (unsigned)n && o + 3 <= 7 + len;) {
			unsigned t = buf[o], l = buf[o + 1] | buf[o + 2] << 8;
			o += 3;
			if (l == 4 && o + 4 <= (unsigned)n)
				printf(" 0x%02x=%u", t, buf[o] | buf[o + 1] << 8 | buf[o + 2] << 16 | (unsigned)buf[o + 3] << 24);
			else if (l == 1 && o < (unsigned)n)
				printf(" 0x%02x=%u", t, buf[o]);
			else {
				printf(" 0x%02x=", t);
				for (unsigned k = 0; k < l && o + k < (unsigned)n; k++)
					printf("%02x", buf[o + k]);
			}
			o += l;
		}
		printf("\n");
		if (nack && typ == 0) {
			unsigned char r[7 + 7] = { 2, txn & 0xff, txn >> 8, msg & 0xff, msg >> 8, 7, 0,
						   0x02, 4, 0, 1, 0, 2, 0 };
			sendto(fd, r, sizeof(r), 0, (struct sockaddr *)&from, sizeof(from));
			logts();
			printf("  -> nack sent\n");
		}
	}
	ctrl(QRTR_TYPE_DEL_SERVER);
	logts();
	printf("unpublished\n");
	return 0;
}
