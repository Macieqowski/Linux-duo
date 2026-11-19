// SPDX-License-Identifier: GPL-2.0-or-later
// duo-nas-pref: set the modem's RAT + band preference over QRTR (qmicli 1.36 can set only the RAT mode).
// Why: with a SIM, the default LTE/UMTS/GSM scan over every band crashes the Duo's modem (RF QLink assertion);
// LTE only works (2026-11-19). This narrows 2G/3G to chosen bands, to find which band/RAT trips it.
// usage: duo-nas-pref <rats> [gsm/wcdma bands] [lte bands]
//   rats:  comma list of gsm,umts,lte
//   gsm/wcdma bands: comma list of gsm900,egsm900,gsm1800,wcdma2100,wcdma900  ("-" = leave unchanged)
//   lte bands: comma list of LTE band numbers 1..64 ("-" or empty = leave unchanged)
// e.g. duo-nas-pref lte,gsm gsm900,egsm900,gsm1800 1,3,7,8,20
#include <libqmi-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static QmiDevice *dev;
static QmiNasRatModePreference rats;
static guint64 bands, lte;
static int rc = 1;

static void die(const char *what, GError *e)
{
	fprintf(stderr, "duo-nas-pref: %s: %s\n", what, e ? e->message : "?");
	exit(1);
}

static void set_done(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiMessageNasSetSystemSelectionPreferenceOutput *out =
		qmi_client_nas_set_system_selection_preference_finish(QMI_CLIENT_NAS(o), r, &e);
	if (!out || !qmi_message_nas_set_system_selection_preference_output_get_result(out, &e))
		die("set system selection preference", e);
	printf("ok: rats 0x%x bands 0x%llx lte 0x%llx\n", rats, (unsigned long long)bands, (unsigned long long)lte);
	rc = 0;
	g_main_loop_quit(loop);
}

static void got_client(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiClient *c = qmi_device_allocate_client_finish(dev, r, &e);
	if (!c)
		die("allocate NAS client", e);
	QmiMessageNasSetSystemSelectionPreferenceInput *in = qmi_message_nas_set_system_selection_preference_input_new();
	qmi_message_nas_set_system_selection_preference_input_set_mode_preference(in, rats, NULL);
	if (bands)
		qmi_message_nas_set_system_selection_preference_input_set_band_preference(in, bands, NULL);
	if (lte)
		qmi_message_nas_set_system_selection_preference_input_set_lte_band_preference(in, lte, NULL);
	qmi_message_nas_set_system_selection_preference_input_set_change_duration(in, QMI_NAS_CHANGE_DURATION_PERMANENT, NULL);
	qmi_client_nas_set_system_selection_preference(QMI_CLIENT_NAS(c), in, 10, NULL, set_done, NULL);
	qmi_message_nas_set_system_selection_preference_input_unref(in);
}

static void opened(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	if (!qmi_device_open_finish(dev, r, &e))
		die("open", e);
	qmi_device_allocate_client(dev, QMI_SERVICE_NAS, QMI_CID_NONE, 10, NULL, got_client, NULL);
}

static void got_dev(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	dev = qmi_device_new_from_node_finish(r, &e);
	if (!dev)
		die("device from node", e);
	qmi_device_open(dev, QMI_DEVICE_OPEN_FLAGS_NONE, 10, NULL, opened, NULL);
}

static void got_node(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QrtrNode *n = qrtr_bus_wait_for_node_finish(QRTR_BUS(o), r, &e);
	if (!n)
		die("qrtr node", e);
	qmi_device_new_from_node(n, NULL, got_dev, NULL);
}

static void got_bus(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QrtrBus *b = qrtr_bus_new_finish(r, &e);
	if (!b)
		die("qrtr bus", e);
	qrtr_bus_wait_for_node(b, 0, 5000, NULL, got_node, NULL);
}

static gboolean timeout(gpointer d)
{
	fprintf(stderr, "duo-nas-pref: timeout\n");
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: duo-nas-pref <gsm,umts,lte> [gsm900,egsm900,gsm1800,wcdma2100,wcdma900|-] [lte bands|-]\n");
		return 2;
	}
	gchar **v = g_strsplit(argv[1], ",", -1);
	for (gchar **p = v; *p; p++) {
		if (!strcmp(*p, "gsm")) rats |= QMI_NAS_RAT_MODE_PREFERENCE_GSM;
		else if (!strcmp(*p, "umts")) rats |= QMI_NAS_RAT_MODE_PREFERENCE_UMTS;
		else if (!strcmp(*p, "lte")) rats |= QMI_NAS_RAT_MODE_PREFERENCE_LTE;
		else { fprintf(stderr, "unknown rat %s\n", *p); return 2; }
	}
	g_strfreev(v);
	if (argc > 2 && strcmp(argv[2], "-")) {
		v = g_strsplit(argv[2], ",", -1);
		for (gchar **p = v; *p; p++) {
			if (!strcmp(*p, "gsm900")) bands |= QMI_NAS_BAND_PREFERENCE_GSM_900_PRIMARY;
			else if (!strcmp(*p, "egsm900")) bands |= QMI_NAS_BAND_PREFERENCE_GSM_900_EXTENDED;
			else if (!strcmp(*p, "gsm1800")) bands |= QMI_NAS_BAND_PREFERENCE_GSM_DCS_1800;
			else if (!strcmp(*p, "wcdma2100")) bands |= QMI_NAS_BAND_PREFERENCE_WCDMA_2100;
			else if (!strcmp(*p, "wcdma900")) bands |= QMI_NAS_BAND_PREFERENCE_WCDMA_900;
			else { fprintf(stderr, "unknown band %s\n", *p); return 2; }
		}
		g_strfreev(v);
	}
	if (argc > 3 && strcmp(argv[3], "-") && *argv[3]) {
		v = g_strsplit(argv[3], ",", -1);
		for (gchar **p = v; *p; p++) {
			int b = atoi(*p);
			if (b < 1 || b > 64) { fprintf(stderr, "bad lte band %s\n", *p); return 2; }
			lte |= (guint64)1 << (b - 1);
		}
		g_strfreev(v);
	}
	loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_seconds(30, timeout, NULL);
	qrtr_bus_new(1000, NULL, got_bus, NULL);
	g_main_loop_run(loop);
	return rc;
}
