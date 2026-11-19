// SPDX-License-Identifier: GPL-2.0-or-later
// duo-diag-quiet: turn the modem's DIAG logging off (all F3 message masks 0, event reports off, all log codes off)
// through diag-router, without stopping the router (a restarted router gets no new DIAG handshake from the modem).
// With duo-diag's full F3 mask the modem streams ~1000 messages/s, which may keep it from ever sleeping; this puts it
// back to the normal (quiet) state, e.g. to test the modem's sleep behaviour. duo-diag-cap sets the masks again.
// usage: duo-diag-quiet
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int sendpkt(int fd, const void *p, size_t n, const char *what)
{
	if (send(fd, p, n, 0) != (ssize_t)n) {
		perror(what);
		return 1;
	}
	printf("duo-diag-quiet: %s sent\n", what);
	return 0;
}

int main(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	memcpy(sa.sun_path, "\0diag", 5);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		perror("duo-diag-quiet: connect to diag-router");
		return 1;
	}
	const uint8_t msg[7] = { 0x7d, 0x05, 0, 0, 0, 0, 0 }; // EXT_MSG_CONFIG: set all SSID masks to 0
	const uint8_t evt[2] = { 0x60, 0x00 };                   // EVENT_REPORT: off
	const uint8_t log[8] = { 0x73, 0, 0, 0, 0, 0, 0, 0 };    // LOG_CONFIG op 0: disable all log codes
	int r = sendpkt(fd, msg, sizeof(msg), "F3 masks 0");
	r |= sendpkt(fd, evt, sizeof(evt), "events off");
	r |= sendpkt(fd, log, sizeof(log), "log codes off");
	usleep(500000); // let the router forward them before the socket closes
	close(fd);
	return r;
}
