/**
 * Copyright 2008-2013 OPPO Mobile Comm Corp., Ltd, All rights reserved.
 * VENDOR_EDIT:
 * FileName:devinfo.c
 * ModuleName:devinfo
 * Author: wangjc
 * Create Date: 2013-10-23
 * Description:add interface to get device information.
 * History:
   <version >  <time>  <author>  <desc>
   1.0		2013-10-23	wangjc	init
   2.0      2015-04-13  hantong modify as platform device  to support diffrent configure in dts
*/

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <soc/qcom/smem.h>
#include <soc/oppo/device_info.h>
#include <soc/oppo/oppo_project.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include "../../../fs/proc/internal.h"

#define DEVINFO_NAME "devinfo"

static struct of_device_id devinfo_id[] = {
	{.compatible = "oppo-devinfo",},
	{},
};

struct devinfo_data { 
	struct platform_device *devinfo;
	int hw_id1_gpio;
	int hw_id2_gpio;
	int hw_id3_gpio;
	int sub_hw_id1;
	int sub_hw_id2;
};

static struct proc_dir_entry *parent = NULL;

static void *device_seq_start(struct seq_file *s, loff_t *pos)
{    
	static unsigned long counter = 0;    
	if ( *pos == 0 ) {        
		return &counter;   
	}else{
		*pos = 0; 
		return NULL;
	}
}

static void *device_seq_next(struct seq_file *s, void *v, loff_t *pos)
{  
	return NULL;
}

static void device_seq_stop(struct seq_file *s, void *v)
{
	return;
}

static int device_seq_show(struct seq_file *s, void *v)
{
	struct proc_dir_entry *pde = s->private;
	struct manufacture_info *info = pde->data;
	if(info)
	  seq_printf(s, "Device version:\t\t%s\nDevice manufacture:\t\t%s\n",
		     info->version,	info->manufacture);	
	return 0;
}

static struct seq_operations device_seq_ops = {
	.start = device_seq_start,
	.next = device_seq_next,
	.stop = device_seq_stop,
	.show = device_seq_show
};

static int device_proc_open(struct inode *inode,struct file *file)
{
	int ret = seq_open(file,&device_seq_ops);
	pr_err("caven %s is called\n",__func__);
	
	if(!ret){
		struct seq_file *sf = file->private_data;
		sf->private = PDE(inode);
	}
	
	return ret;
}
static const struct file_operations device_node_fops = {
	.read =  seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.open = device_proc_open,
	.owner = THIS_MODULE,
};

int register_device_proc(char *name, char *version, char *manufacture)
{
	struct proc_dir_entry *d_entry;
	struct manufacture_info *info;

	if(!parent) {
		parent =  proc_mkdir ("devinfo", NULL);
		if(!parent) {
			pr_err("can't create devinfo proc\n");
			return -ENOENT;
		}
	}

	info = kzalloc(sizeof *info, GFP_KERNEL);
	info->version = version;
	info->manufacture = manufacture;
	d_entry = proc_create_data (name, S_IRUGO, parent, &device_node_fops, info);
	if(!d_entry) {
		pr_err("create %s proc failed.\n", name);
		kfree(info);
		return -ENOENT;
	}
	return 0;
}

static void dram_type_add(void)
{
	struct manufacture_info dram_info;
	int *p = NULL;
	#if 0
	p  = (int *)smem_alloc2(SMEM_DRAM_TYPE,4);
	#else
	p  = (int *)smem_alloc(SMEM_DRAM_TYPE,4,0,0);
	#endif

	if(p)
	{
		switch(*p){
			case DRAM_TYPE0:
				dram_info.version = "EDB8132B3PB-1D-F FBGA";
				dram_info.manufacture = "ELPIDA";
				break;
			case DRAM_TYPE1:
				dram_info.version = "EDB8132B3PB-1D-F FBGA";
				dram_info.manufacture = "ELPIDA";
				break;
			case DRAM_TYPE2:
				dram_info.version = "EDF8132A3PF-GD-F FBGA";
				dram_info.manufacture = "ELPIDA";
				break;
			case DRAM_TYPE3:
				dram_info.version = "K4E8E304ED-AGCC FBGA";
				dram_info.manufacture = "SAMSUNG";
				break;
			default:
				dram_info.version = "unknown";
				dram_info.manufacture = "unknown";
		}

	}else{
		dram_info.version = "unknown";
		dram_info.manufacture = "unknown";

	}

	register_device_proc("ddr", dram_info.version, dram_info.manufacture);
}


static int get_hw_opreator_version(struct devinfo_data *devinfo_data)
{
	int hw_operator_name = 0;
	int ret;
	int id1 = -1;
	int id2 = -1;
	int id3 = -1;
	struct device_node *np;	
	np = devinfo_data->devinfo->dev.of_node;
	if(!devinfo_data){
		pr_err("devinfo_data is NULL\n");
		return 0;
	}
	devinfo_data->hw_id1_gpio = of_get_named_gpio(np, "Hw,operator-gpio1", 0);
	if(devinfo_data->hw_id1_gpio < 0 ) {
		pr_err("devinfo_data->hw_id1_gpio not specified\n");
	}
	devinfo_data->hw_id2_gpio = of_get_named_gpio(np, "Hw,operator-gpio2", 0);
	if(devinfo_data->hw_id2_gpio < 0 ) {
		pr_err("devinfo_data->hw_id2_gpio not specified\n");
	}	
	devinfo_data->hw_id3_gpio = of_get_named_gpio(np, "Hw,operator-gpio3", 0);
	if(devinfo_data->hw_id3_gpio < 0 ) {
		pr_err("devinfo_data->hw_id3_gpio not specified\n");
	}
	/***Tong.han@Bsp.Group.Tp Added for Operator_Pcb detection***/
	if(devinfo_data->hw_id1_gpio >= 0 ) {
		ret = gpio_request(devinfo_data->hw_id1_gpio,"HW_ID1");
		if(ret){
			pr_err("unable to request gpio [%d]\n",devinfo_data->hw_id1_gpio);
		}else{
			id1=gpio_get_value(devinfo_data->hw_id1_gpio);	
		}
 	}
 	if(devinfo_data->hw_id2_gpio >= 0 ) {
		ret = gpio_request(devinfo_data->hw_id2_gpio,"HW_ID2");
		if(ret){
			pr_err("unable to request gpio [%d]\n",devinfo_data->hw_id2_gpio);
		}else{
			id2=gpio_get_value(devinfo_data->hw_id2_gpio);	
		}
 	}
 	if(devinfo_data->hw_id3_gpio >= 0 ) {
		ret = gpio_request(devinfo_data->hw_id3_gpio,"HW_ID2");
		if(ret){
			pr_err("unable to request gpio [%d]\n",devinfo_data->hw_id3_gpio);
		}else{
			id3=gpio_get_value(devinfo_data->hw_id3_gpio);	
		}
 	}
	if(is_project(OPPO_15018)) {
		if(( id1==0 )&&( id2==0 ))
			hw_operator_name = OPERATOR_CHINA_TELECOM;
		else if(( id1==0 )&&( id2==1 ))
			hw_operator_name = OPERATOR_ALL_CHINA_CARRIER;
		else
			hw_operator_name = OPERATOR_UNKOWN;
	}
	if(is_project(OPPO_15009)){		
		if(( id1==0 )&&( id2==0 )&&( id3==0 ))
			hw_operator_name = OPERATOR_CHINA_MOBILE;
		else if(( id1==1 )&&( id2==0 )&&( id3==0 ))
			hw_operator_name = OPERATOR_CHINA_UNICOM ;
		else if(( id1==0 )&&( id2==1 )&&( id3==0 ))
			hw_operator_name = OPERATOR_CHINA_TELECOM;
		else if(( id1==0 )&&( id2==1 )&&( id3==0 ))
			hw_operator_name = OPERATOR_UNKOWN;
	}
	pr_err("hw_operator_name [%d]\n",hw_operator_name);
	return hw_operator_name;
}
static void sub_mainboard_verify(struct devinfo_data *devinfo_data)
{
	int ret;
	int id1 = -1;
	int id2 = -1;
	static char temp_manufacture_sub[12];
	struct device_node *np;	
	struct manufacture_info mainboard_info;
	if(!devinfo_data){
		pr_err("devinfo_data is NULL\n");
		return;
	}
	np = devinfo_data->devinfo->dev.of_node;
	devinfo_data->sub_hw_id1 = of_get_named_gpio(np, "Hw,sub_hwid_1", 0);
	if(devinfo_data->sub_hw_id1 < 0 ) {
		pr_err("devinfo_data->sub_hw_id1 not specified\n");
	}
	devinfo_data->sub_hw_id2 = of_get_named_gpio(np, "Hw,sub_hwid_2", 0);
	if(devinfo_data->sub_hw_id2 < 0 ) {
		pr_err("devinfo_data->sub_hw_id2 not specified\n");
	}	
	
	if(devinfo_data->sub_hw_id1 >= 0 ) {
		ret = gpio_request(devinfo_data->sub_hw_id1,"SUB_HW_ID1");
		if(ret){
			pr_err("unable to request gpio [%d]\n",devinfo_data->sub_hw_id1);
		}else{
			id1=gpio_get_value(devinfo_data->sub_hw_id1);	
		}
 	}
 	if(devinfo_data->sub_hw_id2 >= 0 ) {
		ret = gpio_request(devinfo_data->sub_hw_id2,"SUB_HW_ID2");
		if(ret){
			pr_err("unable to request gpio [%d]\n",devinfo_data->sub_hw_id2);
		}else{
			id2=gpio_get_value(devinfo_data->sub_hw_id2);	
		}
 	}
	mainboard_info.manufacture = temp_manufacture_sub;
	mainboard_info.version ="Qcom";
	switch(get_project()) {
		case OPPO_15018:
		{
			#ifndef IS_PROJECT_15119
			if(( id1==0 )&&( id2==0 )) {
				sprintf(mainboard_info.manufacture,"%d-%d",get_project(),get_Operator_Version());	
			}
			#ifdef VENDOR_EDIT
			//xiaohua.tian@EXP.Driver, add for GPIO setting for sub_mainboard, 2015-05-13
			else if(( id1==1 )&&( id2==0 )) {
				sprintf(mainboard_info.manufacture,"15089-%d",get_Operator_Version());	
			}
			#endif
			else {
				mainboard_info.manufacture = "UNSPECIFIED";
			}
			#else
			if(( id1==0 )&&( id2==1 )) {
				sprintf(mainboard_info.manufacture,"%d-%d",get_project(),get_Operator_Version());	
			} else {
				mainboard_info.manufacture = "UNSPECIFIED";
			}
			#endif
			break;
		}
		case OPPO_15009:
		{	
			if((id1 == 0)&&(id2 == 0))
				sprintf(mainboard_info.manufacture,"15009-%d",OPERATOR_CHINA_MOBILE);
			#ifdef VENDOR_EDIT
			//lile@EXP.BasicDrv.Audio, 2015-06-05, add for EXP setting of the sub_mainboard
			else if((id1 == 1)&&(id2 == 1) && 100 != get_Operator_Version())
				sprintf(mainboard_info.manufacture,"15009-%d",OPERATOR_CHINA_TELECOM);
			else if((id1 == 1)&&(id2 == 1) && 100 == get_Operator_Version())
				sprintf(mainboard_info.manufacture,"15070-%d",get_Operator_Version());
			else if((id1 == 0)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15069-%d",get_Operator_Version());
			#else
			else if((id1 == 1)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15009-%d",OPERATOR_CHINA_MOBILE);
			else if((id1 == 1)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15009-%d",OPERATOR_FOREIGN);
			#endif
			else
				mainboard_info.manufacture = "UNSPECIFIED";
			break;
		}
		case OPPO_15022:
		{	
			if((id1 == 0)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"%d-%d",get_project(),get_Operator_Version());	
			else
				mainboard_info.manufacture = "UNSPECIFIED";
			break;
		}
		#ifdef VENDOR_EDIT
		//xiaohua.tian@EXP.Driver, add for GPIO setting for sub_mainboard, 2015-05-13
		case OPPO_15011:
		{
			if(( id1==0 )) {
				sprintf(mainboard_info.manufacture,"15011-%d",get_Operator_Version());	
			}
			 else {
				mainboard_info.manufacture = "UNSPECIFIED";
			}
			break;
		}
		//lile@EXP.BasicDrv, 2015-11-16, add for GPIO setting for sub_mainboard of 15099
		case OPPO_15029:
		{
			if((id1 == 0)&&(id2 == 1) && OPERATOR_FOREIGN_TAIWAN == get_Operator_Version())
			{
				sprintf(mainboard_info.manufacture,"15029-%d",OPERATOR_FOREIGN_TAIWAN);	
			}
			else if((id1 == 1)&&(id2 == 0) && OPERATOR_FOREIGN_TAIWAN != get_Operator_Version())
			{
				sprintf(mainboard_info.manufacture,"15029-%d",get_Operator_Version());
			}
			else
			{
				mainboard_info.manufacture = "UNSPECIFIED";
			}
			break;
		}
		#endif
		case OPPO_15035:
		{	
			if((id1 == 1)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15035-%d",OPERATOR_CHINA_MOBILE);
			#ifdef VENDOR_EDIT
			//lile@EXP.BasicDrv.CDT, add for 15095 sub board
			else if ((id1 == 0)&&(id2 == 0))
			{
				if(get_Operator_Version() != 105)
					sprintf(mainboard_info.manufacture,"15035-%d",OPERATOR_ALL_CHINA_CARRIER);
				else
					sprintf(mainboard_info.manufacture,"15035-%d",get_Operator_Version());
			}
			else if((id1 == 0)&&(id2 == 1))
			{
				if((get_Operator_Version() >= 100) && (get_Operator_Version() <= 103)) //lile@EXP.BasicDrv.CDT, 2015-10-24, 15095 mainboard with 15301 sub board fail
					sprintf(mainboard_info.manufacture,"15035-%d",get_Operator_Version());
				else
					mainboard_info.manufacture = "UNSPECIFIED";
			}
			#else
			else if ((id1 == 0)&&(id2 == 0))
				sprintf(mainboard_info.manufacture,"15035-%d",OPERATOR_ALL_CHINA_CARRIER);
			#endif
			else
				mainboard_info.manufacture = "UNSPECIFIED";
			break;
		}
//huqiao@EXP.BasicDrv.CDT 2016-01-05 add for 15399 operatorName
		case OPPO_15399:
		{	

			if(get_Modem_Version()==0)
			{
				/* Le.Li@EXP.BSP.Kernel.Driver, 2017/05/06, Add for MT6763 N.*/
				if((OPERATOR_FOREIGN_TAIWAN == get_Operator_Version())||(OPERATOR_FOREIGN_TAIWAN_NEW == get_Operator_Version()))/*Chaoying.Chen@Prd6.BSP.Kernel.Driver,2017/03/14   Add new taiwan board for 15399*/
					sprintf(mainboard_info.manufacture,"15399-%d",get_Operator_Version());
				else
					mainboard_info.manufacture = "UNSPECIFIED";

			}
			else if(get_Modem_Version()==1)
			{
				if((get_Operator_Version() == OPERATOR_FOREIGN_TAIWAN)||(get_Operator_Version() == OPERATOR_FOREIGN_TAIWAN_NEW)) /*Chaoying.Chen@Prd6.BSP.Kernel.Driver,2017/03/14   Add new taiwan board for 15399*/
					mainboard_info.manufacture = "UNSPECIFIED";
				else
					sprintf(mainboard_info.manufacture,"15399-%d",get_Operator_Version());
			}
			
			else
					mainboard_info.manufacture = "UNSPECIFIED";
			break;
		}
		case OPPO_15109:
		{	
			if((id1 == 1)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15109-%d",OPERATOR_CHINA_MOBILE);
			else if ((id1 == 0)&&(id2 == 1))
				sprintf(mainboard_info.manufacture,"15109-%d",OPERATOR_ALL_CHINA_CARRIER);
			else if((id1 == 1)&&(id2 == 0)) //lile@EXP.BasicDrv.CDT, 2015-11-06, add for exp 15309 sub board test
				sprintf(mainboard_info.manufacture,"15109-%d", get_Operator_Version());
			else
				mainboard_info.manufacture = "UNSPECIFIED";
			break;
		}
		default:
		{
			sprintf(mainboard_info.manufacture,"%d-%d",get_project(),get_Operator_Version());
			break;
		}
	}
	register_device_proc("sub_mainboard", mainboard_info.version, mainboard_info.manufacture);
}

static void mainboard_verify(struct devinfo_data *devinfo_data)
{
	struct manufacture_info mainboard_info;
	int hw_opreator_version = 0;
	static char temp_manufacture[12];
	if(!devinfo_data){
		pr_err("devinfo_data is NULL\n");
		return;
	}
	/***Tong.han@Bsp.Group.Tp Added for Operator_Pcb detection***/
	hw_opreator_version = get_hw_opreator_version(devinfo_data);
	/*end of Add*/	 
	mainboard_info.manufacture = temp_manufacture;
	switch(get_PCB_Version()) {
		case HW_VERSION__10:		
			mainboard_info.version ="10";
			sprintf(mainboard_info.manufacture,"%d-SA",hw_opreator_version);
	//		mainboard_info.manufacture = "SA(SB)";
			break;
		case HW_VERSION__11:	
			mainboard_info.version = "11";
			sprintf(mainboard_info.manufacture,"%d-SB",hw_opreator_version);
	//		mainboard_info.manufacture = "SC";
			break;
		case HW_VERSION__12:
			mainboard_info.version = "12";
			sprintf(mainboard_info.manufacture,"%d-SC",hw_opreator_version);
	//		mainboard_info.manufacture = "SD";
			break;
		case HW_VERSION__13:
			mainboard_info.version = "13";
			sprintf(mainboard_info.manufacture,"%d-SD",hw_opreator_version);
    //        mainboard_info.manufacture = "SE";
			break;
		case HW_VERSION__14:
			mainboard_info.version = "14";
			sprintf(mainboard_info.manufacture,"%d-SE",hw_opreator_version);
	//		mainboard_info.manufacture = "SF";
			break;
		case HW_VERSION__15:
			mainboard_info.version = "15";
			sprintf(mainboard_info.manufacture,"%d-(T3-T4)",hw_opreator_version);
	//		mainboard_info.manufacture = "T3-T4";
			break;
		default:	
			mainboard_info.version = "UNKOWN";
			sprintf(mainboard_info.manufacture,"%d-UNKOWN",hw_opreator_version);
	//		mainboard_info.manufacture = "UNKOWN";
	}	
	register_device_proc("mainboard", mainboard_info.version, mainboard_info.manufacture);
}

#ifdef VENDOR_EDIT//Fanhong.Kong@ProDrv.CHG,modified 2014.4.13 for 14027
static void pa_verify(void)
{
	struct manufacture_info pa_info;

	switch(get_Modem_Version()) {
		case 0:		
			pa_info.version = "0";
			pa_info.manufacture = "RFMD PA";
			break;
		case 1:	
			pa_info.version = "1";
			pa_info.manufacture = "SKY PA";
			break;
		case 3:
			pa_info.version = "3";
			pa_info.manufacture = "AVAGO PA";
			break;
		default:	
			pa_info.version = "UNKOWN";
			pa_info.manufacture = "UNKOWN";
	}
			
	register_device_proc("pa", pa_info.version, pa_info.manufacture);

}
#endif /*VENDOR_EDIT*/	

static int devinfo_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct devinfo_data *devinfo_data = NULL;
	devinfo_data = kzalloc(sizeof(struct devinfo_data), GFP_KERNEL);
	if( devinfo_data == NULL ) {
		pr_err("devinfo_data kzalloc failed\n");
		ret = -ENOMEM;
		return ret;
	}
	 
	/*parse_dts*/
	devinfo_data->devinfo = pdev; 
	/*end of parse_dts*/
	
	if(!parent) {
		parent =  proc_mkdir ("devinfo", NULL);
		if(!parent) {
			pr_err("can't create devinfo proc\n");
			ret = -ENOENT;
		}
	}
	
	/*Add devinfo for some devices*/
	pa_verify();
	dram_type_add();
	mainboard_verify(devinfo_data);
	sub_mainboard_verify(devinfo_data);
	/*end of Adding devinfo for some devices*/
	return ret;
}

static int devinfo_remove(struct platform_device *dev)
{
	remove_proc_entry(DEVINFO_NAME, NULL);
	return 0;
}

static struct platform_driver devinfo_platform_driver = {
	.probe = devinfo_probe,
	.remove = devinfo_remove,
	.driver = {
		.name = DEVINFO_NAME,
		.of_match_table = devinfo_id,
	},
};

module_platform_driver(devinfo_platform_driver);

MODULE_DESCRIPTION("OPPO device info");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Wangjc <wjc@oppo.com>");
