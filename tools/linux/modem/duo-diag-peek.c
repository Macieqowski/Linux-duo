// SPDX-License-Identifier: GPL-2.0-or-later
// duo-diag-peek: read the modem's memory with the legacy DIAG PEEKB command (0x02), through diag-router, to read the
// modem's code as it runs (its compressed CLADE "low" code region cannot be unpacked offline). Read-only: no POKE.
// Production modem builds usually reject PEEK (diag replies 0x13 "bad command"); then this prints that and stops.
// A PEEK of an address the modem has not mapped faults the modem, so only read addresses known to be mapped.
// usage: duo-diag-peek ADDR [LEN [out.bin]]   (LEN default 16; with out.bin the bytes go to the file, else hex dump)
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: duo-diag-peek ADDR [LEN [out.bin]]\n");
		return 2;
	}
	uint32_t addr = strtoul(argv[1], NULL, 0);
	uint32_t len = argc > 2 ? strtoul(argv[2], NULL, 0) : 16;
	FILE *out = argc > 3 ? fopen(argv[3], "wb") : NULL;
	if (argc > 3 && !out) {
		perror("duo-diag-peek: open output");
		return 1;
	}
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, "\0diag", 5);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		perror("duo-diag-peek: connect to diag-router");
		return 1;
	}
	static uint8_t rsp[65536];
	for (uint32_t off = 0; off < len; off += 16) {
		uint32_t a = addr + off;
		uint16_t n = len - off < 16 ? len - off : 16;
		uint8_t req[7] = { 0x02, a, a >> 8, a >> 16, a >> 24, n, n >> 8 };
		if (send(fd, req, sizeof(req), 0) != sizeof(req)) {
			perror("duo-diag-peek: send");
			return 1;
		}
		double t0 = mono();
		int got = 0;
		while (!got && mono() - t0 < 3) {
			struct pollfd pf = { .fd = fd, .events = POLLIN };
			if (poll(&pf, 1, 200) <= 0)
				continue;
			ssize_t r = recv(fd, rsp, sizeof(rsp), 0);
			if (r <= 0) {
				fprintf(stderr, "duo-diag-peek: diag-router closed the socket\n");
				return 1;
			}
			if (r >= 8 && rsp[0] >= 0x13 && rsp[0] <= 0x15 && rsp[1] == 0x02 && !memcmp(rsp + 2, req + 1, 4)) {
				fprintf(stderr, "duo-diag-peek: 0x%08x: modem rejected PEEK (diag error 0x%02x)\n", a, rsp[0]);
				return 1;
			}
			if (r >= 7 + n && rsp[0] == 0x02 && !memcmp(rsp + 1, req + 1, 4)) {
				if (out)
					fwrite(rsp + 7, 1, n, out);
				else {
					printf("%08x:", a);
					for (int i = 0; i < n; i++)
						printf(" %02x", rsp[7 + i]);
					printf("  ");
					for (int i = 0; i < n; i++)
						putchar(rsp[7 + i] >= 32 && rsp[7 + i] < 127 ? rsp[7 + i] : '.');
					putchar('\n');
				}
				got = 1;
			}
		}
		if (!got) {
			fprintf(stderr, "duo-diag-peek: 0x%08x: no reply in 3 s\n", a);
			return 1;
		}
	}
	if (out)
		fclose(out);
	return 0;
}
