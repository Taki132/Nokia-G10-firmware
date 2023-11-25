// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <mtk_dbg_common_v1.h>
#include <mtk_lpm_module.h>
#include <gs/mtk_lpm_pwr_gs.h>
#include <mt6781_dbg_fs_common.h>
#include <mt6781_power_gs_compare.h>

static void __exit mt6781_dbg_exit(void)
{
#ifdef MTK_LPM_DBG_COMMON
	mtk_dbg_common_fs_exit();
#endif
	mt6781_dbg_lpm_fs_deinit();
	mt6781_dbg_spm_fs_deinit();
	mt6781_dbg_lpm_deinit();
#ifdef CONFIG_MTK_LPM_GS_DUMP_SUPPORT
	mt6781_power_gs_deinit();
#endif
}

static int __init mt6781_dbg_init(void)
{
#ifdef MTK_LPM_DBG_COMMON
	mtk_dbg_common_fs_init();
#endif
	mt6781_dbg_lpm_init();
	mt6781_dbg_lpm_fs_init();
	mt6781_dbg_spm_fs_init();
#ifdef CONFIG_MTK_LPM_GS_DUMP_SUPPORT
	mt6781_power_gs_init();
#endif
	pr_info("%s %d: finish", __func__, __LINE__);
	return 0;
}

module_init(mt6781_dbg_init);
module_exit(mt6781_dbg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6781 Low Power FileSystem");
MODULE_AUTHOR("MediaTek Inc.");
