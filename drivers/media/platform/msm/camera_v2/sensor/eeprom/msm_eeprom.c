/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include "msm_sd.h"
#include "msm_cci.h"
#include "msm_eeprom.h"
#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/04/15, add for pdaf engineer mode*/
#include <linux/proc_fs.h>
#include <soc/oppo/device_info.h>
#include "hi545.h"
#include "ov8865.h"
#endif

#undef CDBG
#define CDBG(fmt, args...) pr_debug(fmt, ##args)

DEFINE_MSM_MUTEX(msm_eeprom_mutex);
#ifdef CONFIG_COMPAT
static struct v4l2_file_operations msm_eeprom_v4l2_subdev_fops;
#endif
/**
  * msm_eeprom_verify_sum - verify crc32 checksum
  * @mem:	data buffer
  * @size:	size of data buffer
  * @sum:	expected checksum
  *
  * Returns 0 if checksum match, -EINVAL otherwise.
  */
static int msm_eeprom_verify_sum(const char *mem, uint32_t size, uint32_t sum)
{
	uint32_t crc = ~0;

	/* check overflow */
	if (size > crc - sizeof(uint32_t))
		return -EINVAL;

	crc = crc32_le(crc, mem, size);
	if (~crc != sum) {
		CDBG("%s: expect 0x%x, result 0x%x\n", __func__, sum, ~crc);
		return -EINVAL;
	}
	CDBG("%s: checksum pass 0x%x\n", __func__, sum);
	return 0;
}

/**
  * msm_eeprom_match_crc - verify multiple regions using crc
  * @data:	data block to be verified
  *
  * Iterates through all regions stored in @data.  Regions with odd index
  * are treated as data, and its next region is treated as checksum.  Thus
  * regions of even index must have valid_size of 4 or 0 (skip verification).
  * Returns a bitmask of verified regions, starting from LSB.  1 indicates
  * a checksum match, while 0 indicates checksum mismatch or not verified.
  */
static uint32_t msm_eeprom_match_crc(struct msm_eeprom_memory_block_t *data)
{
	int j, rc;
	uint32_t *sum;
	uint32_t ret = 0;
	uint8_t *memptr;
	struct msm_eeprom_memory_map_t *map;

	if (!data) {
		pr_err("%s data is NULL", __func__);
		return -EINVAL;
	}
	map = data->map;
	memptr = data->mapdata;

	for (j = 0; j + 1 < data->num_map; j += 2) {
		/* empty table or no checksum */
		if (!map[j].mem.valid_size || !map[j+1].mem.valid_size) {
			memptr += map[j].mem.valid_size
				+ map[j+1].mem.valid_size;
			continue;
		}
		if (map[j+1].mem.valid_size != sizeof(uint32_t)) {
			CDBG("%s: malformatted data mapping\n", __func__);
			return -EINVAL;
		}
		sum = (uint32_t *) (memptr + map[j].mem.valid_size);
		rc = msm_eeprom_verify_sum(memptr, map[j].mem.valid_size,
					   *sum);
		if (!rc)
			ret |= 1 << (j/2);
		memptr += map[j].mem.valid_size + map[j+1].mem.valid_size;
	}
	return ret;
}

static int msm_eeprom_get_cmm_data(struct msm_eeprom_ctrl_t *e_ctrl,
				       struct msm_eeprom_cfg_data *cdata)
{
	int rc = 0;
	struct msm_eeprom_cmm_t *cmm_data = &e_ctrl->eboard_info->cmm_data;
	cdata->cfg.get_cmm_data.cmm_support = cmm_data->cmm_support;
	cdata->cfg.get_cmm_data.cmm_compression = cmm_data->cmm_compression;
	cdata->cfg.get_cmm_data.cmm_size = cmm_data->cmm_size;
	return rc;
}

static int eeprom_config_read_cal_data(struct msm_eeprom_ctrl_t *e_ctrl,
	struct msm_eeprom_cfg_data *cdata)
{
	int rc;

	/* check range */
	if (cdata->cfg.read_data.num_bytes >
		e_ctrl->cal_data.num_data) {
		CDBG("%s: Invalid size. exp %u, req %u\n", __func__,
			e_ctrl->cal_data.num_data,
			cdata->cfg.read_data.num_bytes);
		return -EINVAL;
	}
	if (!e_ctrl->cal_data.mapdata)
		return -EFAULT;

	rc = copy_to_user(cdata->cfg.read_data.dbuffer,
		e_ctrl->cal_data.mapdata,
		cdata->cfg.read_data.num_bytes);

	return rc;
}

static int msm_eeprom_config(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *argp)
{
	struct msm_eeprom_cfg_data *cdata =
		(struct msm_eeprom_cfg_data *)argp;
	int rc = 0;

	CDBG("%s E\n", __func__);
	switch (cdata->cfgtype) {
	case CFG_EEPROM_GET_INFO:
		CDBG("%s E CFG_EEPROM_GET_INFO\n", __func__);
		cdata->is_supported = e_ctrl->is_supported;
		memcpy(cdata->cfg.eeprom_name,
			e_ctrl->eboard_info->eeprom_name,
			sizeof(cdata->cfg.eeprom_name));
		break;
	case CFG_EEPROM_GET_CAL_DATA:
		CDBG("%s E CFG_EEPROM_GET_CAL_DATA\n", __func__);
		cdata->cfg.get_data.num_bytes =
			e_ctrl->cal_data.num_data;
		break;
	case CFG_EEPROM_READ_CAL_DATA:
		CDBG("%s E CFG_EEPROM_READ_CAL_DATA\n", __func__);
		rc = eeprom_config_read_cal_data(e_ctrl, cdata);
		break;
	case CFG_EEPROM_GET_MM_INFO:
		CDBG("%s E CFG_EEPROM_GET_MM_INFO\n", __func__);
		rc = msm_eeprom_get_cmm_data(e_ctrl, cdata);
		break;
	default:
		break;
	}

	CDBG("%s X rc: %d\n", __func__, rc);
	return rc;
}

static int msm_eeprom_get_subdev_id(struct msm_eeprom_ctrl_t *e_ctrl,
				    void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("%s E\n", __func__);
	if (!subdev_id) {
		pr_err("%s failed\n", __func__);
		return -EINVAL;
	}
	*subdev_id = e_ctrl->subdev_id;
	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("%s X\n", __func__);
	return 0;
}

static long msm_eeprom_subdev_ioctl(struct v4l2_subdev *sd,
		unsigned int cmd, void *arg)
{
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;
	CDBG("%s E\n", __func__);
	CDBG("%s:%d a_ctrl %p argp %p\n", __func__, __LINE__, e_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_eeprom_get_subdev_id(e_ctrl, argp);
	case VIDIOC_MSM_EEPROM_CFG:
		return msm_eeprom_config(e_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}

	CDBG("%s X\n", __func__);
}

static struct msm_camera_i2c_fn_t msm_eeprom_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_seq = msm_camera_cci_i2c_write_seq,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
	msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_poll = msm_camera_cci_i2c_poll,
};

static struct msm_camera_i2c_fn_t msm_eeprom_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
	msm_camera_qup_i2c_write_table_w_microdelay,
};

static struct msm_camera_i2c_fn_t msm_eeprom_spi_func_tbl = {
	.i2c_read = msm_camera_spi_read,
	.i2c_read_seq = msm_camera_spi_read_seq,
};

static int msm_eeprom_open(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_eeprom_ctrl_t *e_ctrl =  v4l2_get_subdevdata(sd);
	CDBG("%s E\n", __func__);
	if (!e_ctrl) {
		pr_err("%s failed e_ctrl is NULL\n", __func__);
		return -EINVAL;
	}
	CDBG("%s X\n", __func__);
	return rc;
}

static int msm_eeprom_close(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_eeprom_ctrl_t *e_ctrl =  v4l2_get_subdevdata(sd);
	CDBG("%s E\n", __func__);
	if (!e_ctrl) {
		pr_err("%s failed e_ctrl is NULL\n", __func__);
		return -EINVAL;
	}
	CDBG("%s X\n", __func__);
	return rc;
}

static const struct v4l2_subdev_internal_ops msm_eeprom_internal_ops = {
	.open = msm_eeprom_open,
	.close = msm_eeprom_close,
};
/**
  * read_eeprom_memory() - read map data into buffer
  * @e_ctrl:	eeprom control struct
  * @block:	block to be read
  *
  * This function iterates through blocks stored in block->map, reads each
  * region and concatenate them into the pre-allocated block->mapdata
  */
static int read_eeprom_memory(struct msm_eeprom_ctrl_t *e_ctrl,
			      struct msm_eeprom_memory_block_t *block)
{
	int rc = 0;
	int j;
	struct msm_eeprom_memory_map_t *emap = block->map;
	struct msm_eeprom_board_info *eb_info;
	uint8_t *memptr = block->mapdata;

	if (!e_ctrl) {
		pr_err("%s e_ctrl is NULL", __func__);
		return -EINVAL;
	}

	eb_info = e_ctrl->eboard_info;

	for (j = 0; j < block->num_map; j++) {
		if (emap[j].saddr.addr) {
			eb_info->i2c_slaveaddr = emap[j].saddr.addr;
			e_ctrl->i2c_client.cci_client->sid =
					eb_info->i2c_slaveaddr >> 1;
			pr_err("qcom,slave-addr = 0x%X\n",
				eb_info->i2c_slaveaddr);
		}

		if (emap[j].page.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].page.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].page.addr,
				emap[j].page.data, emap[j].page.data_t);
				msleep(emap[j].page.delay);
			if (rc < 0) {
				pr_err("%s: page write failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				emap[j].pageen.data, emap[j].pageen.data_t);
				msleep(emap[j].pageen.delay);
			if (rc < 0) {
				pr_err("%s: page enable failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].poll.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].poll.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_poll(
				&(e_ctrl->i2c_client), emap[j].poll.addr,
				emap[j].poll.data, emap[j].poll.data_t);
				msleep(emap[j].poll.delay);
			if (rc < 0) {
				pr_err("%s: poll failed\n", __func__);
				return rc;
			}
		}

		if (emap[j].mem.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].mem.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
				&(e_ctrl->i2c_client), emap[j].mem.addr,
				memptr, emap[j].mem.valid_size);
			if (rc < 0) {
				pr_err("%s: read failed\n", __func__);
				return rc;
			}
			memptr += emap[j].mem.valid_size;
		}
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				0, emap[j].pageen.data_t);
			if (rc < 0) {
				pr_err("%s: page disable failed\n", __func__);
				return rc;
			}
		}
	}
	return rc;
}
/**
  * msm_eeprom_parse_memory_map() - parse memory map in device node
  * @of:	device node
  * @data:	memory block for output
  *
  * This functions parses @of to fill @data.  It allocates map itself, parses
  * the @of node, calculate total data length, and allocates required buffer.
  * It only fills the map, but does not perform actual reading.
  */
static int msm_eeprom_parse_memory_map(struct device_node *of,
				       struct msm_eeprom_memory_block_t *data)
{
	int i, rc = 0;
	char property[PROPERTY_MAXSIZE];
	uint32_t count = 6;
	struct msm_eeprom_memory_map_t *map;

	snprintf(property, PROPERTY_MAXSIZE, "qcom,num-blocks");
	rc = of_property_read_u32(of, property, &data->num_map);
	CDBG("%s: %s %d\n", __func__, property, data->num_map);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		return rc;
	}

	map = kzalloc((sizeof(*map) * data->num_map), GFP_KERNEL);
	if (!map) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		return -ENOMEM;
	}
	data->map = map;

	for (i = 0; i < data->num_map; i++) {
		snprintf(property, PROPERTY_MAXSIZE, "qcom,page%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].page, count);
		if (rc < 0) {
			pr_err("%s: failed %d\n", __func__, __LINE__);
			goto ERROR;
		}

		snprintf(property, PROPERTY_MAXSIZE,
					"qcom,pageen%d", i);
		rc = of_property_read_u32_array(of, property,
			(uint32_t *) &map[i].pageen, count);
		if (rc < 0)
			pr_err("%s: pageen not needed\n", __func__);

		snprintf(property, PROPERTY_MAXSIZE, "qcom,saddr%d", i);
		rc = of_property_read_u32_array(of, property,
			(uint32_t *) &map[i].saddr.addr, 1);
		if (rc < 0)
			CDBG("%s: saddr not needed - block %d\n", __func__, i);

		snprintf(property, PROPERTY_MAXSIZE, "qcom,poll%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].poll, count);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR;
		}

		snprintf(property, PROPERTY_MAXSIZE, "qcom,mem%d", i);
		rc = of_property_read_u32_array(of, property,
				(uint32_t *) &map[i].mem, count);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR;
		}
		data->num_data += map[i].mem.valid_size;
	}

	CDBG("%s num_bytes %d\n", __func__, data->num_data);

	data->mapdata = kzalloc(data->num_data, GFP_KERNEL);
	if (!data->mapdata) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto ERROR;
	}
	return rc;

ERROR:
	kfree(data->map);
	memset(data, 0, sizeof(*data));
	return rc;
}

static struct msm_cam_clk_info cam_8960_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_clk", 24000000},
};

static struct msm_cam_clk_info cam_8974_clk_info[] = {
	[SENSOR_CAM_MCLK] = {"cam_src_clk", 19200000},
	[SENSOR_CAM_CLK] = {"cam_clk", 0},
};

static struct v4l2_subdev_core_ops msm_eeprom_subdev_core_ops = {
	.ioctl = msm_eeprom_subdev_ioctl,
};

static struct v4l2_subdev_ops msm_eeprom_subdev_ops = {
	.core = &msm_eeprom_subdev_core_ops,
};

static int msm_eeprom_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int rc = 0;
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_camera_power_ctrl_t *power_info = NULL;
	CDBG("%s E\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s i2c_check_functionality failed\n", __func__);
		goto probe_failure;
	}

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;
	CDBG("%s client = 0x%p\n", __func__, client);
	e_ctrl->eboard_info = (struct msm_eeprom_board_info *)(id->driver_data);
	if (!e_ctrl->eboard_info) {
		pr_err("%s:%d board info NULL\n", __func__, __LINE__);
		rc = -EINVAL;
		goto ectrl_free;
	}
	power_info = &e_ctrl->eboard_info->power_info;
	e_ctrl->i2c_client.client = client;

	/* Set device type as I2C */
	e_ctrl->eeprom_device_type = MSM_CAMERA_I2C_DEVICE;
	e_ctrl->i2c_client.i2c_func_tbl = &msm_eeprom_qup_func_tbl;

	if (e_ctrl->eboard_info->i2c_slaveaddr != 0)
		e_ctrl->i2c_client.client->addr =
					e_ctrl->eboard_info->i2c_slaveaddr;
	power_info->clk_info = cam_8960_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8960_clk_info);
	power_info->dev = &client->dev;

	/*IMPLEMENT READING PART*/
	/* Initialize sub device */
	v4l2_i2c_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->i2c_client.client,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);
	CDBG("%s success result=%d X\n", __func__, rc);
	return rc;

ectrl_free:
	kfree(e_ctrl);
probe_failure:
	pr_err("%s failed! rc = %d\n", __func__, rc);
	return rc;
}

static int msm_eeprom_i2c_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

#define msm_eeprom_spi_parse_cmd(spic, str, name, out, size)		\
	{								\
		if (of_property_read_u32_array(				\
			spic->spi_master->dev.of_node,			\
			str, out, size)) {				\
			return -EFAULT;					\
		} else {						\
			spic->cmd_tbl.name.opcode = out[0];		\
			spic->cmd_tbl.name.addr_len = out[1];		\
			spic->cmd_tbl.name.dummy_len = out[2];		\
		}							\
	}

static int msm_eeprom_spi_parse_of(struct msm_camera_spi_client *spic)
{
	int rc = -EFAULT;
	uint32_t tmp[3];
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop,read", read, tmp, 3);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop,readseq", read_seq, tmp, 3);
	msm_eeprom_spi_parse_cmd(spic, "qcom,spiop,queryid", query_id, tmp, 3);

	rc = of_property_read_u32_array(spic->spi_master->dev.of_node,
					"qcom,eeprom-id", tmp, 2);
	if (rc) {
		pr_err("%s: Failed to get eeprom id\n", __func__);
		return rc;
	}
	spic->mfr_id0 = tmp[0];
	spic->device_id0 = tmp[1];

	return 0;
}

static int msm_eeprom_match_id(struct msm_eeprom_ctrl_t *e_ctrl)
{
	int rc;
	struct msm_camera_i2c_client *client = &e_ctrl->i2c_client;
	uint8_t id[2];

	rc = msm_camera_spi_query_id(client, 0, &id[0], 2);
	if (rc < 0)
		return rc;
	CDBG("%s: read 0x%x 0x%x, check 0x%x 0x%x\n", __func__, id[0],
		id[1], client->spi_client->mfr_id0,
			client->spi_client->device_id0);
	if (id[0] != client->spi_client->mfr_id0
		    || id[1] != client->spi_client->device_id0)
		return -ENODEV;

	return 0;
}

static int msm_eeprom_get_dt_data(struct msm_eeprom_ctrl_t *e_ctrl)
{
	int rc = 0, i = 0;
	struct msm_eeprom_board_info *eb_info;
	struct msm_camera_power_ctrl_t *power_info =
		&e_ctrl->eboard_info->power_info;
	struct device_node *of_node = NULL;
	struct msm_camera_gpio_conf *gconf = NULL;
	uint16_t gpio_array_size = 0;
	uint16_t *gpio_array = NULL;

	eb_info = e_ctrl->eboard_info;
	if (e_ctrl->eeprom_device_type == MSM_CAMERA_SPI_DEVICE)
		of_node = e_ctrl->i2c_client.
			spi_client->spi_master->dev.of_node;
	else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		of_node = e_ctrl->pdev->dev.of_node;

	if (!of_node) {
		pr_err("%s: %d of_node is NULL\n", __func__ , __LINE__);
		return -ENOMEM;
	}
	rc = msm_camera_get_dt_vreg_data(of_node, &power_info->cam_vreg,
					     &power_info->num_vreg);
	if (rc < 0)
		return rc;

	rc = msm_camera_get_dt_power_setting_data(of_node,
		power_info->cam_vreg, power_info->num_vreg,
		power_info);
	if (rc < 0)
		goto ERROR1;

	power_info->gpio_conf = kzalloc(sizeof(struct msm_camera_gpio_conf),
					GFP_KERNEL);
	if (!power_info->gpio_conf) {
		rc = -ENOMEM;
		goto ERROR2;
	}
	gconf = power_info->gpio_conf;
	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size) {
		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR3;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_camera_get_dt_gpio_req_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR4;
		}

		rc = msm_camera_init_gpio_pin_tbl(of_node, gconf,
			gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR4;
		}
		kfree(gpio_array);
	}

	return rc;
ERROR4:
	kfree(gpio_array);
ERROR3:
	kfree(power_info->gpio_conf);
ERROR2:
	kfree(power_info->cam_vreg);
ERROR1:
	kfree(power_info->power_setting);
	return rc;
}


static int msm_eeprom_cmm_dts(struct msm_eeprom_board_info *eb_info,
				struct device_node *of_node)
{
	int rc = 0;
	struct msm_eeprom_cmm_t *cmm_data = &eb_info->cmm_data;

	cmm_data->cmm_support =
		of_property_read_bool(of_node, "qcom,cmm-data-support");
	if (!cmm_data->cmm_support)
		return -EINVAL;
	cmm_data->cmm_compression =
		of_property_read_bool(of_node, "qcom,cmm-data-compressed");
	if (!cmm_data->cmm_compression)
		CDBG("No MM compression data\n");

	rc = of_property_read_u32(of_node, "qcom,cmm-data-offset",
				  &cmm_data->cmm_offset);
	if (rc < 0)
		CDBG("No MM offset data\n");

	rc = of_property_read_u32(of_node, "qcom,cmm-data-size",
				  &cmm_data->cmm_size);
	if (rc < 0)
		CDBG("No MM size data\n");

	CDBG("cmm_support: cmm_compr %d, cmm_offset %d, cmm_size %d\n",
		cmm_data->cmm_compression,
		cmm_data->cmm_offset,
		cmm_data->cmm_size);
	return 0;
}

static int msm_eeprom_spi_setup(struct spi_device *spi)
{
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_camera_i2c_client *client = NULL;
	struct msm_camera_spi_client *spi_client;
	struct msm_eeprom_board_info *eb_info;
	struct msm_camera_power_ctrl_t *power_info = NULL;
	int rc = 0;

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;
	client = &e_ctrl->i2c_client;
	e_ctrl->is_supported = 0;

	spi_client = kzalloc(sizeof(*spi_client), GFP_KERNEL);
	if (!spi_client) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		kfree(e_ctrl);
		return -ENOMEM;
	}

	rc = of_property_read_u32(spi->dev.of_node, "cell-index",
				  &e_ctrl->subdev_id);
	CDBG("cell-index %d, rc %d\n", e_ctrl->subdev_id, rc);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}

	e_ctrl->eeprom_device_type = MSM_CAMERA_SPI_DEVICE;
	client->spi_client = spi_client;
	spi_client->spi_master = spi;
	client->i2c_func_tbl = &msm_eeprom_spi_func_tbl;
	client->addr_type = MSM_CAMERA_I2C_3B_ADDR;

	eb_info = kzalloc(sizeof(*eb_info), GFP_KERNEL);
	if (!eb_info)
		goto spi_free;
	e_ctrl->eboard_info = eb_info;
	rc = of_property_read_string(spi->dev.of_node, "qcom,eeprom-name",
		&eb_info->eeprom_name);
	CDBG("%s qcom,eeprom-name %s, rc %d\n", __func__,
		eb_info->eeprom_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto board_free;
	}

	rc = msm_eeprom_cmm_dts(e_ctrl->eboard_info, spi->dev.of_node);
	if (rc < 0)
		CDBG("%s MM data miss:%d\n", __func__, __LINE__);

	power_info = &eb_info->power_info;

	power_info->clk_info = cam_8974_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8974_clk_info);
	power_info->dev = &spi->dev;

	rc = msm_eeprom_get_dt_data(e_ctrl);
	if (rc < 0)
		goto board_free;

	/* set spi instruction info */
	spi_client->retry_delay = 1;
	spi_client->retries = 0;

	rc = msm_eeprom_spi_parse_of(spi_client);
	if (rc < 0) {
		dev_err(&spi->dev,
			"%s: Error parsing device properties\n", __func__);
		goto board_free;
	}

	/* prepare memory buffer */
	rc = msm_eeprom_parse_memory_map(spi->dev.of_node,
					 &e_ctrl->cal_data);
	if (rc < 0)
		CDBG("%s: no cal memory map\n", __func__);

	/* power up eeprom for reading */
	rc = msm_camera_power_up(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		goto caldata_free;
	}

	/* check eeprom id */
	rc = msm_eeprom_match_id(e_ctrl);
	if (rc < 0) {
		CDBG("%s: eeprom not matching %d\n", __func__, rc);
		goto power_down;
	}
	/* read eeprom */
	if (e_ctrl->cal_data.map) {
		rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
		if (rc < 0) {
			pr_err("%s: read cal data failed\n", __func__);
			goto power_down;
		}
		e_ctrl->is_supported |= msm_eeprom_match_crc(
						&e_ctrl->cal_data);
	}

	rc = msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		goto caldata_free;
	}

	/* initiazlie subdev */
	v4l2_spi_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->i2c_client.spi_client->spi_master,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);
	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	CDBG("%s success result=%d supported=%x X\n", __func__, rc,
	     e_ctrl->is_supported);

	return 0;

power_down:
	msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
caldata_free:
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
board_free:
	kfree(e_ctrl->eboard_info);
spi_free:
	kfree(spi_client);
	kfree(e_ctrl);
	return rc;
}

static int msm_eeprom_spi_probe(struct spi_device *spi)
{
	int irq, cs, cpha, cpol, cs_high;

	CDBG("%s\n", __func__);
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	spi_setup(spi);

	irq = spi->irq;
	cs = spi->chip_select;
	cpha = (spi->mode & SPI_CPHA) ? 1 : 0;
	cpol = (spi->mode & SPI_CPOL) ? 1 : 0;
	cs_high = (spi->mode & SPI_CS_HIGH) ? 1 : 0;
	CDBG("%s: irq[%d] cs[%x] CPHA[%x] CPOL[%x] CS_HIGH[%x]\n",
			__func__, irq, cs, cpha, cpol, cs_high);
	CDBG("%s: max_speed[%u]\n", __func__, spi->max_speed_hz);

	return msm_eeprom_spi_setup(spi);
}

static int msm_eeprom_spi_remove(struct spi_device *sdev)
{
	struct v4l2_subdev *sd = spi_get_drvdata(sdev);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->i2c_client.spi_client);
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

#ifdef CONFIG_COMPAT
static int eeprom_config_read_cal_data32(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *arg)
{
	int rc;
	uint8_t *ptr_dest = NULL;
	struct msm_eeprom_cfg_data32 *cdata32 =
		(struct msm_eeprom_cfg_data32 *) arg;
	struct msm_eeprom_cfg_data cdata;

	cdata.cfgtype = cdata32->cfgtype;
	cdata.is_supported = cdata32->is_supported;
	cdata.cfg.read_data.num_bytes = cdata32->cfg.read_data.num_bytes;
	/* check range */
	if (cdata.cfg.read_data.num_bytes >
	    e_ctrl->cal_data.num_data) {
		CDBG("%s: Invalid size. exp %u, req %u\n", __func__,
			e_ctrl->cal_data.num_data,
			cdata.cfg.read_data.num_bytes);
		return -EINVAL;
	}
	if (!e_ctrl->cal_data.mapdata)
		return -EFAULT;

	ptr_dest = (uint8_t *) compat_ptr(cdata32->cfg.read_data.dbuffer);

	rc = copy_to_user(ptr_dest, e_ctrl->cal_data.mapdata,
		cdata.cfg.read_data.num_bytes);

#ifndef VENDOR_EDIT
/*deleted by Jinshui.Liu@Camera 20141018 start for need it after kill daemon*/
	/* should only be called once.  free kernel resource */
	if (!rc) {
		kfree(e_ctrl->cal_data.mapdata);
		kfree(e_ctrl->cal_data.map);
		memset(&e_ctrl->cal_data, 0, sizeof(e_ctrl->cal_data));
	}
#endif
	return rc;
}

static int msm_eeprom_config32(struct msm_eeprom_ctrl_t *e_ctrl,
	void __user *argp)
{
	struct msm_eeprom_cfg_data *cdata = (struct msm_eeprom_cfg_data *)argp;
	int rc = 0;

	CDBG("%s E\n", __func__);
	switch (cdata->cfgtype) {
	case CFG_EEPROM_GET_INFO:
		CDBG("%s E CFG_EEPROM_GET_INFO\n", __func__);
		cdata->is_supported = e_ctrl->is_supported;
		memcpy(cdata->cfg.eeprom_name,
			e_ctrl->eboard_info->eeprom_name,
			sizeof(cdata->cfg.eeprom_name));
		break;
	case CFG_EEPROM_GET_CAL_DATA:
		CDBG("%s E CFG_EEPROM_GET_CAL_DATA\n", __func__);
		cdata->cfg.get_data.num_bytes =
			e_ctrl->cal_data.num_data;
		break;
	case CFG_EEPROM_READ_CAL_DATA:
		CDBG("%s E CFG_EEPROM_READ_CAL_DATA\n", __func__);
		rc = eeprom_config_read_cal_data32(e_ctrl, argp);
		break;
	default:
		break;
	}

	CDBG("%s X rc: %d\n", __func__, rc);
	return rc;
}

static long msm_eeprom_subdev_ioctl32(struct v4l2_subdev *sd,
		unsigned int cmd, void *arg)
{
	struct msm_eeprom_ctrl_t *e_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;

	CDBG("%s E\n", __func__);
	CDBG("%s:%d a_ctrl %p argp %p\n", __func__, __LINE__, e_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_eeprom_get_subdev_id(e_ctrl, argp);
	case VIDIOC_MSM_EEPROM_CFG32:
		return msm_eeprom_config32(e_ctrl, argp);
	default:
		return -ENOIOCTLCMD;
	}

	CDBG("%s X\n", __func__);
}

static long msm_eeprom_subdev_do_ioctl32(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);

	return msm_eeprom_subdev_ioctl32(sd, cmd, arg);
}

static long msm_eeprom_subdev_fops_ioctl32(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_eeprom_subdev_do_ioctl32);
}

#endif

#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/03/02, modify for 15013 eeprom*/
uint16_t s5k3m2_module = 0;
uint16_t s5k3m2_lsc_info = 0;
extern bool pdaf_calibration_flag;
static void msm_eeprom_s5k3m2_read_vendorInfo(struct msm_eeprom_ctrl_t *e_ctrl)
{
    int rc = 0;
    uint16_t read_data = 0;

    e_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_WORD_ADDR;

    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
            		&e_ctrl->i2c_client, 0x0031,
            		&read_data, MSM_CAMERA_I2C_BYTE_DATA);
    if (rc < 0) {
		pr_err("%s read 0x0031 failed\n", __func__);
	} else {
        pr_err("%s read 0x0031=%d\n", __func__,read_data);
    }

    if (read_data == 0x01) {
        int i;
        uint16_t sum1 = 0, read_value = 0;
        for (i = 0; i <= 0x1B; i++) {
            rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
        		&e_ctrl->i2c_client, i,
        		&read_data, MSM_CAMERA_I2C_BYTE_DATA);
            if (rc < 0) {
        		pr_err("%s read 0x%x fail\n", __func__, i);
        	} else {
        	    if (i == 0) {
                    read_value = read_data;
                    //pr_err("%s read 0x%x=%d\n", __func__, i, read_value);
        	    }
                sum1 += read_data;
            }
        }
        sum1 = sum1%255;

        rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
        		&e_ctrl->i2c_client, 0x0032,
        		&read_data, MSM_CAMERA_I2C_BYTE_DATA);

        if (sum1 == read_data) {
            pr_err("%s read module info success,value=%d\n", __func__, read_value);
            s5k3m2_module = read_value;
        }

        //read PDAF calibration
        read_data = 0;
        read_value = 0;
        //PDAF Gain Map Flag
        rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
        		&e_ctrl->i2c_client, 0x0B08,
        		&read_data, MSM_CAMERA_I2C_BYTE_DATA);
        if (rc < 0) {
            pr_err("%s read 0x0B08 failed\n", __func__);
        }

		//PDAF PCC Flag
        rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
        		&e_ctrl->i2c_client, 0x0B0A,
        		&read_value, MSM_CAMERA_I2C_BYTE_DATA);
        if (rc < 0) {
            pr_err("%s read 0x0B0A failed\n", __func__);
        }

        if (read_data == 1 && read_value == 1) {
            pr_err("%s pdaf calibration is valid\n", __func__);
            pdaf_calibration_flag = true;
        }


        //read LSC algorithm manufacture
        rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read(
        		&e_ctrl->i2c_client, 0x0030,
        		&s5k3m2_lsc_info, MSM_CAMERA_I2C_BYTE_DATA);
        if (rc < 0) {
            pr_err("%s read 0x0030 failed\n", __func__);
        } else {
            pr_err("%s read 0x0030=%d\n", __func__,s5k3m2_lsc_info);
        }
		
    }
}

#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/04/15, add for pdaf engineer mode*/
static ssize_t s5k3m2_eeprom_proc_read(struct file *filp, char __user *buff,
                        	size_t len, loff_t *data)
{
    char value[2] = {0};

    snprintf(value, sizeof(value), "%d", s5k3m2_lsc_info);

    pr_err("%s,lsc_info=%d,value=%s\n", __func__,s5k3m2_lsc_info,value);
    return simple_read_from_buffer(buff, len, data, value,1);
}

static const struct file_operations s5k3m2_eeprom_test_fops = {
    .owner		= THIS_MODULE,
    .read		= s5k3m2_eeprom_proc_read,
    //.write		= s5k3m2_eeprom_proc_write,
};

static int msm_eeprom_proc_init(void)
{
    int ret=0;
    struct proc_dir_entry *proc_entry;

    proc_entry = proc_create_data("s5k3m2_eeprom_info", 0666, NULL, &s5k3m2_eeprom_test_fops, NULL);
    if (proc_entry == NULL)
    {
		ret = -ENOMEM;
	  	pr_err("[%s]: Error! Couldn't create s5k3m2_eeprom_info proc entry\n", __func__);
    }
    return ret;
}

#endif
#ifdef VENDOR_EDIT
/* lanhe add for read s5k3h7 otp */
static int read_eeprom_memory_3h7(struct msm_eeprom_ctrl_t *e_ctrl,
			      struct msm_eeprom_memory_block_t *block)
{
	int rc = 0;
	int j;
	struct msm_eeprom_memory_map_t *emap = block->map;
	struct msm_eeprom_board_info *eb_info;
	uint8_t *memptr = block->mapdata;

	if (!e_ctrl) {
		pr_err("%s e_ctrl is NULL", __func__);
		return -EINVAL;
	}

	eb_info = e_ctrl->eboard_info;

	for (j = 0; j < block->num_map; j++) {
		if (emap[j].saddr.addr) {
			eb_info->i2c_slaveaddr = emap[j].saddr.addr;
			e_ctrl->i2c_client.cci_client->sid =
					eb_info->i2c_slaveaddr >> 1;
			pr_err("qcom,slave-addr = 0x%X\n",
				eb_info->i2c_slaveaddr);
		}
		
		if (emap[j].page.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].page.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].page.addr,
				emap[j].page.data, emap[j].page.data_t);
				msleep(emap[j].page.delay);
			if (rc < 0) {
				pr_err("%s: page write failed\n", __func__);
				return rc;
			}
		}
		
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				emap[j].pageen.data, emap[j].pageen.data_t);
				msleep(emap[j].pageen.delay);
			if (rc < 0) {
				pr_err("%s: page enable failed\n", __func__);
				return rc;
			}
		}
		
		if (emap[j].poll.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].poll.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].poll.addr,
				emap[j].poll.data, emap[j].poll.data_t);
				msleep(emap[j].poll.delay);
			if (rc < 0) {
				pr_err("%s: poll failed\n", __func__);
				return rc;
			}
		}

		if (emap[j].mem.valid_size) {
			memset(memptr,0x00,emap[j].mem.valid_size);//
			e_ctrl->i2c_client.addr_type = emap[j].mem.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(
				&(e_ctrl->i2c_client), emap[j].mem.addr,
				memptr, emap[j].mem.valid_size);
			if (rc < 0) {
				pr_err("%s: read failed\n", __func__);
				return rc;
			}
			memptr += emap[j].mem.valid_size;
		}

	}
	return rc;
}

//Jindian.guan@Camera.Driver, 2017/06/16, add for hi545 eeprom
static int custom_hynix_define_otp_read(struct msm_eeprom_ctrl_t *e_ctrl,
                struct msm_eeprom_memory_map_t *emap, uint8_t *memptr) {
	int m = 0;
	int k = 0;
	uint32_t addr = 0;
	int rc =0;

	CDBG("dmm debug %s come in\n",__func__);
	//initial  sensor
	for (m = 0; m < sizeof(init_reg_array) / (sizeof(init_reg_array[0])); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	             init_reg_array[m].reg_addr, init_reg_array[m].reg_data, MSM_CAMERA_I2C_WORD_DATA);
	    if (rc < 0) {
	        pr_err("%s: hi553 init  failed\n", __func__);
	        return rc;
	    }
	}

	// set to otp mode
	for (m = 0; m < sizeof(init_otp_array) / sizeof(init_otp_array[0]); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	              init_otp_array[m].reg_addr, init_otp_array[m].reg_data, MSM_CAMERA_I2C_BYTE_DATA);
	    mdelay(init_otp_array[m].delay);
	    if (rc < 0) {
	        pr_err("%s: hi545 to otp mode  failed\n", __func__);
	        return rc;
	    }
	}

	for (addr = emap->mem.addr, k = 0; k < (emap->mem.valid_size); addr++, k++) {
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client), 0x10A, (addr>>8)&0xff, MSM_CAMERA_I2C_BYTE_DATA);
	    rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client), 0x10B, addr & 0xff, MSM_CAMERA_I2C_BYTE_DATA);
	    rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client), 0x102, 0, MSM_CAMERA_I2C_BYTE_DATA);
	    rc |= e_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_WORD_DATA;
	    rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(&(e_ctrl->i2c_client), 0x108, memptr, 1);
	    pr_err("hi545 custom:addr:[0x%04x] value: (0x%x)\n", addr, *memptr);
	    memptr++;  // must
	    if (rc < 0) {
	        pr_err("%s: hi545 read failed\n", __func__);
	        return rc;
	    }
	}

	//set  to normol mode
	for (m = 0; m < sizeof(otp_to_norm_mode_array)/sizeof(otp_to_norm_mode_array[0]); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	            otp_to_norm_mode_array[m].reg_addr, otp_to_norm_mode_array[m].reg_data, 1);
	    if (rc < 0) {
	        pr_err("%s: to normal  failed\n", __func__);
	        return rc;
	    }
	}

	return rc;
}

static int read_eeprom_memory_hi545(struct msm_eeprom_ctrl_t *e_ctrl,
			      struct msm_eeprom_memory_block_t *block)
{
	int rc = 0;
	int j;
	struct msm_eeprom_memory_map_t *emap = block->map;
	struct msm_eeprom_board_info *eb_info;
	uint8_t *memptr = block->mapdata;

	if (!e_ctrl) {
		pr_err("%s e_ctrl is NULL", __func__);
		return -EINVAL;
	}

	eb_info = e_ctrl->eboard_info;

	for (j = 0; j < block->num_map; j++) {
		if (emap[j].saddr.addr) {
			eb_info->i2c_slaveaddr = emap[j].saddr.addr;
			e_ctrl->i2c_client.cci_client->sid =
					eb_info->i2c_slaveaddr >> 1;
			pr_err("qcom,slave-addr = 0x%X\n",
				eb_info->i2c_slaveaddr);
		}

		if (emap[j].page.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].page.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].page.addr,
				emap[j].page.data, emap[j].page.data_t);
				msleep(emap[j].page.delay);
			if (rc < 0) {
				pr_err("%s: page write failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				emap[j].pageen.data, emap[j].pageen.data_t);
				msleep(emap[j].pageen.delay);
			if (rc < 0) {
				pr_err("%s: page enable failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].poll.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].poll.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_poll(
				&(e_ctrl->i2c_client), emap[j].poll.addr,
				emap[j].poll.data, emap[j].poll.data_t);
				msleep(emap[j].poll.delay);
			if (rc < 0) {
				pr_err("%s: poll failed\n", __func__);
				return rc;
			}
		}

		if (emap[j].mem.valid_size) {
		    rc = custom_hynix_define_otp_read(e_ctrl, &emap[j], memptr);
		    if(rc == 0)
		        e_ctrl->is_supported = 1;
		}
	}
	return rc;
}


//Jindian.guan@Camera.Driver, 2017/07/6, add for ov8865 eeprom
static int custom_ov8865_define_otp_read(struct msm_eeprom_ctrl_t *e_ctrl,
                struct msm_eeprom_memory_map_t *emap, uint8_t *memptr) {
	int m = 0;
	int k = 0;
	uint32_t addr = 0;
	int rc =0;

	CDBG("dmm debug %s come in\n",__func__);
	//initial  sensor
	for (m = 0; m < sizeof(ov8865_init_reg_array) / (sizeof(ov8865_init_reg_array[0])); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	             ov8865_init_reg_array[m].reg_addr, ov8865_init_reg_array[m].reg_data, MSM_CAMERA_I2C_BYTE_DATA);
        mdelay(ov8865_init_reg_array[m].delay);
	    if (rc < 0) {
	        pr_err("%s: ov8865 init  failed\n", __func__);
	        return rc;
	    }
	}

	// set to otp mode
	for (m = 0; m < sizeof(ov8865_init_otp_array) / sizeof(ov8865_init_otp_array[0]); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	              ov8865_init_otp_array[m].reg_addr, ov8865_init_otp_array[m].reg_data, MSM_CAMERA_I2C_BYTE_DATA);
	    mdelay(ov8865_init_otp_array[m].delay);
	    if (rc < 0) {
	        pr_err("%s: ov8865 to otp mode  failed\n", __func__);
	        return rc;
	    }
	}

    for (addr = emap->mem.addr, k = 0; k < (emap->mem.valid_size); addr++, k++) {
        rc |= e_ctrl->i2c_client.i2c_func_tbl->i2c_read_seq(&(e_ctrl->i2c_client),addr, memptr, 1);
        memptr++;  // must
        if (rc < 0) {
	        pr_err("%s: hi545 read failed\n", __func__);
	        return rc;
	    }
    }

	//set  to normol mode
	for (m = 0; m < sizeof(ov8865_otp_to_norm_mode_array)/sizeof(ov8865_otp_to_norm_mode_array[0]); m++){
	    rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(&(e_ctrl->i2c_client),
	            ov8865_otp_to_norm_mode_array[m].reg_addr, ov8865_otp_to_norm_mode_array[m].reg_data, 1);
        mdelay(ov8865_otp_to_norm_mode_array[m].delay);
	    if (rc < 0) {
	        pr_err("%s: to normal  failed\n", __func__);
	        return rc;
	    }
	}

	return rc;
}


static int read_eeprom_memory_ov8865(struct msm_eeprom_ctrl_t *e_ctrl,
			      struct msm_eeprom_memory_block_t *block)
{
	int rc = 0;
	int j;
	struct msm_eeprom_memory_map_t *emap = block->map;
	struct msm_eeprom_board_info *eb_info;
	uint8_t *memptr = block->mapdata;

	if (!e_ctrl) {
		pr_err("%s e_ctrl is NULL", __func__);
		return -EINVAL;
	}

	eb_info = e_ctrl->eboard_info;

	for (j = 0; j < block->num_map; j++) {
		if (emap[j].saddr.addr) {
			eb_info->i2c_slaveaddr = emap[j].saddr.addr;
			e_ctrl->i2c_client.cci_client->sid =
					eb_info->i2c_slaveaddr >> 1;
			pr_err("qcom,slave-addr = 0x%X\n",
				eb_info->i2c_slaveaddr);
		}

		if (emap[j].page.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].page.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].page.addr,
				emap[j].page.data, emap[j].page.data_t);
				msleep(emap[j].page.delay);
			if (rc < 0) {
				pr_err("%s: page write failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].pageen.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].pageen.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&(e_ctrl->i2c_client), emap[j].pageen.addr,
				emap[j].pageen.data, emap[j].pageen.data_t);
				msleep(emap[j].pageen.delay);
			if (rc < 0) {
				pr_err("%s: page enable failed\n", __func__);
				return rc;
			}
		}
		if (emap[j].poll.valid_size) {
			e_ctrl->i2c_client.addr_type = emap[j].poll.addr_t;
			rc = e_ctrl->i2c_client.i2c_func_tbl->i2c_poll(
				&(e_ctrl->i2c_client), emap[j].poll.addr,
				emap[j].poll.data, emap[j].poll.data_t);
				msleep(emap[j].poll.delay);
			if (rc < 0) {
				pr_err("%s: poll failed\n", __func__);
				return rc;
			}
		}

		if (emap[j].mem.valid_size) {
		    rc = custom_ov8865_define_otp_read(e_ctrl, &emap[j], memptr);
		    if(rc == 0)
		        e_ctrl->is_supported = 1;
		}
	}
	return rc;
}

#endif

#ifdef VENDOR_EDIT
//xiongxing add for compability of evt1 and evt2 of s5k4h8
uint8_t s5k4h8_module = 0;
#define CHECK_SUM_OFFSET        17
#define FLAG_OFFSET             16
#define SENSOR_ID_OFFSET        7
#define GROUP_SEP               28
#define OTP_PAGE                (1)//(4)
#define OTP_PAGE_SIZE           (64)
#define OTP_BASE                ((OTP_PAGE-1)*OTP_PAGE_SIZE)
static void msm_eeprom_s5k4h8_read_vendorInfo(struct msm_eeprom_ctrl_t * e_ctrl){
    
    uint8_t *buffer = (uint8_t *)e_ctrl->cal_data.mapdata;
    uint32_t size = e_ctrl->cal_data.num_data;
    int i = 0, j = 0;
    uint32_t sum = 0;

    if ( size < OTP_PAGE_SIZE*OTP_PAGE ){
        pr_err("%s size is %d \n", __func__, size);
        return;
    }
    
    for( i = 0; i < 2; i++ ){
        if( buffer[OTP_BASE + FLAG_OFFSET + i*GROUP_SEP] == 0x40 ){
            s5k4h8_module = buffer[OTP_BASE + SENSOR_ID_OFFSET + i*GROUP_SEP];
            sum = 0;
            for( j = 0; j < (0x0A0F - 0x0A04 + 1); j++ ){
                sum += buffer[OTP_BASE + j + i*GROUP_SEP];
                CDBG("%s checksum addr 0x%x \n", __func__, 0x0A04 + j + i*GROUP_SEP);
            }
            
            CDBG("%s checksum is 0x%x, should be 0x%x \n", __func__, sum%255, buffer[OTP_BASE + i*GROUP_SEP + CHECK_SUM_OFFSET]);

            if( sum%255 == buffer[OTP_BASE + i*GROUP_SEP + CHECK_SUM_OFFSET] ){
                break;
            }else{
                pr_err("%s checksum is not right\n", __func__);
                s5k3m2_module = 0;
            }
            
        }else{
            CDBG("%s group %d is invalid \n", __func__, i);
            s5k3m2_module = 0;
        }
    }

    if( i == 2){
        pr_err("%s no availble group \n",__func__);
    }
    
    CDBG("%s EVT1:0x34 EVT2:0x35  found id [0x%x] \n", __func__, s5k4h8_module);
    
}
#endif


/*xiongxing add for project 15399, imx170 and ov5695*/
#define IMX179OTP_BYPE_PER_GROUP 32
#define IMX179OTP_VALID_DATA 0x01
#define IMX179OTP_PAGE_SIZE 3
int  module_vendor_imx179;
static void msm_eeprom_imx179_read_vendorInfo(struct msm_eeprom_ctrl_t *e_ctrl){
    uint8_t *buffer = (uint8_t *)e_ctrl->cal_data.mapdata;
    //uint32_t size = e_ctrl->cal_data.num_data;
    int i = 0, j = 0;
    uint32_t sum = 0;
    uint16_t checksum = 0;

    for( i = 0; i < IMX179OTP_PAGE_SIZE; i++ ){
        if( ( buffer[ i*IMX179OTP_BYPE_PER_GROUP ] & 0x03) == IMX179OTP_VALID_DATA ){
            break;
        }
    }

    if( i == IMX179OTP_PAGE_SIZE ){
        pr_err("imx179 otp, invalid otp data\n");
        return;
    }

    for( j = 1; j <= 0x341C - 0x3405 + 1; j++ ){
        sum += buffer[ i*IMX179OTP_BYPE_PER_GROUP + j];
    }

    checksum = buffer[ i*IMX179OTP_BYPE_PER_GROUP + j ] + ( buffer[ i*IMX179OTP_BYPE_PER_GROUP + j + 1] << 8 );
    
    if( sum != checksum ){
        pr_err("checksum failed, should be 0x%x, now is 0x%x", checksum, sum);
        return;
    }
	
	module_vendor_imx179 = buffer[ i*IMX179OTP_BYPE_PER_GROUP + 1];
	
	return;
}

#define OV5695OTP_BYPE_PER_GROUP 7
#define OV5695OTP_PAGE_SIZE 2
int module_vendor_ov5695;

static void msm_eeprom_ov5695_read_vendorInfo(struct msm_eeprom_ctrl_t *e_ctrl){
    uint8_t *buffer = (uint8_t *)e_ctrl->cal_data.mapdata;
    //uint32_t size = e_ctrl->cal_data.num_data;
    int i = 0, index = 0;
	int flag = 0;
    uint32_t sum = 0;
	
	flag = buffer[0];
	
	if( ( flag & 0xC0 ) == 0x40 ){
		index = 0;
	}else if( ( flag & 0x30 ) == 0x10 ){
		index = 1;
	}else{
		pr_err("flag 0x%x, invalid data", flag);
		return;
	}
	
	for( i = 1; i < OV5695OTP_BYPE_PER_GROUP; i++ ){
		sum += buffer[ i + index*OV5695OTP_BYPE_PER_GROUP ];
	}

    /*
	if( sum%255 != buffer[ i + index*OV5695OTP_BYPE_PER_GROUP ] ){
		pr_err("ov5695 invalid otp data, checksum should be 0x%x, now is 0x%x\n",
				buffer[ i + index*OV5695OTP_BYPE_PER_GROUP ], sum%255);
		return;
	}
	*/
	
	module_vendor_ov5695 = buffer[ 1 + index*OV5695OTP_BYPE_PER_GROUP ];
}

#define OV5648OTP_BYPE_PER_GROUP 9
#define OV5648OTP_PAGE_SIZE 3
int module_vendor_ov5648;

static void msm_eeprom_ov5648_read_vendorInfo(struct msm_eeprom_ctrl_t *e_ctrl){
    uint8_t *buffer = (uint8_t *)e_ctrl->cal_data.mapdata;
    //uint32_t size = e_ctrl->cal_data.num_data;
    int i = 0;
	int flag = 0;
    int MID;
    int lens_id;

    buffer = buffer + 5; 
    for( i = 0; i < OV5648OTP_PAGE_SIZE; i++ ){
        flag = buffer[i*OV5648OTP_BYPE_PER_GROUP];
        if( ( flag & 0x80 ) == 0 ){
            MID = flag & 0x7F;
            lens_id = buffer[i*OV5648OTP_BYPE_PER_GROUP + 1];
            break;
        }
    }

    if( i == OV5648OTP_PAGE_SIZE ){
        pr_err("%s invalid otp flag\n", __func__);
        return;
    }
        
	module_vendor_ov5648 = lens_id | ( MID << 8 );
}


#define OV8858_OTP_PAGE_SIZE 16
uint8_t ov8858_module = 0;
static void msm_eeprom_ov8858_read_vendorInfo(struct msm_eeprom_ctrl_t *e_ctrl)
{
    uint8_t *buffer = (uint8_t *) e_ctrl->cal_data.mapdata;
    uint32_t size = e_ctrl->cal_data.num_data;
    uint8_t flag = 0;
    int i;
    
    CDBG("%s E\n", __func__);
    
    if( size < OV8858_OTP_PAGE_SIZE ){
        pr_err("%s size is %d \n", __func__, size);
        return;
    }
    
    flag = buffer[0];
    
    if( flag == 0){
        pr_err("%s err flag is zero\n",__func__);
        return;
    }

    for( i = 0; i < 3; i++ ){
        
        if( (flag & 0xC0) == 0x40 ){
            break;
        }
        
        flag <<= 2;
    }
    
    if( i == 3 ){
        pr_err("%s no valid group\n", __func__);
        return;
    }
    
    ov8858_module = buffer[ 1 + i*5 ];
    
    CDBG("%s module is 0x%x\n", __func__, ov8858_module);
}

char otp_3h7[20] = {0};
static void msm_eeprom_s5k3h7_read_OTPInfo(struct msm_eeprom_ctrl_t *e_ctrl){
    uint8_t *buffer = (uint8_t *) e_ctrl->cal_data.mapdata;
    uint32_t size = e_ctrl->cal_data.num_data;
    int grp_off[3] = {0,64,128};
    int i;
    uint16_t current_r, current_b, current_gr, current_gb;
    uint16_t current_g;
    int current_r_gain_3h7 = 0;
    int current_b_gain_3h7 = 0;

    if( size < 192 ){
        pr_err("data is not enough\n");
        snprintf(otp_3h7, 20, "s5k3h7");
        return;
    }
    for( i = 0; i < 3; i++ ){
        if( (buffer[grp_off[i]] & 0xC0 ) == 0x40 ){
			current_r  = buffer[grp_off[i]+22] + ((buffer[grp_off[i]+21] & 0x03) << 8);
			current_b  = buffer[grp_off[i]+23] + ((buffer[grp_off[i]+21] & 0x0C) << 6);
			current_gr = buffer[grp_off[i]+24] + ((buffer[grp_off[i]+21] & 0x30) << 4);
			current_gb = buffer[grp_off[i]+25] + ((buffer[grp_off[i]+21] & 0xC0) << 2);
            current_g  = (current_gr + current_gb) / 2;

			current_r_gain_3h7 = 512 * current_r /current_g;
			current_b_gain_3h7 = 512 * current_b /current_g;
            break;
        }
    }

    if( i == 3 ){
       pr_err("err no otp data\n");
       snprintf(otp_3h7, 20, "s5k3h7");
       return;
    }

    snprintf(otp_3h7, 20, "s5k3h7-%d-%d", current_r_gain_3h7, current_b_gain_3h7);
}

#endif

static int msm_eeprom_platform_probe(struct platform_device *pdev)
{
	int rc = 0;
	int j = 0;
	uint32_t temp;

	struct msm_camera_cci_client *cci_client = NULL;
	struct msm_eeprom_ctrl_t *e_ctrl = NULL;
	struct msm_eeprom_board_info *eb_info = NULL;
	struct device_node *of_node = pdev->dev.of_node;
	struct msm_camera_power_ctrl_t *power_info = NULL;

	CDBG("%s E\n", __func__);

	e_ctrl = kzalloc(sizeof(*e_ctrl), GFP_KERNEL);
	if (!e_ctrl) {
		pr_err("%s:%d kzalloc failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	e_ctrl->eeprom_v4l2_subdev_ops = &msm_eeprom_subdev_ops;
	e_ctrl->eeprom_mutex = &msm_eeprom_mutex;

	e_ctrl->is_supported = 0;
	if (!of_node) {
		pr_err("%s dev.of_node NULL\n", __func__);
		return -EINVAL;
	}

	rc = of_property_read_u32(of_node, "cell-index",
		&pdev->id);
	CDBG("cell-index %d, rc %d\n", pdev->id, rc);
	if (rc < 0) {
		pr_err("failed rc %d\n", rc);
		return rc;
	}
	e_ctrl->subdev_id = pdev->id;

	rc = of_property_read_u32(of_node, "qcom,cci-master",
		&e_ctrl->cci_master);
	CDBG("qcom,cci-master %d, rc %d\n", e_ctrl->cci_master, rc);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		return rc;
	}
	rc = of_property_read_u32(of_node, "qcom,slave-addr",
		&temp);
	if (rc < 0) {
		pr_err("%s failed rc %d\n", __func__, rc);
		return rc;
	}

	/* Set platform device handle */
	e_ctrl->pdev = pdev;
	/* Set device type as platform device */
	e_ctrl->eeprom_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	e_ctrl->i2c_client.i2c_func_tbl = &msm_eeprom_cci_func_tbl;
	e_ctrl->i2c_client.cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!e_ctrl->i2c_client.cci_client) {
		pr_err("%s failed no memory\n", __func__);
		return -ENOMEM;
	}

	e_ctrl->eboard_info = kzalloc(sizeof(
		struct msm_eeprom_board_info), GFP_KERNEL);
	if (!e_ctrl->eboard_info) {
		pr_err("%s failed line %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto cciclient_free;
	}
	eb_info = e_ctrl->eboard_info;
	power_info = &eb_info->power_info;
	eb_info->i2c_slaveaddr = temp;

	power_info->clk_info = cam_8974_clk_info;
	power_info->clk_info_size = ARRAY_SIZE(cam_8974_clk_info);
	power_info->dev = &pdev->dev;


	rc = of_property_read_u32(of_node, "qcom,i2c-freq-mode",
		&eb_info->i2c_freq_mode);
	if (rc < 0 || (eb_info->i2c_freq_mode >= I2C_MAX_MODES)) {
		eb_info->i2c_freq_mode = I2C_STANDARD_MODE;
		CDBG("%s Default I2C standard speed mode.\n", __func__);
	}

	pr_err("qcom,slave-addr = 0x%X\n", eb_info->i2c_slaveaddr);
	cci_client = e_ctrl->i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = e_ctrl->cci_master;
	cci_client->sid = eb_info->i2c_slaveaddr >> 1;
	cci_client->retries = 3;
	cci_client->id_map = 0;
#ifdef VENDOR_EDIT
	/*hufeng add 2014-11-03 to make eeprom use i2c_fast_mode*/
	cci_client->i2c_freq_mode = 1;
#endif
	rc = of_property_read_string(of_node, "qcom,eeprom-name",
		&eb_info->eeprom_name);
	CDBG("%s qcom,eeprom-name %s, rc %d\n", __func__,
		eb_info->eeprom_name, rc);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto board_free;
	}

	rc = msm_eeprom_cmm_dts(e_ctrl->eboard_info, of_node);
	if (rc < 0)
		CDBG("%s MM data miss:%d\n", __func__, __LINE__);

	rc = msm_eeprom_get_dt_data(e_ctrl);
	if (rc)
		goto board_free;

	rc = msm_eeprom_parse_memory_map(of_node, &e_ctrl->cal_data);
	if (rc < 0)
		goto board_free;

	rc = msm_camera_power_up(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
	if (rc) {
		pr_err("failed rc %d\n", rc);
		goto memdata_free;
	}

#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/03/02, modify for 15013 eeprom*/
    if (strcmp(eb_info->eeprom_name, "sunny_f13s01l") == 0) {
        msm_eeprom_s5k3m2_read_vendorInfo(e_ctrl);
        msm_eeprom_proc_init();
    }
#endif
#ifdef VENDOR_EDIT
/* lanhe add for 15005 3h7 eeprom*/
    if (strcmp(eb_info->eeprom_name, "truly_s5k3h7") == 0) 
		rc = read_eeprom_memory_3h7(e_ctrl, &e_ctrl->cal_data);
    else if(strcmp(eb_info->eeprom_name, "hynix_hi545") == 0)
        rc = read_eeprom_memory_hi545(e_ctrl, &e_ctrl->cal_data);
    else if(strcmp(eb_info->eeprom_name, "omnivision_8865") == 0)
        rc = read_eeprom_memory_ov8865(e_ctrl, &e_ctrl->cal_data);
	else
#endif
	rc = read_eeprom_memory(e_ctrl, &e_ctrl->cal_data);
	if (rc < 0) {
		pr_err("%s read_eeprom_memory failed\n", __func__);
		goto power_down;
	}
	for (j = 0; j < e_ctrl->cal_data.num_data; j++)
		CDBG("memory_data[%d] = 0x%X\n", j,
			e_ctrl->cal_data.mapdata[j]);

#ifdef VENDOR_EDIT
    /*xiongxing add for 15095 s5k4h8*/
    if ( strcmp( eb_info->eeprom_name, "qtech_s5k4h8") == 0){
        msm_eeprom_s5k4h8_read_vendorInfo(e_ctrl);
    }
#endif

#ifdef VENDOR_EDIT
    if ( strcmp( eb_info->eeprom_name, "truly_ov8858" ) == 0 )
    {
        msm_eeprom_ov8858_read_vendorInfo(e_ctrl);
    }
#endif

#ifdef VENDOR_EDIT
/*xiongxing add for 15399, imx179, ov5695 and ov5648*/
    if( strcmp( eb_info->eeprom_name, "sony_imx179" ) == 0 ){
        msm_eeprom_imx179_read_vendorInfo(e_ctrl);
    }

    if( strcmp( eb_info->eeprom_name, "omnivision_5695" ) == 0 ){
        msm_eeprom_ov5695_read_vendorInfo(e_ctrl);
    }
    
    if( strcmp( eb_info->eeprom_name, "omnivision_5648" ) == 0 ){
        msm_eeprom_ov5648_read_vendorInfo(e_ctrl);
    }

    if( strcmp( eb_info->eeprom_name, "truly_s5k3h7_new" ) == 0 ){
        msm_eeprom_s5k3h7_read_OTPInfo(e_ctrl);
    }
#endif

	e_ctrl->is_supported |= msm_eeprom_match_crc(&e_ctrl->cal_data);

	rc = msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
	if (rc) {
		pr_err("failed rc %d\n", rc);
		goto memdata_free;
	}
	v4l2_subdev_init(&e_ctrl->msm_sd.sd,
		e_ctrl->eeprom_v4l2_subdev_ops);
	v4l2_set_subdevdata(&e_ctrl->msm_sd.sd, e_ctrl);
	platform_set_drvdata(pdev, &e_ctrl->msm_sd.sd);
	e_ctrl->msm_sd.sd.internal_ops = &msm_eeprom_internal_ops;
	e_ctrl->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(e_ctrl->msm_sd.sd.name,
		ARRAY_SIZE(e_ctrl->msm_sd.sd.name), "msm_eeprom");
	media_entity_init(&e_ctrl->msm_sd.sd.entity, 0, NULL, 0);
	e_ctrl->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	e_ctrl->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_EEPROM;
	msm_sd_register(&e_ctrl->msm_sd);

#ifdef CONFIG_COMPAT
	msm_eeprom_v4l2_subdev_fops = v4l2_subdev_fops;
	msm_eeprom_v4l2_subdev_fops.compat_ioctl32 =
		msm_eeprom_subdev_fops_ioctl32;
	e_ctrl->msm_sd.sd.devnode->fops = &msm_eeprom_v4l2_subdev_fops;
#endif

	e_ctrl->is_supported = (e_ctrl->is_supported << 1) | 1;
	CDBG("%s X\n", __func__);
	return rc;

power_down:
	msm_camera_power_down(power_info, e_ctrl->eeprom_device_type,
		&e_ctrl->i2c_client);
memdata_free:
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
board_free:
	kfree(e_ctrl->eboard_info);
cciclient_free:
	kfree(e_ctrl->i2c_client.cci_client);
	kfree(e_ctrl);
	return rc;
}

static int msm_eeprom_platform_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct msm_eeprom_ctrl_t  *e_ctrl;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	e_ctrl = (struct msm_eeprom_ctrl_t *)v4l2_get_subdevdata(sd);
	if (!e_ctrl) {
		pr_err("%s: eeprom device is NULL\n", __func__);
		return 0;
	}

	kfree(e_ctrl->i2c_client.cci_client);
	kfree(e_ctrl->cal_data.mapdata);
	kfree(e_ctrl->cal_data.map);
	if (e_ctrl->eboard_info) {
		kfree(e_ctrl->eboard_info->power_info.gpio_conf);
		kfree(e_ctrl->eboard_info);
	}
	kfree(e_ctrl);
	return 0;
}

static const struct of_device_id msm_eeprom_dt_match[] = {
	{ .compatible = "qcom,eeprom" },
	{ }
};

MODULE_DEVICE_TABLE(of, msm_eeprom_dt_match);

static struct platform_driver msm_eeprom_platform_driver = {
	.driver = {
		.name = "qcom,eeprom",
		.owner = THIS_MODULE,
		.of_match_table = msm_eeprom_dt_match,
	},
	.remove = msm_eeprom_platform_remove,
};

static const struct i2c_device_id msm_eeprom_i2c_id[] = {
	{ "msm_eeprom", (kernel_ulong_t)NULL},
	{ }
};

static struct i2c_driver msm_eeprom_i2c_driver = {
	.id_table = msm_eeprom_i2c_id,
	.probe  = msm_eeprom_i2c_probe,
	.remove = msm_eeprom_i2c_remove,
	.driver = {
		.name = "msm_eeprom",
	},
};

static struct spi_driver msm_eeprom_spi_driver = {
	.driver = {
		.name = "qcom_eeprom",
		.owner = THIS_MODULE,
		.of_match_table = msm_eeprom_dt_match,
	},
	.probe = msm_eeprom_spi_probe,
	.remove = msm_eeprom_spi_remove,
};

static int __init msm_eeprom_init_module(void)
{
	int rc = 0;
	CDBG("%s E\n", __func__);
	rc = platform_driver_probe(&msm_eeprom_platform_driver,
		msm_eeprom_platform_probe);
	CDBG("%s:%d platform rc %d\n", __func__, __LINE__, rc);
#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/03/02, modify for eeprom*/
    return rc;
#endif
	rc = spi_register_driver(&msm_eeprom_spi_driver);
	CDBG("%s:%d spi rc %d\n", __func__, __LINE__, rc);
	return i2c_add_driver(&msm_eeprom_i2c_driver);
}

static void __exit msm_eeprom_exit_module(void)
{
	platform_driver_unregister(&msm_eeprom_platform_driver);
#ifdef VENDOR_EDIT
/*zhengrong.zhang, 2015/03/02, modify for eeprom*/
    return;
#endif
	spi_unregister_driver(&msm_eeprom_spi_driver);
	i2c_del_driver(&msm_eeprom_i2c_driver);
}

module_init(msm_eeprom_init_module);
module_exit(msm_eeprom_exit_module);
MODULE_DESCRIPTION("MSM EEPROM driver");
MODULE_LICENSE("GPL v2");
