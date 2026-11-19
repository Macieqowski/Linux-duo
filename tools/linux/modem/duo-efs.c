// SPDX-License-Identifier: GPL-2.0-or-later
// duo-efs: read-only access to the Duo modem's EFS (its own flash file system with NV items and config files) over
// DIAG, through linux-msm diag-router (abstract unix socket "\0diag", one raw DIAG packet per message), with the
// EFS2 diag subsystem commands (DIAG_SUBSYS_CMD 0x4b, subsystem 19 = FS) that QPST/QXDM/QCSuper use.
// It never writes: only STAT, OPENDIR/READDIR/CLOSEDIR, OPEN(O_RDONLY)/READ/CLOSE and GET (item files).
// usage: duo-efs [-s subsys] stat PATH... | ls DIR... | cat PATH...
// diag-router sends every DIAG packet to every client, so the replies are picked out of the stream by their header.
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

enum {
	EFS_OPEN = 2,
	EFS_CLOSE = 3,
	EFS_READ = 4,
	EFS_OPENDIR = 11,
	EFS_READDIR = 12,
	EFS_CLOSEDIR = 13,
	EFS_STAT = 15,
	EFS_GET = 39,
};

static int fd;
static unsigned subsys = 19;
static uint8_t rsp[65536];

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static const char *efserr(int32_t e)
{
	static const char *fs[] = { "", "FS_DIAG_INCONSISTENT_STATE", "FS_DIAG_INVALID_SEQ_NO", "FS_DIAG_DIR_NOT_OPEN",
				    "FS_DIAG_DIRENT_NOT_FOUND", "FS_DIAG_INVALID_PATH", "FS_DIAG_PATH_TOO_LONG",
				    "FS_DIAG_TOO_MANY_OPEN_DIRS", "FS_DIAG_INVALID_DIR_ENTRY",
				    "FS_DIAG_TOO_MANY_OPEN_FILES", "FS_DIAG_UNKNOWN_FILETYPE", "FS_DIAG_NOT_NAND_FLASH",
				    "FS_DIAG_UNAVAILABLE_INFO" };
	if (e > 0x40000000 && e <= 0x4000000c)
		return fs[e - 0x40000000];
	return strerror(e);
}

// send cmd (header + body), wait up to 3 s for the reply with the same header; returns reply length or -1
static ssize_t xfer(unsigned cmd, const uint8_t *body, size_t blen)
{
	uint8_t req[600];
	if (blen + 4 > sizeof(req))
		return -1;
	req[0] = 0x4b;
	req[1] = subsys;
	req[2] = cmd;
	req[3] = cmd >> 8;
	memcpy(req + 4, body, blen);
	if (send(fd, req, blen + 4, 0) != (ssize_t)(blen + 4)) {
		perror("duo-efs: send");
		return -1;
	}
	double t0 = mono();
	while (mono() - t0 < 3) {
		struct pollfd pf = { fd, POLLIN };
		if (poll(&pf, 1, 200) <= 0)
			continue;
		ssize_t n = recv(fd, rsp, sizeof(rsp), 0);
		if (n <= 0) {
			fprintf(stderr, "duo-efs: diag-router closed the socket\n");
			return -1;
		}
		if (n >= 4 && rsp[0] == 0x4b && rsp[1] == subsys && (rsp[2] | rsp[3] << 8) == cmd)
			return n;
		// 0x13 bad command, 0x14 bad params, 0x15 bad length: error code + echo of our request
		if (n >= 5 && (rsp[0] == 0x13 || rsp[0] == 0x14 || rsp[0] == 0x15) && rsp[1] == 0x4b &&
		    rsp[2] == subsys && (rsp[3] | rsp[4] << 8) == cmd) {
			fprintf(stderr, "duo-efs: cmd %u rejected with diag error 0x%02x\n", cmd, rsp[0]);
			return -1;
		}
	}
	fprintf(stderr, "duo-efs: cmd %u: no reply in 3 s\n", cmd);
	return -1;
}

static const char *ftype(uint32_t mode)
{
	switch (mode & 0170000) {
	case 0040000:
		return "dir";
	case 0100000:
		return "file";
	case 0120000:
		return "link";
	case 0160000:
		return "item";
	default:
		return "other";
	}
}

static int do_stat(const char *path)
{
	ssize_t n = xfer(EFS_STAT, (const uint8_t *)path, strlen(path) + 1);
	if (n < 4 + 28)
		return 1;
	int32_t e = rd32(rsp + 4);
	if (e) {
		printf("%s: %s\n", path, efserr(e));
		return 1;
	}
	uint32_t mode = rd32(rsp + 8);
	printf("%s: %s mode 0%o size %u nlink %u mtime %u\n", path, ftype(mode), mode, rd32(rsp + 12), rd32(rsp + 16),
	       rd32(rsp + 24));
	return 0;
}

static int do_ls(const char *dir)
{
	ssize_t n = xfer(EFS_OPENDIR, (const uint8_t *)dir, strlen(dir) + 1);
	if (n < 12)
		return 1;
	uint32_t dp = rd32(rsp + 4);
	int32_t e = rd32(rsp + 8);
	if (e) {
		printf("%s: opendir: %s\n", dir, efserr(e));
		return 1;
	}
	printf("%s:\n", dir);
	int ret = 0;
	for (uint32_t seq = 1; seq < 10000; seq++) {
		uint8_t b[8];
		wr32(b, dp);
		wr32(b + 4, seq);
		n = xfer(EFS_READDIR, b, 8);
		if (n < 4 + 36) {
			ret = 1;
			break;
		}
		e = rd32(rsp + 12);
		if (e) {
			printf("  readdir: %s\n", efserr(e));
			ret = 1;
			break;
		}
		const char *name = (const char *)rsp + 40;
		size_t nl = strnlen(name, n - 40);
		if (!nl)
			break;
		uint32_t mode = rd32(rsp + 20);
		printf("  %-5s 0%06o %8u  %.*s\n", ftype(mode), mode, rd32(rsp + 24), (int)nl, name);
	}
	uint8_t b[4];
	wr32(b, dp);
	xfer(EFS_CLOSEDIR, b, 4);
	return ret;
}

static void hexdump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i += 16) {
		printf("  %04zx:", i);
		for (size_t j = i; j < i + 16; j++)
			j < n ? printf(" %02x", p[j]) : printf("   ");
		printf("  ");
		for (size_t j = i; j < i + 16 && j < n; j++)
			putchar(p[j] >= 32 && p[j] < 127 ? p[j] : '.');
		putchar('\n');
	}
}

// item files (S_IFITM, the usual kind under /nv/item_files) can only be read with GET
static int do_get(const char *path)
{
	uint8_t b[520];
	size_t pl = strlen(path) + 1;
	if (pl > 500)
		return 1;
	wr32(b, 512);	   // data_length: largest item we accept
	wr32(b + 4, pl);   // path_length
	b[8] = 1, b[9] = 0; // sequence number
	memcpy(b + 10, path, pl);
	ssize_t n = xfer(EFS_GET, b, 10 + pl);
	if (n < 14)
		return 1;
	int32_t num = rd32(rsp + 4), e = rd32(rsp + 8);
	if (e) {
		printf("%s: get: %s\n", path, efserr(e));
		return 1;
	}
	printf("%s: get: %d bytes\n", path, num);
	if (num > 0 && 14 + (size_t)num <= (size_t)n)
		hexdump(rsp + 14, num);
	return 0;
}

static int do_cat(const char *path)
{
	uint8_t b[512];
	size_t pl = strlen(path) + 1;
	if (pl > 500)
		return 1;
	wr32(b, 0); // O_RDONLY
	wr32(b + 4, 0);
	memcpy(b + 8, path, pl);
	ssize_t n = xfer(EFS_OPEN, b, 8 + pl);
	if (n < 12)
		return 1;
	uint32_t f = rd32(rsp + 4);
	int32_t e = rd32(rsp + 8);
	if (e) {
		printf("%s: open: %s, trying GET\n", path, efserr(e));
		return do_get(path);
	}
	printf("%s:\n", path);
	uint8_t data[65536];
	size_t tot = 0;
	int ret = 0;
	while (tot < sizeof(data)) {
		wr32(b, f);
		wr32(b + 4, 1024);
		wr32(b + 8, tot);
		n = xfer(EFS_READ, b, 12);
		if (n < 20) {
			ret = 1;
			break;
		}
		int32_t got = rd32(rsp + 12);
		e = rd32(rsp + 16);
		if (e) {
			printf("  read: %s\n", efserr(e));
			ret = 1;
			break;
		}
		if (got <= 0)
			break;
		if (got > n - 20)
			got = n - 20;
		if (tot + got > sizeof(data))
			got = sizeof(data) - tot;
		memcpy(data + tot, rsp + 20, got);
		tot += got;
		if (got < 1024)
			break;
	}
	wr32(b, f);
	xfer(EFS_CLOSE, b, 4);
	hexdump(data, tot);
	printf("  (%zu bytes)\n", tot);
	return ret;
}

int main(int argc, char **argv)
{
	int a = 1;
	if (a + 1 < argc && !strcmp(argv[a], "-s")) {
		subsys = strtoul(argv[a + 1], NULL, 0);
		a += 2;
	}
	if (a + 1 >= argc || (strcmp(argv[a], "stat") && strcmp(argv[a], "ls") && strcmp(argv[a], "cat") &&
			      strcmp(argv[a], "get"))) {
		fprintf(stderr, "usage: duo-efs [-s subsys] stat|ls|cat|get PATH...  (read-only)\n");
		return 2;
	}
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, "\0diag", 5);
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		perror("duo-efs: connect to diag-router");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	int ret = 0;
	for (int i = a + 1; i < argc; i++) {
		if (!strcmp(argv[a], "stat"))
			ret |= do_stat(argv[i]);
		else if (!strcmp(argv[a], "ls"))
			ret |= do_ls(argv[i]);
		else if (!strcmp(argv[a], "get"))
			ret |= do_get(argv[i]);
		else
			ret |= do_cat(argv[i]);
	}
	return ret;
}
