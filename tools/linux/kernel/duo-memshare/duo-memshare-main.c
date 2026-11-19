// SPDX-License-Identifier: GPL-2.0
/*
 * duo-memshare: the QMI "memshare" service (0x34 v1 instance 1) for the Surface Duo modem, as a standalone module.
 *
 * Downstream SM8150 kernels (drivers/soc/qcom/memshare) lend the modem memory on request: clients GPS (id 0),
 * id 1 (5 MiB reserved at boot on sm8150, named "FTM" in the downstream ramdump table) and DIAG (id 2), all for
 * proc MPSS (0). Mainline has no such service. This module serves it the downstream way: CMA allocation, then a
 * hypervisor assignment of the block from HLOS to MSS_MSA (read/write), and the physical address back to the modem.
 *
 * QMI message tables: memshare_qmi_msg.[ch] from Nikita Travkin's mainline proposal (2021, "soc: qcom: Add Qualcomm
 * Memshare QMI service"). Request handling follows the downstream driver; no DT node needed (test module).
 */
#include <linux/dma-mapping.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/qmi.h>

#include "memshare_qmi_msg.h"

#define MEMSHARE_MAX_CLIENTS 8
#define MEMSHARE_PROC_MPSS 0

static bool assign = true;
module_param(assign, bool, 0444);
MODULE_PARM_DESC(assign, "hand the memory to the modem through the hypervisor (default on, as downstream)");

static uint boot_client_size = 0x500000;
module_param(boot_client_size, uint, 0444);
MODULE_PARM_DESC(boot_client_size, "size reported/allocated for client id 1 (downstream sm8150: 5 MiB)");

struct ms_client {
	u32 id, proc;
	u32 size;		/* allocated size, 0 = nothing allocated */
	u32 query_size;		/* what query_size answers (downstream qcom,peripheral-size) */
	dma_addr_t phys;
	void *cookie;
	bool assigned;
	bool in_use;
};

static struct {
	struct platform_device *pdev;
	struct qmi_handle qmi;
	struct mutex lock;
	struct ms_client clients[MEMSHARE_MAX_CLIENTS];
	int nclients;
} ms;

static struct ms_client *ms_client_get(u32 id, u32 proc)
{
	int i;

	for (i = 0; i < ms.nclients; i++)
		if (ms.clients[i].id == id && ms.clients[i].proc == proc)
			return &ms.clients[i];
	if (ms.nclients == MEMSHARE_MAX_CLIENTS)
		return NULL;
	ms.clients[ms.nclients] = (struct ms_client){ .id = id, .proc = proc };
	return &ms.clients[ms.nclients++];
}

static int ms_assign(struct ms_client *c, bool to_modem)
{
	struct qcom_scm_vmperm hlos = { QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_RWX };
	struct qcom_scm_vmperm mss = { QCOM_SCM_VMID_MSS_MSA, QCOM_SCM_PERM_RW };
	u64 src = to_modem ? BIT(QCOM_SCM_VMID_HLOS) : BIT(QCOM_SCM_VMID_MSS_MSA);
	int ret;

	if (!assign || c->assigned == to_modem)
		return 0;
	ret = qcom_scm_assign_mem(c->phys, c->size, &src, to_modem ? &mss : &hlos, 1);
	if (ret) {
		dev_err(&ms.pdev->dev, "hyp assign of %pad + %#x %s failed: %d\n", &c->phys, c->size,
			to_modem ? "to MSS" : "back to HLOS", ret);
		return ret;
	}
	c->assigned = to_modem;
	return 0;
}

static int ms_alloc(struct ms_client *c, u32 size)
{
	if (c->size) {
		if (size > c->size)
			dev_warn(&ms.pdev->dev, "client %u asks %#x, block is %#x\n", c->id, size, c->size);
		return 0;
	}
	size = PAGE_ALIGN(size);
	c->cookie = dma_alloc_attrs(&ms.pdev->dev, size, &c->phys, GFP_KERNEL, DMA_ATTR_NO_KERNEL_MAPPING);
	if (!c->cookie) {
		dev_err(&ms.pdev->dev, "client %u: cannot allocate %#x bytes\n", c->id, size);
		return -ENOMEM;
	}
	c->size = size;
	dev_info(&ms.pdev->dev, "client %u proc %u: %#x bytes at %pad\n", c->id, c->proc, size, &c->phys);
	return 0;
}

static void ms_alloc_generic(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn,
			     const void *data)
{
	const struct mem_alloc_generic_req_msg_v01 *req = data;
	struct mem_alloc_generic_resp_msg_v01 *resp;
	struct ms_client *c;
	int ret;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return;
	resp->resp.result = QMI_RESULT_FAILURE_V01;
	resp->resp.error = QMI_ERR_NO_MEMORY_V01;
	resp->sequence_id_valid = true;
	resp->sequence_id = req->sequence_id;

	dev_info(&ms.pdev->dev, "alloc_generic: node %u client %u proc %u bytes %#x seq %u contig %d/%u align %d/%u\n",
		 sq->sq_node, req->client_id, req->proc_id, req->num_bytes, req->sequence_id,
		 req->alloc_contiguous_valid, req->alloc_contiguous, req->block_alignment_valid,
		 req->block_alignment);

	mutex_lock(&ms.lock);
	c = ms_client_get(req->client_id, req->proc_id);
	if (c && !ms_alloc(c, req->num_bytes ? req->num_bytes : c->query_size) && !ms_assign(c, true)) {
		c->in_use = true;
		resp->resp.result = QMI_RESULT_SUCCESS_V01;
		resp->resp.error = QMI_ERR_NONE_V01;
		resp->dhms_mem_alloc_addr_info_valid = true;
		resp->dhms_mem_alloc_addr_info_len = 1;
		resp->dhms_mem_alloc_addr_info[0].phy_addr = c->phys;
		resp->dhms_mem_alloc_addr_info[0].num_bytes = c->size;
	}
	mutex_unlock(&ms.lock);

	ret = qmi_send_response(qmi, sq, txn, MEM_ALLOC_GENERIC_MSG_V01, MEM_MAX_MSG_LEN_V01,
				mem_alloc_generic_resp_msg_data_v01_ei, resp);
	if (ret < 0)
		dev_err(&ms.pdev->dev, "alloc_generic response failed: %d\n", ret);
	kfree(resp);
}

static void ms_free_generic(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn,
			    const void *data)
{
	const struct mem_free_generic_req_msg_v01 *req = data;
	struct mem_free_generic_resp_msg_v01 resp = { .resp.result = QMI_RESULT_SUCCESS_V01 };
	struct ms_client *c = NULL;
	int ret;

	dev_info(&ms.pdev->dev, "free_generic: node %u client %d/%u proc %d/%u len %u\n", sq->sq_node,
		 req->client_id_valid, req->client_id, req->proc_id_valid, req->proc_id,
		 req->dhms_mem_alloc_addr_info_len);
	mutex_lock(&ms.lock);
	if (req->client_id_valid && req->proc_id_valid)
		c = ms_client_get(req->client_id, req->proc_id);
	/* keep the block (and its assignment) for the next request, as downstream does for boot-time clients */
	if (c)
		c->in_use = false;
	mutex_unlock(&ms.lock);

	ret = qmi_send_response(qmi, sq, txn, MEM_FREE_GENERIC_MSG_V01, MEM_MAX_MSG_LEN_V01,
				mem_free_generic_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(&ms.pdev->dev, "free_generic response failed: %d\n", ret);
}

static void ms_query_size(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn,
			  const void *data)
{
	const struct mem_query_size_req_msg_v01 *req = data;
	struct mem_query_size_rsp_msg_v01 resp = { .resp.result = QMI_RESULT_SUCCESS_V01 };
	struct ms_client *c;
	int ret;

	mutex_lock(&ms.lock);
	c = ms_client_get(req->client_id, req->proc_id_valid ? req->proc_id : MEMSHARE_PROC_MPSS);
	resp.size_valid = true;
	resp.size = c ? c->query_size : 0;
	mutex_unlock(&ms.lock);
	dev_info(&ms.pdev->dev, "query_size: node %u client %u proc %d/%u -> %#x\n", sq->sq_node, req->client_id,
		 req->proc_id_valid, req->proc_id, resp.size);

	ret = qmi_send_response(qmi, sq, txn, MEM_QUERY_SIZE_MSG_V01, MEM_MAX_MSG_LEN_V01,
				mem_query_size_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(&ms.pdev->dev, "query_size response failed: %d\n", ret);
}

/* legacy GPS interface: one block, no client id */
static void ms_alloc_legacy(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn,
			    const void *data)
{
	const struct mem_alloc_req_msg_v01 *req = data;
	struct mem_alloc_resp_msg_v01 resp = { .resp = QMI_RESULT_FAILURE_V01 };
	struct ms_client *c;
	int ret;

	dev_info(&ms.pdev->dev, "alloc (legacy): node %u bytes %#x\n", sq->sq_node, req->num_bytes);
	mutex_lock(&ms.lock);
	c = ms_client_get(0, MEMSHARE_PROC_MPSS);
	if (c && !ms_alloc(c, req->num_bytes) && !ms_assign(c, true)) {
		c->in_use = true;
		resp.resp = QMI_RESULT_SUCCESS_V01;
		resp.handle_valid = true;
		resp.handle = c->phys;
		resp.num_bytes_valid = true;
		resp.num_bytes = c->size;
	}
	mutex_unlock(&ms.lock);
	ret = qmi_send_response(qmi, sq, txn, MEM_ALLOC_MSG_V01, MEM_MAX_MSG_LEN_V01,
				mem_alloc_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(&ms.pdev->dev, "alloc response failed: %d\n", ret);
}

static void ms_free_legacy(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn,
			   const void *data)
{
	struct mem_free_resp_msg_v01 resp = { .resp = QMI_RESULT_SUCCESS_V01 };
	int ret;

	dev_info(&ms.pdev->dev, "free (legacy): node %u\n", sq->sq_node);
	ret = qmi_send_response(qmi, sq, txn, MEM_FREE_MSG_V01, MEM_MAX_MSG_LEN_V01,
				mem_free_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(&ms.pdev->dev, "free response failed: %d\n", ret);
}

static const struct qmi_msg_handler ms_handlers[] = {
	{ .type = QMI_REQUEST, .msg_id = MEM_ALLOC_MSG_V01, .ei = mem_alloc_req_msg_data_v01_ei,
	  .decoded_size = sizeof(struct mem_alloc_req_msg_v01), .fn = ms_alloc_legacy },
	{ .type = QMI_REQUEST, .msg_id = MEM_FREE_MSG_V01, .ei = mem_free_req_msg_data_v01_ei,
	  .decoded_size = sizeof(struct mem_free_req_msg_v01), .fn = ms_free_legacy },
	{ .type = QMI_REQUEST, .msg_id = MEM_ALLOC_GENERIC_MSG_V01, .ei = mem_alloc_generic_req_msg_data_v01_ei,
	  .decoded_size = sizeof(struct mem_alloc_generic_req_msg_v01), .fn = ms_alloc_generic },
	{ .type = QMI_REQUEST, .msg_id = MEM_FREE_GENERIC_MSG_V01, .ei = mem_free_generic_req_msg_data_v01_ei,
	  .decoded_size = sizeof(struct mem_free_generic_req_msg_v01), .fn = ms_free_generic },
	{ .type = QMI_REQUEST, .msg_id = MEM_QUERY_SIZE_MSG_V01, .ei = mem_query_size_req_msg_data_v01_ei,
	  .decoded_size = sizeof(struct mem_query_size_req_msg_v01), .fn = ms_query_size },
	{}
};

static int __init ms_init(void)
{
	struct ms_client *c;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;
	mutex_init(&ms.lock);
	ms.pdev = platform_device_register_simple("duo-memshare", -1, NULL, 0);
	if (IS_ERR(ms.pdev))
		return PTR_ERR(ms.pdev);
	ret = dma_coerce_mask_and_coherent(&ms.pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_pdev;

	/* downstream sm8150: client 0 (GPS) and 2 on request, client 1 = 5 MiB allocated at boot */
	ms_client_get(0, MEMSHARE_PROC_MPSS);
	c = ms_client_get(1, MEMSHARE_PROC_MPSS);
	c->query_size = boot_client_size;
	if (boot_client_size)
		ms_alloc(c, boot_client_size);
	ms_client_get(2, MEMSHARE_PROC_MPSS);

	ret = qmi_handle_init(&ms.qmi, MEM_MAX_MSG_LEN_V01, NULL, ms_handlers);
	if (ret < 0)
		goto err_free;
	ret = qmi_add_server(&ms.qmi, MEM_SERVICE_SVC_ID, MEM_SERVICE_VER, MEM_SERVICE_INS_ID);
	if (ret < 0)
		goto err_qmi;
	dev_info(&ms.pdev->dev, "memshare service up (assign %d)\n", assign);
	return 0;

err_qmi:
	qmi_handle_release(&ms.qmi);
err_free:
	for (c = ms.clients; c < ms.clients + ms.nclients; c++)
		if (c->size)
			dma_free_attrs(&ms.pdev->dev, c->size, c->cookie, c->phys, DMA_ATTR_NO_KERNEL_MAPPING);
err_pdev:
	platform_device_unregister(ms.pdev);
	return ret;
}

static void __exit ms_exit(void)
{
	struct ms_client *c;

	qmi_handle_release(&ms.qmi);
	for (c = ms.clients; c < ms.clients + ms.nclients; c++) {
		if (!c->size)
			continue;
		/* memory still lent to a running modem must not come back to Linux */
		if (c->assigned && ms_assign(c, false)) {
			dev_warn(&ms.pdev->dev, "client %u: block stays with the modem (leaked)\n", c->id);
			continue;
		}
		dma_free_attrs(&ms.pdev->dev, c->size, c->cookie, c->phys, DMA_ATTR_NO_KERNEL_MAPPING);
	}
	platform_device_unregister(ms.pdev);
}

module_init(ms_init);
module_exit(ms_exit);
MODULE_DESCRIPTION("QMI memshare service for the Surface Duo modem (test module)");
MODULE_LICENSE("GPL");
