/*
 * fake_panel.c: fake panel driver.
 *
 * Copyright (c) 2014-2017, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iomap.h>
#include "fake_panel.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
#if defined(CONFIG_ARCH_TEGRA_210_SOC)
#define INT_GIC_BASE                    0
#define INT_PRI_BASE                    (INT_GIC_BASE + 32)
#define INT_SEC_BASE                    (INT_PRI_BASE + 32)
#define INT_TRI_BASE                    (INT_SEC_BASE + 32)
#define INT_QUAD_BASE                   (INT_TRI_BASE + 32)
#define INT_QUINT_BASE                  (INT_QUAD_BASE + 32)
#define INT_DISPLAY_GENERAL            (INT_TRI_BASE + 9)
#define INT_DPAUX                       (INT_QUINT_BASE + 31)
#endif
#else
#include <mach/irqs.h>/*for INT_DISPLAY_GENERAL, INT_DPAUX*/
#endif

#define DSI_PANEL_RESET		1

#define DC_CTRL_MODE	TEGRA_DC_OUT_CONTINUOUS_MODE

static struct tegra_dsi_out dsi_fake_panel_pdata = {
	.controller_vs = DSI_VS_1,
	.n_data_lanes = 4,

	.video_data_type = TEGRA_DSI_VIDEO_TYPE_VIDEO_MODE,
	.video_burst_mode = TEGRA_DSI_VIDEO_NONE_BURST_MODE,
	.refresh_rate = 60,

	.pixel_format = TEGRA_DSI_PIXEL_FORMAT_24BIT_P,
	.virtual_channel = TEGRA_DSI_VIRTUAL_CHANNEL_0,

	.panel_reset = DSI_PANEL_RESET,
	.power_saving_suspend = true,
	.video_clock_mode = TEGRA_DSI_VIDEO_CLOCK_TX_ONLY,

	.ulpm_not_supported = true,

};

static struct tegra_dc_mode dsi_fake_panel_modes[] = {
	{
#ifdef CONFIG_TEGRA_NVDISPLAY
		.pclk = 193224000, /* @60Hz*/
		.h_ref_to_sync = 1,
		.v_ref_to_sync = 11,
		.h_sync_width = 1,
		.v_sync_width = 1,
		.h_back_porch = 20,
		.v_back_porch = 7,
		.h_active = 1200,
		.v_active = 1920,
		.h_front_porch = 107,
		.v_front_porch = 497,
#else
		.pclk = 154700000, /* @60Hz*/
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 1,
		.h_sync_width = 16,
		.v_sync_width = 2,
		.h_back_porch = 32,
		.v_back_porch = 16,
		.h_active = 1920,
		.v_active = 1200,
		.h_front_porch = 120,
		.v_front_porch = 17,
#endif
	},
};

int tegra_dc_init_fake_panel_link_cfg(struct tegra_dc_dp_link_config *cfg)
{
	/*
	 * Currently fake the values for testing - same as eDp
	 * will need to add a method to update as needed
	 */
	if (!cfg->is_valid) {
		cfg->max_lane_count = 4;
		cfg->tps3_supported = false;
		cfg->support_enhanced_framing = true;
		cfg->downspread = true;
		cfg->support_fast_lt = true;
		cfg->aux_rd_interval = 0;
		cfg->alt_scramber_reset_cap = true;
		cfg->only_enhanced_framing = true;
		cfg->edp_cap = true;
		cfg->max_link_bw = 20;
		cfg->scramble_ena = 0;
		cfg->lt_data_valid = 0;
	}
	return 0;
}

static int tegra_dc_reset_fakedsi_panel(struct tegra_dc *dc, long dc_outtype)
{
	struct tegra_dc_out *dc_out = dc->out;
	if (dc_outtype == TEGRA_DC_OUT_FAKE_DSI_GANGED) {
		dc_out->dsi->ganged_type = TEGRA_DSI_GANGED_SYMMETRIC_EVEN_ODD;
		dc_out->dsi->even_odd_split_width = 1;
		dc_out->dsi->dsi_instance = DSI_INSTANCE_0;
		dc_out->dsi->n_data_lanes = 8;
	} else if (dc_outtype == TEGRA_DC_OUT_FAKE_DSIB) {
		dc_out->dsi->ganged_type = 0;
		dc_out->dsi->dsi_instance = DSI_INSTANCE_1;
		dc_out->dsi->n_data_lanes = 4;
	} else if (dc_outtype == TEGRA_DC_OUT_FAKE_DSIA) {
		dc_out->dsi->ganged_type = 0;
		dc_out->dsi->dsi_instance = DSI_INSTANCE_0;
		dc_out->dsi->n_data_lanes = 4;
	}

	return 0;
}

int tegra_dc_init_fakedsi_panel(struct tegra_dc *dc, long dc_outtype)
{
	struct tegra_dc_out *dc_out = dc->out;
	/* Set the needed resources */

	dc_out->dsi = &dsi_fake_panel_pdata;
#if defined(CONFIG_TEGRA_NVDISPLAY)
	dc_out->parent_clk = "pll_d_out1";
#else
	dc_out->parent_clk = "pll_d_out0";
#endif
	dc_out->modes = dsi_fake_panel_modes;
	dc_out->n_modes = ARRAY_SIZE(dsi_fake_panel_modes);
	dc_out->enable = NULL;
	dc_out->postpoweron = NULL;
	dc_out->disable = NULL;
	dc_out->postsuspend	= NULL;
	dc_out->width = 217;
	dc_out->height = 135;
	dc_out->flags = DC_CTRL_MODE;
	tegra_dc_reset_fakedsi_panel(dc, dc_outtype);

	if (!dc->out->sd_settings)
		return -EINVAL;
#ifndef CONFIG_TEGRA_NVDISPLAY
	dc->out->sd_settings->enable = 1;
#endif
	dc->out->sd_settings->enable_int = 1;

	return 0;
}


int tegra_dc_destroy_dsi_resources(struct tegra_dc *dc, long dc_outtype)
{
	int i = 0;
	struct tegra_dc_dsi_data *dsi = tegra_dc_get_outdata(dc);

	if (!dsi) {
		dev_err(&dc->ndev->dev, " dsi out_data not found\n");
		return -EINVAL;
	}

	dsi->max_instances = dc->out->dsi->ganged_type ? MAX_DSI_INSTANCE : 1;

	mutex_lock(&dsi->lock);
	tegra_dc_io_start(dc);
	for (i = 0; i < dsi->max_instances; i++) {
		if (dsi->base[i]) {
			iounmap(dsi->base[i]);
			dsi->base[i] = NULL;
		}
	}

	if (dsi->avdd_dsi_csi) {
		dsi->avdd_dsi_csi = NULL;
	}

#if defined (CONFIG_TEGRA_NVDISPLAY)
	if (dsi->pad_ctrl)
		tegra_dsi_padctrl_shutdown(dc);
#endif

	tegra_dc_io_end(dc);
	mutex_unlock(&dsi->lock);

	return 0;
}

int tegra_dc_reinit_dsi_resources(struct tegra_dc *dc, long dc_outtype)
{
	int err = 0, i;
	int dsi_instance;
	void __iomem *base;
	struct device_node *np_dsi = tegra_dc_get_conn_np(dc);
	struct tegra_dc_dsi_data *dsi = tegra_dc_get_outdata(dc);

	if (!dsi) {
		dev_err(&dc->ndev->dev, " dsi: allocation deleted\n");
		return -ENOMEM;
	}

	/* Since all fake DSI share the same DSI pointer, need to reset here */
	/* to avoid misconfigurations when switching between fake DSI types */
	tegra_dc_reset_fakedsi_panel(dc, dc_outtype);

	dsi->max_instances = is_simple_dsi(dc->out->dsi) ? 1 : MAX_DSI_INSTANCE;
	dsi_instance = (int)dc->out->dsi->dsi_instance;

	for (i = 0; i < dsi->max_instances; i++) {
		if (is_simple_dsi(dc->out->dsi))
			base = of_iomap(np_dsi, dsi_instance);
		else /* ganged type OR split link*/
			base = of_iomap(np_dsi, i);

		if (!base) {
			dev_err(&dc->ndev->dev, "dsi: ioremap failed\n");
			err = -ENOENT;
			goto err_iounmap;
		}

		dsi->base[i] = base;
	}

	dsi->avdd_dsi_csi =  devm_regulator_get(&dc->ndev->dev, "avdd_dsi_csi");
	if (IS_ERR(dsi->avdd_dsi_csi)) {
		dev_err(&dc->ndev->dev, "dsi: avdd_dsi_csi reg get failed\n");
		err = -ENODEV;
		goto err_release_regs;
	}
#if defined (CONFIG_TEGRA_NVDISPLAY)
	dsi->pad_ctrl = tegra_dsi_padctrl_init(dc);
	if (IS_ERR(dsi->pad_ctrl)) {
		dev_err(&dc->ndev->dev, "dsi: Padctrl sw init failed\n");
		err = PTR_ERR(dsi->pad_ctrl);
		goto err_release_regs;
	}
	dsi->info.enable_hs_clock_on_lp_cmd_mode = true;
#endif
	/* Need to always reinitialize clocks to ensure proper functionality */
	tegra_dsi_init_clock_param(dc);
	return 0;

err_release_regs:
	if (dsi->avdd_dsi_csi)
		dsi->avdd_dsi_csi = NULL;

err_iounmap:
	for (; i >= 0; i--) {
		if (dsi->base[i]) {
			iounmap(dsi->base[i]);
			dsi->base[i] = NULL;
		}
	}

	return err;
}
