/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2017 MediaTek Inc.
 */

#ifndef __MTK_MCDI_PLAT_H__
#define __MTK_MCDI_PLAT_H__

#define NF_CPU                  4
#define NF_CLUSTER              1
#define CPU_PWR_STAT_MASK       0x0000000F
#define CLUSTER_PWR_STAT_MASK   0x00010000

#define cpu_is_invalid(id)      (!(id >= 0 && id < NF_CPU))
#define cluster_is_invalid(id)  (id != 0)

/* MCDI mbox interface */
#define MCDI_SSPM_INTF
/* #define MCDI_MCUPM_INTF */

enum {
	ALL_CPU_IN_CLUSTER = 0,
	CPU_CLUSTER,
	CPU_IN_OTHER_CLUSTER,
	NF_PWR_STAT_MAP_TYPE
};

enum {
	CPU_TYPE_L,

	NF_CPU_TYPE
};

#define get_cpu_type_str(i) \
	(i == 0 ? "cpu L" : "unknown")

unsigned int get_pwr_stat_check_map(int type, int idx);
int cluster_idx_get(int cpu);
int cpu_type_idx_get(int cpu);

void mcdi_status_init(void);
void mcdi_of_init(void **base);

#endif /* __MTK_MCDI_PLAT_H__ */
