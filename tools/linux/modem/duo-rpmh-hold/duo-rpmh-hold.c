// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * duo-rpmh-hold: TEST module. Holds RPMh votes from the application processor (APPS DRV) for named cmd-db
 * resources, in the active, wake AND sleep sets, so the resource stays at that value while the AP sleeps too.
 * Purpose: find whether the Surface Duo modem's "quick offline" wake (QLink retrain after an RPMh-assisted power
 * collapse of the modem's offline shell) needs a resource that Android's AP keeps voted, e.g. the RF clock buffers
 * (rfclka*), the baseband clock buffers (lnbclka*), XO, or an ARC level (cx/mx/mss/qphy).
 *
 *   insmod duo-rpmh-hold.ko votes=rfclka1:4:1,lnbclka2:4:1,xo.lvl:0:3
 *
 * Each entry is NAME:OFFSET:VALUE: NAME is the cmd-db resource id (list them in /sys/kernel/debug/cmd-db), OFFSET
 * is added to its address (VRM-type clock buffers: 4 = enable; ARC levels: 0), VALUE is written.
 * All APPS clients share one vote per resource (last write wins): the votes are NOT released at rmmod, because a 0
 * would also cancel what clk-rpmh holds for Linux drivers (rf_clk2 for Wi-Fi, ln_bb_clk2 for the codec). They last
 * until the next phone restart (or until clk-rpmh rewrites that resource). Only vote ON values that are at least what
 * Linux already votes; never regulator voltages (a wrong VRM vote can damage hardware) and never cx/mx (rpmhpd).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <soc/qcom/cmd-db.h>
#include <soc/qcom/rpmh.h>
#include <soc/qcom/tcs.h>

#define MAX_VOTES 16

static char *votes;
module_param(votes, charp, 0444);
MODULE_PARM_DESC(votes, "NAME:OFFSET:VALUE[,...] cmd-db resources to hold");

static struct platform_device *pdev;
static struct tcs_cmd cmds[MAX_VOTES];
static char names[MAX_VOTES][32];
static int ncmds;

static int send_all(void)
{
	static const enum rpmh_state states[] = { RPMH_SLEEP_STATE, RPMH_WAKE_ONLY_STATE, RPMH_ACTIVE_ONLY_STATE };
	int i, s, ret, err = 0;

	for (i = 0; i < ncmds; i++) {
		struct tcs_cmd c = cmds[i];

		for (s = 0; s < ARRAY_SIZE(states); s++) {
			ret = rpmh_write(&pdev->dev, states[s], &c, 1);
			if (ret) {
				pr_err("duo-rpmh-hold: %s (0x%05x) state %d: %d\n", names[i], c.addr, states[s], ret);
				err = ret;
			}
		}
		pr_info("duo-rpmh-hold: %s 0x%05x = %u (held until restart)\n", names[i], c.addr, c.data);
	}
	return err;
}

static int parse(void)
{
	char *buf, *p, *e;
	int ret = 0;

	if (!votes || !*votes)
		return -EINVAL;
	buf = kstrdup(votes, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	p = buf;
	while ((e = strsep(&p, ",")) && ncmds < MAX_VOTES) {
		char *name = strsep(&e, ":"), *off = strsep(&e, ":"), *val = e;
		u32 addr, o, v;

		if (!name || !off || !val || kstrtou32(off, 0, &o) || kstrtou32(val, 0, &v) || o > 0xc) {
			pr_err("duo-rpmh-hold: bad entry (want NAME:OFFSET:VALUE)\n");
			ret = -EINVAL;
			break;
		}
		addr = cmd_db_read_addr(name);
		if (!addr) {
			pr_err("duo-rpmh-hold: %s not in cmd-db\n", name);
			ret = -ENOENT;
			break;
		}
		strscpy(names[ncmds], name, sizeof(names[0]));
		cmds[ncmds].addr = addr + o;
		cmds[ncmds].data = v;
		ncmds++;
	}
	kfree(buf);
	return ret;
}

static int __init duo_rpmh_hold_init(void)
{
	struct platform_device *rsc;
	struct device_node *np;
	int ret;

	ret = cmd_db_ready();
	if (ret)
		return ret;
	ret = parse();
	if (ret)
		return ret;

	/* rpmh_write() takes its controller from dev->parent: register a child of the APPS RSC */
	np = of_find_compatible_node(NULL, NULL, "qcom,rpmh-rsc");
	if (!np)
		return -ENODEV;
	rsc = of_find_device_by_node(np);
	of_node_put(np);
	if (!rsc || !platform_get_drvdata(rsc))
		return -EPROBE_DEFER;

	pdev = platform_device_register_data(&rsc->dev, "duo-rpmh-hold", PLATFORM_DEVID_NONE, NULL, 0);
	put_device(&rsc->dev);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	ret = send_all();
	if (ret)
		platform_device_unregister(pdev);
	return ret;
}

static void __exit duo_rpmh_hold_exit(void)
{
	pr_info("duo-rpmh-hold: unloaded, the votes stay until the next restart\n");
	platform_device_unregister(pdev);
}

module_init(duo_rpmh_hold_init);
module_exit(duo_rpmh_hold_exit);
MODULE_DESCRIPTION("Surface Duo test: hold APPS RPMh votes for cmd-db resources");
MODULE_LICENSE("GPL");
