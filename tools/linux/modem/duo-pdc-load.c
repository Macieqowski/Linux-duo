// SPDX-License-Identifier: GPL-2.0-or-later
// duo-pdc-load: load the board's hardware (platform) MBN into the Surface Duo modem over QRTR and select it.
// qmicli 1.36 --pdc-load-config segfaults over qrtr, hence this tool (same flow: SHA-1 id, 1 KiB chunks driven by
// the load-config indication, then set-selected).
// The modem keeps the result only for this boot (rmtfs runs read-only), so run it on every boot.
// Activation needs a modem restart ("Intentionally crash with err fatal for MCFG activation"); with remoteproc
// recovery off that leaves the modem dead, so --activate is separate and only for a caller that handles recovery.
// usage: duo-pdc-load [--activate] <mcfg_hw.mbn> [qrtr-node=0]
#include <libqmi-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 1024
static GMainLoop *loop;
static QmiDevice *dev;
static QmiClientPdc *pdc;
static int node_id = 0, activate, rc = 1;
static guchar *data;
static gsize size, sent;
static GArray *cfg_id;

static void die(const char *what, GError *e)
{
	fprintf(stderr, "duo-pdc-load: %s: %s\n", what, e ? e->message : "?");
	exit(1);
}

static void send_chunk(void);

static void done(int ok)
{
	rc = ok ? 0 : 1;
	g_main_loop_quit(loop);
}

static void activated(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiMessagePdcActivateConfigOutput *out = qmi_client_pdc_activate_config_finish(pdc, r, &e);
	if (!out || !qmi_message_pdc_activate_config_output_get_result(out, &e))
		die("activate", e);
	printf("activate requested (the modem restarts now)\n");
	done(1);
}

static void on_selected_ind(QmiClientPdc *c, QmiIndicationPdcSetSelectedConfigOutput *o, gpointer d)
{
	guint16 res = 0xffff;
	qmi_indication_pdc_set_selected_config_output_get_indication_result(o, &res, NULL);
	printf("selected: indication result %u\n", res);
	if (res != 0) {
		done(0);
		return;
	}
	if (!activate) {
		done(1);
		return;
	}
	QmiMessagePdcActivateConfigInput *in = qmi_message_pdc_activate_config_input_new();
	qmi_message_pdc_activate_config_input_set_config_type(in, QMI_PDC_CONFIGURATION_TYPE_PLATFORM, NULL);
	qmi_message_pdc_activate_config_input_set_token(in, 3, NULL);
	qmi_client_pdc_activate_config(pdc, in, 10, NULL, activated, NULL);
	qmi_message_pdc_activate_config_input_unref(in);
}

static void select_sent(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiMessagePdcSetSelectedConfigOutput *out = qmi_client_pdc_set_selected_config_finish(pdc, r, &e);
	if (!out || !qmi_message_pdc_set_selected_config_output_get_result(out, &e))
		die("set selected", e);
}

static void select_cfg(void)
{
	QmiMessagePdcSetSelectedConfigInput *in = qmi_message_pdc_set_selected_config_input_new();
	qmi_message_pdc_set_selected_config_input_set_type_with_id_v2(in, QMI_PDC_CONFIGURATION_TYPE_PLATFORM, cfg_id, NULL);
	qmi_message_pdc_set_selected_config_input_set_token(in, 2, NULL);
	qmi_client_pdc_set_selected_config(pdc, in, 10, NULL, select_sent, NULL);
	qmi_message_pdc_set_selected_config_input_unref(in);
}

static void on_load_ind(QmiClientPdc *c, QmiIndicationPdcLoadConfigOutput *o, gpointer d)
{
	guint16 res = 0xffff;
	guint32 remaining = 0;
	gboolean frame_reset = FALSE;
	qmi_indication_pdc_load_config_output_get_indication_result(o, &res, NULL);
	if (res != 0) {
		// 0x0b-ish "already exists" style errors: the config is in the modem already, go on and select it
		fprintf(stderr, "load: indication result %u\n", res);
		if (sent >= size) {
			select_cfg();
			return;
		}
		done(0);
		return;
	}
	if (qmi_indication_pdc_load_config_output_get_frame_reset(o, &frame_reset, NULL) && frame_reset) {
		fprintf(stderr, "load: frame reset by the modem\n");
		done(0);
		return;
	}
	qmi_indication_pdc_load_config_output_get_remaining_size(o, &remaining, NULL);
	if (sent < size) {
		send_chunk();
		return;
	}
	printf("loaded %zu bytes (remaining %u)\n", size, remaining);
	select_cfg();
}

static void chunk_sent(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiMessagePdcLoadConfigOutput *out = qmi_client_pdc_load_config_finish(pdc, r, &e);
	if (!out || !qmi_message_pdc_load_config_output_get_result(out, &e))
		die("load chunk", e);
}

static void send_chunk(void)
{
	gsize n = MIN((gsize)CHUNK, size - sent);
	GArray *chunk = g_array_sized_new(FALSE, FALSE, 1, n);
	g_array_append_vals(chunk, data + sent, n);
	sent += n;
	QmiMessagePdcLoadConfigInput *in = qmi_message_pdc_load_config_input_new();
	qmi_message_pdc_load_config_input_set_config_chunk(in, QMI_PDC_CONFIGURATION_TYPE_PLATFORM, cfg_id, size, chunk, NULL);
	qmi_message_pdc_load_config_input_set_token(in, 1, NULL);
	qmi_client_pdc_load_config(pdc, in, 10, NULL, chunk_sent, NULL);
	qmi_message_pdc_load_config_input_unref(in);
	g_array_unref(chunk);
}

static void got_client(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	QmiClient *c = qmi_device_allocate_client_finish(dev, r, &e);
	if (!c)
		die("allocate PDC client", e);
	pdc = QMI_CLIENT_PDC(c);
	g_signal_connect(pdc, "load-config", G_CALLBACK(on_load_ind), NULL);
	g_signal_connect(pdc, "set-selected-config", G_CALLBACK(on_selected_ind), NULL);
	send_chunk();
}

static void opened(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	if (!qmi_device_open_finish(dev, r, &e))
		die("open", e);
	qmi_device_allocate_client(dev, QMI_SERVICE_PDC, QMI_CID_NONE, 10, NULL, got_client, NULL);
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
	qrtr_bus_wait_for_node(b, node_id, 5000, NULL, got_node, NULL);
}

static gboolean timeout(gpointer d)
{
	fprintf(stderr, "duo-pdc-load: timeout\n");
	done(0);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	int i = 1;
	GError *e = NULL;
	if (i < argc && !strcmp(argv[i], "--activate")) {
		activate = 1;
		i++;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: duo-pdc-load [--activate] <mcfg_hw.mbn> [qrtr-node]\n");
		return 2;
	}
	if (!g_file_get_contents(argv[i], (gchar **)&data, &size, &e))
		die("read mbn", e);
	if (i + 1 < argc)
		node_id = atoi(argv[i + 1]);
	// config id = SHA-1 of the file (what qmicli and the vendor PDC daemons use)
	GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA1);
	g_checksum_update(cs, data, size);
	guint8 dig[20];
	gsize dl = sizeof(dig);
	g_checksum_get_digest(cs, dig, &dl);
	g_checksum_free(cs);
	cfg_id = g_array_sized_new(FALSE, FALSE, 1, dl);
	g_array_append_vals(cfg_id, dig, dl);
	setvbuf(stdout, NULL, _IOLBF, 0);
	loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_seconds(60, timeout, NULL);
	qrtr_bus_new(1000, NULL, got_bus, NULL);
	g_main_loop_run(loop);
	return rc;
}
