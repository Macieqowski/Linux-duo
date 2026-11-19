// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * duo-hinge: print the Surface Duo's hinge angle (degrees, one line per SSC report) from the sensor DSP.
 * The hinge is a ready Microsoft SSC sensor ("hinge_angle", reports "<angle> 0 <second estimate>"), but libssc has no
 * class for it: open it through the generic SSCSensor and decode the raw report (sns_std_sensor_event: field 1 =
 * packed floats). 0 = closed, 180 = flat, ~360 = screens back to back.
 * usage: duo-hinge [data-type]   (default hinge_angle; e.g. posture, hall)
 * build: cc -O2 duo-hinge.c $(pkg-config --cflags --libs libssc gio-2.0) -o duo-hinge
 */
#include <stdio.h>
#include <string.h>
#include <libssc/libssc.h>

#define SSC_MSG_REPORT_MEASUREMENT 1025

static GMainLoop *loop;
static guint64 uid_hi, uid_lo;

/* field 1 of the report: packed (wire type 2) or single (wire type 5) floats */
static void decode(const guint8 *p, gsize n)
{
	gsize i = 0;
	int printed = 0;

	while (i < n) {
		guint64 key = 0;
		int shift = 0;
		while (i < n) { guint8 b = p[i++]; key |= (guint64)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
		guint field = key >> 3, wt = key & 7;
		if (wt == 0) { while (i < n && (p[i++] & 0x80)); continue; }
		if (wt == 1) { i += 8; continue; }
		if (wt == 5) {
			if (field == 1 && i + 4 <= n) { float f; memcpy(&f, p + i, 4); printf(printed++ ? " %.2f" : "%.2f", f); }
			i += 4; continue;
		}
		if (wt == 2) {
			guint64 len = 0; shift = 0;
			while (i < n) { guint8 b = p[i++]; len |= (guint64)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
			if (field == 1)
				for (gsize j = 0; j + 4 <= len && i + j + 4 <= n; j += 4) {
					float f; memcpy(&f, p + i + j, 4); printf(printed++ ? " %.2f" : "%.2f", f);
				}
			i += len; continue;
		}
		break;
	}
	if (printed) { putchar('\n'); fflush(stdout); }
}

static void report(GObject *client, guint32 msg_id, guint64 hi, guint64 lo, GArray *buf, gpointer data)
{
	if (msg_id == SSC_MSG_REPORT_MEASUREMENT && hi == uid_hi && lo == uid_lo)
		decode((const guint8 *)buf->data, buf->len);
}

static void opened(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	if (!ssc_sensor_open_finish(SSC_SENSOR(src), res, &err)) {
		fprintf(stderr, "duo-hinge: open failed: %s\n", err->message);
		g_main_loop_quit(loop);
	}
}

static void created(GObject *src, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) err = NULL;
	SSCSensor *s = ssc_sensor_new_finish(res, &err);
	GObject *client = NULL;
	gchar *name = NULL;

	if (!s) {
		fprintf(stderr, "duo-hinge: no sensor: %s\n", err ? err->message : "?");
		g_main_loop_quit(loop);
		return;
	}
	g_object_get(s, "uid-high", &uid_hi, "uid-low", &uid_lo, "client", &client, "name", &name, NULL);
	fprintf(stderr, "duo-hinge: sensor \"%s\"\n", name ? name : "?");
	g_free(name);
	g_signal_connect(client, "report", G_CALLBACK(report), NULL);
	ssc_sensor_open(s, NULL, opened, NULL);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	loop = g_main_loop_new(NULL, FALSE);
	ssc_sensor_new(argc > 1 ? argv[1] : "hinge_angle", NULL, created, NULL);
	g_main_loop_run(loop);
	return 1;
}
