/*
 * Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/memblock.h>

#include <linux/nvmap.h>
#include <linux/tegra-ivc.h>

#include "nvmap_priv.h"
#include "iomap.h"
#include "board.h"
#include <linux/platform/tegra/common.h>
#include "../../../drivers/virt/tegra/syscalls.h"

/* FIXME: Statically allocated from DT carveout */
phys_addr_t __weak tegra_carveout_start = 0xa8f00000;
phys_addr_t __weak tegra_carveout_size =
	config_enabled(CONFIG_NVMAP_CONVERT_CARVEOUT_TO_IOVMM) ? 0 : SZ_512M;

phys_addr_t __weak tegra_vpr_start;
phys_addr_t __weak tegra_vpr_size;
bool __weak tegra_vpr_resize;

struct device __weak tegra_generic_dev;
struct device __weak tegra_vpr_dev;
struct device __weak tegra_iram_dev;
struct device __weak tegra_generic_cma_dev;
struct device __weak tegra_vpr_cma_dev;
struct dma_resize_notifier_ops __weak vpr_dev_ops;

static const struct of_device_id nvmap_of_ids[] = {
	{ .compatible = "nvidia,carveouts" },
	{ }
};

/*
 * Order of this must match nvmap_carveouts;
 */
static char *nvmap_carveout_names[] = {
	"iram",
	"generic",
	"vpr"
};

static struct nvmap_platform_carveout nvmap_carveouts[] = {
	[0] = {
		.name		= "iram",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_IRAM,
		.base		= 0,
		.size		= 0,
		.dma_dev	= &tegra_iram_dev,
	},
	[1] = {
		.name		= "generic-0",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_GENERIC,
		.base		= 0,
		.size		= 0,
		.dma_dev	= &tegra_generic_dev,
	},
	[2] = {
		.name		= "vpr",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_VPR,
		.base		= 0,
		.size		= 0,
		.dma_dev	= &tegra_vpr_dev,
	},
};

static struct nvmap_platform_data nvmap_data = {
	.carveouts	= nvmap_carveouts,
	.nr_carveouts	= ARRAY_SIZE(nvmap_carveouts),
};

/*
 * In case there is no DT entry.
 */
static struct platform_device nvmap_platform_device  = {
	.name	= "tegra-carveouts",
	.id	= -1,
	.dev	= {
		.platform_data = &nvmap_data,
	},
};

/*
 * @data must be at least a 2 element array of u64's.
 */
static int nvmap_populate_carveout(const void *data,
				   struct nvmap_platform_carveout *co)
{
	__be64 *co_data = (__be64 *)data;
	u64 base, size;

	base = be64_to_cpup(co_data);
	size = be64_to_cpup(co_data + 1);

	/*
	 * If base and size are 0, then assume the CO is not being populated.
	 */
	if (base == 0 && size == 0)
		return -ENODEV;

	if (size == 0)
		return -EINVAL;

	co->base = (phys_addr_t)base;
	co->size = (size_t)size;

	pr_info("Populating %s\n", co->name);
	pr_info("  base = 0x%08lx size = 0x%x\n",
		(unsigned long)base, (unsigned int)size);

	return 0;
}

#ifdef CONFIG_TEGRA_VIRTUALIZATION
int nvmap_populate_ivm_carveout(struct device_node *n,
				struct nvmap_platform_carveout *co)
{
	struct device_node *hvn;
	u32 id;
	int ret;
	struct tegra_hv_ivm_cookie *ivm;
	unsigned int guestid;

	if (hyp_read_gid(&guestid)) {
		pr_err("failed to read gid\n");
		return -EINVAL;
	}

	hvn = of_parse_phandle(n, "ivm", 0);
	if (!hvn) {
		pr_err("failed to parse ivm\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_index(n, "ivm", 1, &id);
	if (ret) {
		pr_err("failed to read ivm id\n");
		return -EINVAL;
	}

	ivm = tegra_hv_mempool_reserve(hvn, id);
	if (!ivm) {
		pr_err("failed to reserve IV memory pool %d\n", id);
		return -ENOMEM;
	}

	/* XXX: Are these the available fields from IVM cookie? */
	co->base     = (phys_addr_t)ivm->ipa;
	co->peer     = ivm->peer_vmid;
	co->size     = ivm->size;
	co->vmid     = (int)guestid;

	if (!co->base || !co->size)
		return -EINVAL;

	/* See if this VM can allocate (or just create handle from ID)
	 * generated by peer partition */
	if (of_property_read_u32(n, "alloc", &co->can_alloc))
		return -EINVAL;

	pr_info("IVM carveout IPA:%p, size=%d, peer vmid=%d\n",
		(void *)(uintptr_t)co->base, co->size, co->peer);

	co->is_ivm    = true;
	co->name      = "ivc"; /* XXX: Add peer ID suffix */
	/* Relying on nvmap_heap_create() fallback to use heap->dev as dma_dev
	 */
	co->usage_mask = NVMAP_HEAP_CARVEOUT_IVM;

	of_node_put(hvn);
	return 0;
}
#else
int nvmap_populate_ivm_carveout(struct device_node *n,
				struct nvmap_platform_carveout *co)
{
	return -EINVAL;
}
#endif

static int __nvmap_init_legacy(void);
static int __nvmap_init_dt(struct platform_device *pdev)
{
	const void *prop;
	int prop_len, i;
	struct device_node *node, *n;

	if (!of_match_device(nvmap_of_ids, &pdev->dev)) {
		pr_err("Missing DT entry!\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(nvmap_carveout_names); i++) {
		prop = of_get_property(pdev->dev.of_node,
				       nvmap_carveout_names[i], &prop_len);
		if (!prop || prop_len != 4 * sizeof(u32)) {
			pr_err("Missing carveout for %s!\n",
			       nvmap_carveout_names[i]);
			continue;
		}
		nvmap_populate_carveout(prop, &nvmap_carveouts[i]);
	}

	/* For VM_2 we need carveout. So, enabling it here */
	__nvmap_init_legacy();
	/* Parse and setup inter VM carveouts */
	node = of_get_child_by_name(pdev->dev.of_node, "ivm_carveouts");
	if (node) {
		int nivm = of_get_child_count(node);

		if (nivm) {
			int ncarveouts = ARRAY_SIZE(nvmap_carveouts) + nivm;
			/* Where should this be freed? */
			struct nvmap_platform_carveout *carveouts =
				devm_kzalloc(&pdev->dev,
					ncarveouts * sizeof(*carveouts),
					GFP_KERNEL);
			int index;
			if (!carveouts)
				return -EINVAL;

			/* Copy over data from fixed carveouts */
			memcpy(carveouts, nvmap_carveouts,
					ARRAY_SIZE(nvmap_carveouts) *
						sizeof(*carveouts));

			index = ARRAY_SIZE(nvmap_carveouts);
			for_each_child_of_node(node, n) {
				if (nvmap_populate_ivm_carveout(n,
						&carveouts[index])) {
					kfree(carveouts);
					return -ENOMEM;
				}
				index++;
			}
			nvmap_data.carveouts = carveouts;
			nvmap_data.nr_carveouts = ncarveouts;
		}
	}

	pdev->dev.platform_data = &nvmap_data;

	return 0;
}

/*
 * This requires proper kernel arguments to have been passed.
 */
static int __nvmap_init_legacy(void)
{
	/* IRAM. */
	nvmap_carveouts[0].base = TEGRA_IRAM_BASE + TEGRA_RESET_HANDLER_SIZE;
	nvmap_carveouts[0].size = TEGRA_IRAM_SIZE - TEGRA_RESET_HANDLER_SIZE;

	/* Carveout. */
	if (config_enabled(CONFIG_NVMAP_CONVERT_CARVEOUT_TO_IOVMM) ||
	    tegra_vpr_resize || tegra_carveout_size) {
		/* Do nothing */
	} else {
		int err;
		phys_addr_t addr, size = SZ_128M;

		addr = memblock_find_in_range(size, 0, size, SZ_128K);
		if (!addr) {
			pr_err("%s() Failed to allocate %pa\n",
			       __func__, &size);
			return -ENOMEM;
		}

		err = memblock_reserve(addr, size);
		if (err) {
			pr_err("%s() Failed to reserve %pa\n",
			       __func__, &size);
			return -ENOMEM;
		}

		tegra_carveout_size = size;
		tegra_carveout_start = addr;

		pr_info("%s() allocate carveout=%pa@0%pa\n",
			__func__, &size, &addr);
	}

	nvmap_carveouts[1].base = tegra_carveout_start;
	nvmap_carveouts[1].size = tegra_carveout_size;

	/* VPR */
	nvmap_carveouts[2].base = tegra_vpr_start;
	nvmap_carveouts[2].size = tegra_vpr_size;

	if (tegra_vpr_resize) {
		nvmap_carveouts[1].cma_dev = &tegra_generic_cma_dev;
		nvmap_carveouts[2].cma_dev = &tegra_vpr_cma_dev;
	}

	return 0;
}

/*
 * Fills in the platform data either from the device tree or with the
 * legacy path.
 */
int nvmap_init(struct platform_device *pdev)
{
	int err;
	struct dma_declare_info vpr_dma_info;
	struct dma_declare_info generic_dma_info;

	if (pdev->dev.platform_data) {
		err = __nvmap_init_legacy();
		if (err)
			return err;
	}

	if (pdev->dev.of_node) {
		err = __nvmap_init_dt(pdev);
		if (err)
			return err;
	}

	if (!tegra_vpr_resize)
		goto end;
	generic_dma_info.name = "generic";
	generic_dma_info.size = 0;
	generic_dma_info.cma_dev = nvmap_carveouts[1].cma_dev;
	generic_dma_info.notifier.ops = NULL;

	vpr_dma_info.name = "vpr";
	vpr_dma_info.size = SZ_32M;
	vpr_dma_info.cma_dev = nvmap_carveouts[2].cma_dev;
	vpr_dma_info.notifier.ops = &vpr_dev_ops;

	if (nvmap_carveouts[1].size) {
		err = dma_declare_coherent_resizable_cma_memory(
				&tegra_generic_dev, &generic_dma_info);
		if (err) {
			pr_err("Generic coherent memory declaration failed\n");
			return err;
		}
	}
	if (nvmap_carveouts[2].size) {
		err = dma_declare_coherent_resizable_cma_memory(
				&tegra_vpr_dev, &vpr_dma_info);
		if (err) {
			pr_err("VPR coherent memory declaration failed\n");
			return err;
		}
	}

end:
	return err;
}

static int nvmap_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int nvmap_resume(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver nvmap_driver = {
	.probe		= nvmap_probe,
	.remove		= nvmap_remove,
	.suspend	= nvmap_suspend,
	.resume		= nvmap_resume,

	.driver = {
		.name	= "tegra-carveouts",
		.owner	= THIS_MODULE,
		.of_match_table = nvmap_of_ids,
	},
};

static int __init nvmap_init_driver(void)
{
	int e = 0;
	struct device_node *dnode;

	nvmap_dev = NULL;

	/* Pick DT vs legacy loading. */
	dnode = of_find_compatible_node(NULL, NULL, "nvidia,carveouts");
	if (!dnode)
		e = platform_device_register(&nvmap_platform_device);
	else
		of_node_put(dnode);

	if (e)
		goto fail;

	e = nvmap_heap_init();
	if (e)
		goto fail;

	e = platform_driver_register(&nvmap_driver);
	if (e) {
		nvmap_heap_deinit();
		goto fail;
	}

fail:
	return e;
}
fs_initcall(nvmap_init_driver);

static void __exit nvmap_exit_driver(void)
{
	platform_driver_unregister(&nvmap_driver);
	nvmap_heap_deinit();
	nvmap_dev = NULL;
}
module_exit(nvmap_exit_driver);
