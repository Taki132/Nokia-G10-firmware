/*
 * Copyright (C) 2018, SI-IN, Yun Shi (yun.shi@si-in.com).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#ifndef _SIA81XX_REGMAP_H
#define _SIA81XX_REGMAP_H

struct sia81xx_reg_vals {
	const char *vals;
	const unsigned int num;
};

struct sia81xx_reg_default_val {
	unsigned int chip_type;
	const unsigned int offset;
	struct sia81xx_reg_vals reg_defaults[AUDIO_SCENE_NUM];
};

struct sia81xx_opt_if {
	int (*check_chip_id)(struct regmap *regmap);
	void (*set_xfilter)(struct regmap *regmap, unsigned int val);
	void (*chip_on)(struct regmap *regmap);
	void (*chip_off)(struct regmap *regmap);
	bool (*get_chip_en)(struct regmap *regmap);
};


int sia81xx_regmap_read(struct regmap *regmap, 
	unsigned int start_reg, unsigned int reg_num, char *buf);
int sia81xx_regmap_write(struct regmap *regmap, 
	unsigned int start_reg, unsigned int reg_num, const char *buf);
void sia81xx_regmap_defaults(struct regmap *regmap, 
	unsigned int chip_type, unsigned int scene, unsigned int channel_num);
struct regmap *sia81xx_regmap_init(struct i2c_client *client, 
	unsigned int chip_type);
void sia81xx_regmap_remove(struct regmap *regmap);


/* option interface function */
int sia81xx_regmap_check_chip_id(struct regmap *regmap, 
	unsigned int chip_type);
void sia81xx_regmap_set_xfilter(struct regmap *regmap, 
	unsigned int chip_type, unsigned int val);
void sia81xx_regmap_set_chip_on(struct regmap *regmap, 
	unsigned int chip_type);
void sia81xx_regmap_set_chip_off(struct regmap *regmap, 
	unsigned int chip_type);
bool sia81xx_regmap_get_chip_en(struct regmap *regmap, 
	unsigned int chip_type);

#endif /* _SIA81XX_REGMAP_H */

