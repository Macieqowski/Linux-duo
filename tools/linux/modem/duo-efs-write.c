// SPDX-License-Identifier: GPL-2.0-or-later
// duo-efs-write: create, overwrite or delete small files in the Duo modem's EFS over DIAG, through linux-msm
// diag-router (abstract unix socket "\0diag"), with the EFS2 diag subsystem commands (DIAG_SUBSYS_CMD 0x4b,
// subsystem 19 = FS): MKDIR 9, OPEN 2 + WRITE 5 + CLOSE 3, UNLINK 8, RMDIR 10 (layouts as in QCSuper's efs_shell).
// EFS is the modem's own persistent storage. Where a write lands depends on rmtfs: with duo-rmtfs (rmtfs on Linux's
// own EFS copy in /var/lib/duo-efs) it persists in that copy only; with `rmtfs -r -P` it lives in rmtfs's RAM until
// the phone restarts; only an rmtfs writing the modemst partitions would change the EFS that Android uses too.
// The modem flushes its EFS to rmtfs when it stops. Use duo-efs (read-only) to look before and after.
// usage: duo-efs-write mkdir DIR | put PATH HEXBYTES | rm PATH | rmdir DIR
//   e.g. duo-efs-write put /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg 01
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
	EFS_WRITE = 5,
	EFS_UNLINK = 8,
	EFS_MKDIR = 9,
	EFS_RMDIR = 10,
};

static int fd;
static uint8_t rsp[65536];

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int32_t rd32(const uint8_t *p)
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
	static char buf[64];
	if (e > 0x40000000 && e <= 0x4000000c) {
		snprintf(buf, sizeof(buf), "FS_DIAG error 0x%x", e);
		return buf;
	}
	return strerror(e);
}

// send cmd (header + body), wait up to 3 s for the reply with the same header; returns reply length or -1
static ssize_t xfer(unsigned cmd, const uint8_t *body, size_t blen)
{
	uint8_t req[600];
	if (blen + 4 > sizeof(req))
		return -1;
	req[0] = 0x4b;
	req[1] = 19;
	req[2] = cmd;
	req[3] = cmd >> 8;
	memcpy(req + 4, body, blen);
	if (send(fd, req, blen + 4, 0) != (ssize_t)(blen + 4)) {
		perror("duo-efs-write: send");
		return -1;
	}
	double t0 = mono();
	while (mono() - t0 < 3) {
		struct pollfd pf = { fd, POLLIN };
		if (poll(&pf, 1, 200) <= 0)
			continue;
		ssize_t n = recv(fd, rsp, sizeof(rsp), 0);
		if (n <= 0) {
			fprintf(stderr, "duo-efs-write: diag-router closed the socket\n");
			return -1;
		}
		if (n >= 4 && rsp[0] == 0x4b && rsp[1] == 19 && (rsp[2] | rsp[3] << 8) == cmd)
			return n;
		if (n >= 5 && (rsp[0] == 0x13 || rsp[0] == 0x14 || rsp[0] == 0x15) && rsp[1] == 0x4b && rsp[2] == 19 &&
		    (rsp[3] | rsp[4] << 8) == cmd) {
			fprintf(stderr, "duo-efs-write: cmd %u rejected with diag error 0x%02x\n", cmd, rsp[0]);
			return -1;
		}
	}
	fprintf(stderr, "duo-efs-write: cmd %u: no reply in 3 s\n", cmd);
	return -1;
}

// commands whose reply is just header + errno
static int simple(unsigned cmd, const uint8_t *body, size_t blen, const char *what, const char *path)
{
	ssize_t n = xfer(cmd, body, blen);
	if (n < 8)
		return 1;
	int32_t e = rd32(rsp + 4);
	printf("%s %s: %s\n", what, path, e ? efserr(e) : "ok");
	return e != 0;
}

static int do_mkdir(const char *path)
{
	uint8_t b[512];
	size_t pl = strlen(path) + 1;
	if (pl > 500)
		return 1;
	uint16_t mode = 040777; // S_IFDIR | 0777, sent as int16
	b[0] = mode, b[1] = mode >> 8;
	memcpy(b + 2, path, pl);
	return simple(EFS_MKDIR, b, 2 + pl, "mkdir", path);
}

static int do_path_cmd(unsigned cmd, const char *what, const char *path)
{
	size_t pl = strlen(path) + 1;
	if (pl > 500)
		return 1;
	return simple(cmd, (const uint8_t *)path, pl, what, path);
}

static int do_put(const char *path, const char *hex)
{
	uint8_t data[256];
	size_t dl = strlen(hex) / 2;
	if (strlen(hex) % 2 || dl == 0 || dl > sizeof(data)) {
		fprintf(stderr, "duo-efs-write: HEXBYTES must be 1..256 bytes in hex, e.g. 01\n");
		return 2;
	}
	for (size_t i = 0; i < dl; i++) {
		char h[3] = { hex[2 * i], hex[2 * i + 1], 0 }, *end;
		data[i] = strtoul(h, &end, 16);
		if (*end) {
			fprintf(stderr, "duo-efs-write: bad hex '%s'\n", h);
			return 2;
		}
	}
	uint8_t b[600];
	size_t pl = strlen(path) + 1;
	if (pl > 500)
		return 1;
	wr32(b, 01101);	    // O_WRONLY | O_CREAT | O_TRUNC
	wr32(b + 4, 0100644); // S_IFREG | 0644
	memcpy(b + 8, path, pl);
	ssize_t n = xfer(EFS_OPEN, b, 8 + pl);
	if (n < 12)
		return 1;
	int32_t f = rd32(rsp + 4), e = rd32(rsp + 8);
	if (e) {
		printf("put %s: open: %s\n", path, efserr(e));
		return 1;
	}
	wr32(b, f);
	wr32(b + 4, 0); // offset
	memcpy(b + 8, data, dl);
	n = xfer(EFS_WRITE, b, 8 + dl);
	int ret = 1;
	if (n >= 20) {
		int32_t got = rd32(rsp + 12);
		e = rd32(rsp + 16);
		if (e)
			printf("put %s: write: %s\n", path, efserr(e));
		else if ((size_t)got != dl)
			printf("put %s: short write %d of %zu\n", path, got, dl);
		else
			ret = 0;
	}
	wr32(b, f);
	n = xfer(EFS_CLOSE, b, 4);
	if (n < 8 || rd32(rsp + 4)) {
		printf("put %s: close failed\n", path);
		ret = 1;
	}
	if (!ret)
		printf("put %s: %zu bytes written\n", path, dl);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc < 3 || (strcmp(argv[1], "mkdir") && strcmp(argv[1], "put") && strcmp(argv[1], "rm") &&
			 strcmp(argv[1], "rmdir")) ||
	    (!strcmp(argv[1], "put") && argc != 4)) {
		fprintf(stderr, "usage: duo-efs-write mkdir DIR | put PATH HEXBYTES | rm PATH | rmdir DIR\n"
				"  writes the modem's persistent EFS (shared with Android)\n");
		return 2;
	}
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, "\0diag", 5);
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		perror("duo-efs-write: connect to diag-router");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!strcmp(argv[1], "mkdir"))
		return do_mkdir(argv[2]);
	if (!strcmp(argv[1], "rm"))
		return do_path_cmd(EFS_UNLINK, "rm", argv[2]);
	if (!strcmp(argv[1], "rmdir"))
		return do_path_cmd(EFS_RMDIR, "rmdir", argv[2]);
	return do_put(argv[2], argv[3]);
}
