/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
*/


/*****************************************************************************
 * Header Files
*****************************************************************************/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/page.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
/*#include <mach/irqs.h>*/
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/miscdevice.h>
#include <linux/wakelock.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h>
#ifdef SIGTEST
#include <asm/siginfo.h>
#endif
#include <mach/gpio_const.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
/*#include <mach/mt_clkmgr.h>*/
#include "scp_helper.h"
#include "scp_ipi.h"
#include "scp_excep.h"
#include "vow.h"
#include "vow_hw.h"
#include "vow_assert.h"

#include "audio_task_manager.h"
#include "audio_ipi_queue.h"



/*****************************************************************************
 * Configurations
*****************************************************************************/
#define DEBUG_VOWDRV 1
#define VOW_PRE_LEARN_MODE 1

#define VOW_TAG "VOW:"

#if DEBUG_VOWDRV
#define PRINTK_VOWDRV(format, args...) printk(format, ##args)
#else
#define PRINTK_VOWDRV(format, args...)
#endif


/*****************************************************************************
 * Variable Definition
****************************************************************************/
static const char vowdrv_name[] = "VOW_driver_device";
static unsigned int VowDrv_Wait_Queue_flag;
static unsigned int VoiceData_Wait_Queue_flag;
static DECLARE_WAIT_QUEUE_HEAD(VowDrv_Wait_Queue);
static DECLARE_WAIT_QUEUE_HEAD(VoiceData_Wait_Queue);
static DEFINE_SPINLOCK(vowdrv_lock);
static struct wake_lock VOW_suspend_lock;
static int init_flag = -1;
struct platform_device *VowPltFmDev;

/*****************************************************************************
* Function  Declaration
****************************************************************************/
static void vow_ipi_reg_ok(short id);
static void vow_service_getVoiceData(void);
static bool vow_IPICmd_Send(audio_ipi_msg_data_t data_type,
			    audio_ipi_msg_ack_t ack_type, uint16_t msg_id, uint32_t param1,
			    uint32_t param2, char *payload);
static void vow_IPICmd_Received(ipi_msg_t *ipi_msg);
static bool vow_IPICmd_ReceiveAck(ipi_msg_t *ipi_msg);
static void vow_Task_Unloaded_Handling(void);
static bool VowDrv_SetFlag(VOW_FLAG_TYPE type, bool set);
static int VowDrv_GetHWStatus(void);

/*****************************************************************************
* VOW SERVICES
*****************************************************************************/

static struct
{
	void                 *vow_init_model;
	void                 *vow_noise_model;
	void                 *vow_fir_model;
	VOW_SPEAKER_MODEL_T  vow_speaker_model[MAX_VOW_SPEAKER_MODEL];
	unsigned long        vow_info_apuser[MAX_VOW_INFO_LEN];
	unsigned int         vow_info_dsp[MAX_VOW_INFO_LEN];
	unsigned long        voicedata_user_addr;
	unsigned long        voicedata_user_size;
	short                *voicedata_kernel_ptr;
	void                 *voicddata_scp_ptr;
	dma_addr_t           voicedata_scp_addr;
	short                voicedata_idx;
	bool                 scp_command_flag;
	bool                 recording_flag;
	int                  scp_command_id;
	VOW_EINT_STATUS      eint_status;
	VOW_PWR_STATUS       pwr_status;
	int                  send_ipi_count;
	bool                 suspend_lock;
	bool                 firstRead;
	unsigned long        voicedata_user_return_size_addr;
	unsigned int         voice_buf_offset;
	unsigned int         voice_length;
	unsigned int         transfer_length;
	struct device_node   *node;
	struct pinctrl       *pinctrl;
	struct pinctrl_state *pins_eint_on;
	struct pinctrl_state *pins_eint_off;
	bool                 bypass_enter_phase3;
	unsigned int         enter_phase3_cnt;
	unsigned int         force_phase_stage;
	bool                 swip_log_enable;
} vowserv;

static struct device dev = {
	.init_name = "vowdmadev",
	.coherent_dma_mask = ~0,             /* dma_alloc_coherent(): allow any address */
	.dma_mask = &dev.coherent_dma_mask,  /* other APIs: use the same mask as coherent */
};


/*****************************************************************************
* DSP IPI HANDELER
*****************************************************************************/

static void vow_Task_Unloaded_Handling(void)
{
	pr_debug("%s()\n", __func__);
}

static bool vow_IPICmd_ReceiveAck(ipi_msg_t *ipi_msg)
{
	bool result = false;

	pr_debug("[VOW_Kernel_Ack]vow get ipi id:%x, result: %x, ret data:%x\n",
		 ipi_msg->msg_id, ipi_msg->param1, ipi_msg->param2);

	switch (ipi_msg->msg_id) {
	case IPIMSG_VOW_ENABLE:
	case IPIMSG_VOW_DISABLE:
	case IPIMSG_VOW_SETMODE:
	case IPIMSG_VOW_SET_MODEL:
	case IPIMSG_VOW_SET_SMART_DEVICE:
		if (ipi_msg->param1 == VOW_IPI_SUCCESS)
			result = true;
		break;
	case IPIMSG_VOW_APREGDATA_ADDR:
		if (ipi_msg->param1 == VOW_IPI_SUCCESS)
			result = true;
		break;
	case IPIMSG_VOW_SET_FLAG:
		if (ipi_msg->param1 == VOW_IPI_SUCCESS) {
			unsigned int return_id;
			unsigned int return_value;

			result = true;
			return_id    = (ipi_msg->param2 >> WORD_H);
			return_value = (ipi_msg->param2 & WORD_L_MASK);
			switch (return_id) {
			case VOW_FLAG_FORCE_PHASE1_DEBUG:
			case VOW_FLAG_FORCE_PHASE2_DEBUG:
				vowserv.force_phase_stage = return_value;
				break;
			case VOW_FLAG_SWIP_LOG_PRINT:
				vowserv.swip_log_enable = return_value;
				break;
			default:
				break;
			}
		}
		break;
	default:
		break;
	}
	return result;
}

static void vow_IPICmd_Received(ipi_msg_t *ipi_msg)
{
	/* result: ipi_msg->param1 */
	/* return: ipi_msg->param2 */
	/*pr_debug("[VOW_Kernel]vow get ipi id:%x, result: %x, ret data:%x\n", */
	/*	 ipi_msg->msg_id, ipi_msg->param1, ipi_msg->param2); */
	switch (ipi_msg->msg_id) {
	case IPIMSG_VOW_DATAREADY: {
		if (vowserv.recording_flag) {
			unsigned int *ptr32;

			ptr32 = (unsigned int *)ipi_msg->payload;
			vowserv.voice_buf_offset = (*ptr32++);
			vowserv.voice_length = (*ptr32);
			vow_service_getVoiceData();
		}
		break;
	}
	case IPIMSG_VOW_RECOGNIZE_OK:
		vowserv.enter_phase3_cnt++;
		if (vowserv.bypass_enter_phase3 == false)
			vow_ipi_reg_ok((short)ipi_msg->param2);
		break;
	default:
		break;
	}
}

static bool vow_IPICmd_Send(audio_ipi_msg_data_t data_type,
			    audio_ipi_msg_ack_t ack_type, uint16_t msg_id, uint32_t param1,
			    uint32_t param2, char *payload)
{
	bool ret = true;
	ipi_msg_t ipi_msg;

	audio_send_ipi_msg(&ipi_msg, TASK_SCENE_VOW, AUDIO_IPI_LAYER_KERNEL_TO_SCP,
			   data_type, ack_type, msg_id, param1, param2, payload);
	if (ipi_msg.ack_type == AUDIO_IPI_MSG_ACK_BACK)
	ret = vow_IPICmd_ReceiveAck(&ipi_msg);
	return ret;
}

static void vow_ipi_reg_ok(short id)
{
	vowserv.scp_command_flag = true;
	vowserv.scp_command_id = id;
	VowDrv_Wait_Queue_flag = 1;
	wake_up_interruptible(&VowDrv_Wait_Queue);
}

static void vow_service_getVoiceData(void)
{
	if (VoiceData_Wait_Queue_flag == 0) {
		VoiceData_Wait_Queue_flag = 1;
		wake_up_interruptible(&VoiceData_Wait_Queue);
	} else {
		/*pr_debug("getVoiceData but no one wait for it, may lost it!!\n");*/
	}
}

/*****************************************************************************
* DSP SERVICE FUNCTIONS
*****************************************************************************/
static void vow_service_Init(void)
{
	int I;
	bool ret;

	pr_debug("vow_service_Init:%x\n", init_flag);
	audio_load_task(TASK_SCENE_VOW);
	if (init_flag != 1) {

		/*register IPI handler*/
		audio_task_register_callback(TASK_SCENE_VOW,
					     vow_IPICmd_Received,
					     vow_Task_Unloaded_Handling);
		/*Initialization*/
		VowDrv_Wait_Queue_flag    = 0;
		VoiceData_Wait_Queue_flag = 0;
		vowserv.send_ipi_count    = 0; /* count the busy times */
		vowserv.scp_command_flag  = false;
		vowserv.recording_flag    = false;
		vowserv.suspend_lock      = 0;
		vowserv.voice_length      = 0;
		vowserv.firstRead         = false;
		vowserv.voice_buf_offset  = 0;
		vowserv.bypass_enter_phase3 = false;
		vowserv.enter_phase3_cnt  = 0;
		spin_lock(&vowdrv_lock);
		vowserv.pwr_status        = VOW_PWR_OFF;
		vowserv.eint_status       = VOW_EINT_DISABLE;
		vowserv.force_phase_stage = NO_FORCE;
		vowserv.swip_log_enable   = true;
		spin_unlock(&vowdrv_lock);
		vowserv.vow_init_model    = NULL;
		vowserv.vow_noise_model   = NULL;
		vowserv.vow_fir_model     = NULL;
		for (I = 0; I < MAX_VOW_SPEAKER_MODEL; I++) {
			vowserv.vow_speaker_model[I].model_ptr = NULL;
			vowserv.vow_speaker_model[I].id        = -1;
			vowserv.vow_speaker_model[I].enabled   = 0;
		}
		vowserv.voicddata_scp_ptr = (void *)scp_get_reserve_mem_virt(VOW_MEM_ID)
					    + VOW_VOICEDATA_OFFSET;
		vowserv.voicedata_scp_addr = scp_get_reserve_mem_phys(VOW_MEM_ID)
					     + VOW_VOICEDATA_OFFSET;

		/* pr_debug("Set Debug1 Buffer Address:%x\n", vowserv.voicedata_scp_addr); */

		vowserv.vow_info_dsp[0] = vowserv.voicedata_scp_addr;
		ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
				      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_APREGDATA_ADDR,
				      sizeof(unsigned int) * 1, 0,
				      (char *)&vowserv.vow_info_dsp[0]);
		if (ret == 0)
			pr_debug("IPIMSG_VOW_APREGDATA_ADDR ipi send error\n\r");

		vowserv.voicedata_kernel_ptr = NULL;
		vowserv.voicedata_idx = 0;
		msleep(VOW_WAITCHECK_INTERVAL_MS);
#if VOW_PRE_LEARN_MODE
		VowDrv_SetFlag(VOW_FLAG_PRE_LEARN, true);
#endif
		wake_lock_init(&VOW_suspend_lock, WAKE_LOCK_SUSPEND, "VOW wakelock");
		init_flag = 1;
	} else {
		vowserv.vow_info_dsp[0] = vowserv.voicedata_scp_addr;
		ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
				      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_APREGDATA_ADDR,
				      sizeof(unsigned int) * 1, 0,
				      (char *)&vowserv.vow_info_dsp[0]);
		if (ret == 0)
			pr_debug("IPIMSG_VOW_APREGDATA_ADDR ipi send error\n\r");

#if VOW_PRE_LEARN_MODE
		VowDrv_SetFlag(VOW_FLAG_PRE_LEARN, true);
#endif
	}
}

int vow_service_GetParameter(unsigned long arg)
{
	if (copy_from_user((void *)(&vowserv.vow_info_apuser[0]),
			   (const void __user *)(arg),
			   sizeof(vowserv.vow_info_apuser))) {
		pr_debug("Get parameter fail\n");
		return -EFAULT;
	}
	pr_debug("Get parameter: %lu %lu %lu %lu %lu\n", vowserv.vow_info_apuser[0],
							 vowserv.vow_info_apuser[1],
							 vowserv.vow_info_apuser[2],
							 vowserv.vow_info_apuser[3],
							 vowserv.vow_info_apuser[4]);

	return 0;
}

static int vow_service_CopyModel(int model, int slot)
{
	switch (model) {
	case VOW_MODEL_INIT:
		if (copy_from_user((void *)(vowserv.vow_init_model),
				   (const void __user *)(vowserv.vow_info_apuser[1]),
				   vowserv.vow_info_apuser[2])) {
			pr_debug("Copy Init Model Fail\n");
			return -EFAULT;
		}
		break;
	case VOW_MODEL_SPEAKER:
		if (vowserv.vow_info_apuser[2] > scp_get_reserve_mem_size(VOW_MEM_ID)) {
			pr_debug("DMA Size Too Large\n");
			return -EFAULT;
		}
		if (copy_from_user((void *)(vowserv.vow_speaker_model[slot].model_ptr),
				   (const void __user *)(vowserv.vow_info_apuser[1]),
				   vowserv.vow_info_apuser[2])) {
			pr_debug("Copy Speaker Model Fail\n");
			return -EFAULT;
		}
		vowserv.vow_speaker_model[slot].enabled = 1;
		vowserv.vow_speaker_model[slot].id      = vowserv.vow_info_apuser[0];
		break;
	case VOW_MODEL_NOISE:
		if (copy_from_user((void *)(vowserv.vow_noise_model),
				   (const void __user *)(vowserv.vow_info_apuser[1]),
				   vowserv.vow_info_apuser[2])) {
			pr_debug("Copy Noise Model Fail\n");
			return -EFAULT;
		}
		break;
	case VOW_MODEL_FIR:
		if (copy_from_user((void *)(vowserv.vow_fir_model),
				   (const void __user *)(vowserv.vow_info_apuser[1]),
				   vowserv.vow_info_apuser[2])) {
			pr_debug("Copy FIR Model Fail\n");
			return -EFAULT;
		}
		break;
	default:
		break;
	}
	return 0;
}

static int vow_service_FindFreeSpeakerModel(void)
{
	int I;

	I = 0;
	do {
		if (vowserv.vow_speaker_model[I].enabled == 0)
			break;
		I++;
	} while (I < MAX_VOW_SPEAKER_MODEL);

	PRINTK_VOWDRV("FindFreeSpeakerModel:%d\n", I);

	if (I == MAX_VOW_SPEAKER_MODEL) {
		pr_debug("Find Free Speaker Model Fail\n");
		return -1;
	}
	return I;
}

static int vow_service_SearchSpeakerModel(int id)
{
	int I;

	I = 0;
	do {
		if (vowserv.vow_speaker_model[I].id == id)
			break;
		I++;
	} while (I < MAX_VOW_SPEAKER_MODEL);

	if (I == MAX_VOW_SPEAKER_MODEL) {
		pr_debug("Search Speaker Model By ID Fail:%x\n", id);
		return -1;
	}
	return I;
}

static bool vow_service_ReleaseSpeakerModel(int id)
{
	int I;

	I = vow_service_SearchSpeakerModel(id);

	if (I == -1) {
		pr_debug("Speaker Model Fail:%x\n", id);
		return false;
	}
	PRINTK_VOWDRV("ReleaseSpeakerModel:id_%x\n", id);

	vowserv.vow_speaker_model[I].model_ptr = NULL;
	vowserv.vow_speaker_model[I].id        = -1;
	vowserv.vow_speaker_model[I].enabled   = 0;

	return true;
}

static bool vow_service_SetVowMode(unsigned long arg)
{
	bool ret;

	vow_service_GetParameter(arg);

	vowserv.vow_info_dsp[0] = vowserv.vow_info_apuser[0];

	PRINTK_VOWDRV("SetVowMode:mode_%x\n", vowserv.vow_info_dsp[0]);
	ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
			      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_SETMODE,
			      sizeof(unsigned int) * 1, 0,
			      (char *)&vowserv.vow_info_dsp[0]);
	return ret;
}

static bool vow_service_SetSpeakerModel(unsigned long arg)
{
	int I;
	bool ret;
	char *ptr8;

	I = vow_service_FindFreeSpeakerModel();
	if (I == -1) {
		pr_debug("SetSpeakerModel Fail\n");
		return false;
	}
	vow_service_GetParameter(arg);

	vowserv.vow_speaker_model[I].model_ptr = (void *)scp_get_reserve_mem_virt(VOW_MEM_ID);

	if (vow_service_CopyModel(VOW_MODEL_SPEAKER, I) != 0)
		return false;

	ptr8 = (char *)vowserv.vow_speaker_model[I].model_ptr;
	PRINTK_VOWDRV("SetSPKModel:get_reserve_mem_virt(VOW_MEM_ID): %x, ID: %x, enabled: %x\n",
		      (unsigned int)scp_get_reserve_mem_virt(VOW_MEM_ID),
		      vowserv.vow_speaker_model[I].id,
		      vowserv.vow_speaker_model[I].enabled);
	PRINTK_VOWDRV("SetSPKModel:CheckValue:%x %x %x %x %x %x\n",
		      *(char *)&ptr8[0], *(char *)&ptr8[1], *(char *)&ptr8[2],
		      *(char *)&ptr8[3], *(short *)&ptr8[160], *(int *)&ptr8[7960]);


	vowserv.vow_info_dsp[0] = VOW_MODEL_SPEAKER;
	vowserv.vow_info_dsp[1] = vowserv.vow_info_apuser[0];
	vowserv.vow_info_dsp[2] = scp_get_reserve_mem_phys(VOW_MEM_ID);
	vowserv.vow_info_dsp[3] = vowserv.vow_info_apuser[2];


	PRINTK_VOWDRV("SetSpeakerModel:model_%x,addr_%x, id_%x, size_%x\n",
		      vowserv.vow_info_dsp[0],
		      vowserv.vow_info_dsp[2],
		      vowserv.vow_info_dsp[1],
		      vowserv.vow_info_dsp[3]);

	ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
			      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_SET_MODEL,
			      sizeof(unsigned int) * 4, 0,
			      (char *)&vowserv.vow_info_dsp[0]);
	return ret;
}

static bool vow_service_SetInitModel(unsigned long arg)
{
	return true;
}

static bool vow_service_SetNoiseModel(unsigned long arg)
{
	return true;
}

static bool vow_service_SetFirModel(unsigned long arg)
{
	return true;
}

static bool vow_service_SetVBufAddr(unsigned long arg)
{
	vow_service_GetParameter(arg);

	pr_debug("SetVBufAddr:addr_%x, size_%x\n",
		 (unsigned int)vowserv.vow_info_apuser[1],
		 (unsigned int)vowserv.vow_info_apuser[2]);
	if (vowserv.voicedata_kernel_ptr != NULL)
		vfree(vowserv.voicedata_kernel_ptr);

	vowserv.voicedata_user_addr = vowserv.vow_info_apuser[1];
	vowserv.voicedata_user_size = vowserv.vow_info_apuser[2];
	vowserv.voicedata_user_return_size_addr = vowserv.vow_info_apuser[3];
	vowserv.voicedata_kernel_ptr = vmalloc(vowserv.voicedata_user_size);

	VOW_ASSERT(vowserv.voicedata_kernel_ptr != NULL);

	return true;
}

static bool vow_service_Enable(void)
{
	bool ret;

	PRINTK_VOWDRV("vow_service_Enable\n");
	if ((vowserv.recording_flag) && (vowserv.suspend_lock == 0)) {
		vowserv.suspend_lock = 1;
		wake_lock(&VOW_suspend_lock); /* Let AP will not suspend */
		PRINTK_VOWDRV("==DEBUG MODE START==\n");
	}
	ret = vow_IPICmd_Send(AUDIO_IPI_MSG_ONLY,
			      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_ENABLE,
			      0, 0,
			      NULL);
	return ret;
}

static bool vow_service_Disable(void)
{
	bool ret;

	PRINTK_VOWDRV("vow_service_Disable\n");
	ret = vow_IPICmd_Send(AUDIO_IPI_MSG_ONLY,
			      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_DISABLE,
			      0, 0,
			      NULL);
	if ((VowDrv_GetHWStatus() == VOW_PWR_ON) && (vowserv.recording_flag == true))
		vow_service_getVoiceData();

	if (vowserv.suspend_lock == 1) {
		vowserv.suspend_lock = 0;
		wake_unlock(&VOW_suspend_lock); /* Let AP will suspend */
		PRINTK_VOWDRV("==DEBUG MODE STOP==\n");
	}
	scp_deregister_feature(VOW_FEATURE_ID);
	return ret;
}

static bool vow_service_SyncVoiceDataAck(void)
{
	bool ret;

	ret = vow_IPICmd_Send(AUDIO_IPI_MSG_ONLY,
			      AUDIO_IPI_MSG_BYPASS_ACK, IPIMSG_VOW_DATAREADY_ACK,
			      0, 0,
			      NULL);
	return ret;
}

static void vow_service_ReadVoiceData(void)
{
	int stop_condition = 0;
	unsigned int fail_cnt = 0;

	/*int rdata;*/
	while (1) {
		if (VoiceData_Wait_Queue_flag == 0)
			wait_event_interruptible(VoiceData_Wait_Queue, VoiceData_Wait_Queue_flag);

		if (VoiceData_Wait_Queue_flag == 1) {
			VoiceData_Wait_Queue_flag = 0;
			if ((VowDrv_GetHWStatus() == VOW_PWR_OFF) || (vowserv.recording_flag == false)) {
				vowserv.voicedata_idx = 0;
				stop_condition = 1;
				PRINTK_VOWDRV("stop read vow voice data: %d, %d\n",
					      VowDrv_GetHWStatus(), vowserv.recording_flag);
			} else {
				/*PRINTK_VOWDRV("get once:%x\n",vowserv.voice_length);*/
				VOW_ASSERT(vowserv.voicedata_kernel_ptr != NULL);

				memcpy(&vowserv.voicedata_kernel_ptr[vowserv.voicedata_idx],
				       vowserv.voicddata_scp_ptr + vowserv.voice_buf_offset,
				       vowserv.voice_length);

				fail_cnt = 0;
				while (!vow_service_SyncVoiceDataAck()) {
					PRINTK_VOWDRV("SendVoiceDataAck Again\n");
					fail_cnt++;
					if (fail_cnt > VOW_IPI_SEND_CNT_TIMEOUT) {
						PRINTK_VOWDRV("IPI SEND TIMEOUT\n");
						VOW_ASSERT(0);
					}
				}

				/*PRINTK_VOWDRV("TX Leng:%d, %d, %d\n", vowserv.voicedata_idx,*/
				/*	      vowserv.voice_buf_offset, vowserv.voice_length);*/

				vowserv.voicedata_idx += (vowserv.voice_length >> 1);

				if (vowserv.voicedata_idx >= (VOW_VOICE_RECORD_BIG_THRESHOLD >> 1))
					vowserv.transfer_length = VOW_VOICE_RECORD_BIG_THRESHOLD;
				else
					vowserv.transfer_length = VOW_VOICE_RECORD_THRESHOLD;

				if (vowserv.voicedata_idx >= (vowserv.transfer_length >> 1)) {
					unsigned int ret;

					ret = copy_to_user((void __user *)(vowserv.voicedata_user_return_size_addr),
							   &vowserv.transfer_length,
							   4);

					ret = copy_to_user((void __user *)vowserv.voicedata_user_addr,
							   vowserv.voicedata_kernel_ptr,
							   vowserv.transfer_length);

					/* move left data to buffer's head */
					if (vowserv.voicedata_idx > (vowserv.transfer_length >> 1)) {
						memcpy(&vowserv.voicedata_kernel_ptr[0],
						       &vowserv.voicedata_kernel_ptr[(vowserv.transfer_length >> 1)],
						       (vowserv.voicedata_idx << 1) - vowserv.transfer_length);
						vowserv.voicedata_idx -= (vowserv.transfer_length >> 1);
					} else
						vowserv.voicedata_idx = 0;
					stop_condition = 1;
				}
			}

			if (stop_condition == 1)
				break;

		}
	}
}


/*****************************************************************************
 * VOW CONTROL FUNCTIONS
*****************************************************************************/

static int VowDrv_SetHWStatus(int status)
{
	int ret = 0;

	PRINTK_VOWDRV("SetHWStatus:set:%x, cur:%x\n", status, vowserv.pwr_status);
	if ((status < NUM_OF_VOW_PWR_STATUS) && (status >= VOW_PWR_OFF)) {
		spin_lock(&vowdrv_lock);
		vowserv.pwr_status = status;
		spin_unlock(&vowdrv_lock);
	} else {
		PRINTK_VOWDRV("VowDrv_SetHWStatus error input:%d\n", status);
		ret = -1;
	}
	return ret;
}

static int VowDrv_GetHWStatus(void)
{
	int ret = 0;

	spin_lock(&vowdrv_lock);
	ret = vowserv.pwr_status;
	spin_unlock(&vowdrv_lock);
	return ret;
}

int VowDrv_EnableHW(int status)
{
	int ret = 0;
	int pwr_status = 0;

	PRINTK_VOWDRV("VowDrv_EnableHW:%x\n", status);

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return -1;
	}

	if (status < 0) {
		pr_debug("VowDrv_EnableHW error input:%x\n", status);
		ret = -1;
	} else {
		pwr_status = (status == 0)?VOW_PWR_OFF : VOW_PWR_ON;

		if (pwr_status == VOW_PWR_ON) {
			scp_register_feature(VOW_FEATURE_ID);
			/* clear enter_phase3_cnt */
			vowserv.enter_phase3_cnt = 0;
		}
		VowDrv_SetHWStatus(pwr_status);
	}
	return ret;
}

int VowDrv_ChangeStatus(void)
{
	PRINTK_VOWDRV("VowDrv_ChangeStatus\n");

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return -1;
	}

	VowDrv_Wait_Queue_flag = 1;
	wake_up_interruptible(&VowDrv_Wait_Queue);
	return 0;
}

void VowDrv_SetSmartDevice(bool enable)
{
	bool ret;
	unsigned int eint_num;
	unsigned ints[2] = {0, 0};

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return;
	}

	PRINTK_VOWDRV("VowDrv_SetSmartDevice:%x\n", enable);
	if (vowserv.node) {
		of_property_read_u32_array(vowserv.node, "debounce", ints, ARRAY_SIZE(ints));
		switch (ints[0]) {
		case 61:
			eint_num = 0;
			break;
		case 62:
			eint_num = 1;
			break;
		case 63:
			eint_num = 2;
			break;
		case 64:
			eint_num = 3;
			break;
		case 65:
			eint_num = 4;
			break;
		case 66:
			eint_num = 5;
			break;
		case 67:
			eint_num = 6;
			break;
		case 68:
			eint_num = 7;
			break;
		case 85:
			eint_num = 8;
			break;
		case 86:
			eint_num = 9;
			break;
		case 87:
			eint_num = 10;
			break;
		case 88:
			eint_num = 11;
			break;
		case 89:
			eint_num = 12;
			break;
		case 90:
			eint_num = 13;
			break;
		case 91:
			eint_num = 14;
			break;
		case 92:
			eint_num = 15;
			break;
		case 93:
			eint_num = 16;
			break;
		default:
			eint_num = 0xFF;
			break;
		}
		if (enable == false)
			eint_num = 0xFF;

		vowserv.vow_info_dsp[0] = enable;
		vowserv.vow_info_dsp[1] = eint_num;
		ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
				      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_SET_SMART_DEVICE,
				      sizeof(unsigned int) * 2, 0,
				      (char *)&vowserv.vow_info_dsp[0]);
		if (ret == 0)
			pr_debug("IPIMSG_VOW_SET_SMART_DEVICE ipi send error\n\r");
	} else {
		/* no node here */
		PRINTK_VOWDRV("there is no node\n");
	}
}

void VowDrv_SetSmartDevice_GPIO(bool enable)
{
	if (vowserv.node) {
		if (enable == false) {
			PRINTK_VOWDRV("VowDrv_SetSmartDev_gpio:OFF\n");
			pinctrl_select_state(vowserv.pinctrl, vowserv.pins_eint_off);
		} else {
			PRINTK_VOWDRV("VowDrv_SetSmartDev_gpio:ON\n");
			pinctrl_select_state(vowserv.pinctrl, vowserv.pins_eint_on);
		}
	} else {
		/* no node here */
		PRINTK_VOWDRV("there is no node\n");
	}
}

static bool VowDrv_SetFlag(VOW_FLAG_TYPE type, bool set)
{
	bool ret;

	pr_debug("VowDrv_SetFlag:type%x, set:%x\n", type, set);
	vowserv.vow_info_dsp[0] = type;
	vowserv.vow_info_dsp[1] = set;

	ret = vow_IPICmd_Send(AUDIO_IPI_PAYLOAD,
			      AUDIO_IPI_MSG_NEED_ACK, IPIMSG_VOW_SET_FLAG,
			      sizeof(unsigned int) * 2, 0,
			      (char *)&vowserv.vow_info_dsp[0]);
	if (ret == 0)
		pr_debug("IPIMSG_VOW_SET_FLAG ipi send error\n\r");

	return ret;
}

void VowDrv_SetDmicLowPower(bool enable)
{
	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return;
	}

	VowDrv_SetFlag(VOW_FLAG_DMIC_LOWPOWER, enable);
}

void VowDrv_SetPeriodicEnable(bool enable)
{
	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return;
	}

	VowDrv_SetFlag(VOW_FLAG_PERIODIC_ENABLE, enable);
}

static ssize_t VowDrv_GetPhase1Debug(struct device *kobj, struct device_attribute *attr, char *buf)
{
	unsigned int stat;
	char cstr[35];
	int size = sizeof(cstr);

	stat = (vowserv.force_phase_stage == FORCE_PHASE1) ? 1 : 0;

	return snprintf(buf, size, "Force Phase1 Setting = %s\n", (stat == 0x1) ? "YES" : "NO");
}

static ssize_t VowDrv_SetPhase1Debug(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int enable;

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return n;
	}

	if (kstrtouint(buf, 0, &enable) != 0)
		return -EINVAL;

	VowDrv_SetFlag(VOW_FLAG_FORCE_PHASE1_DEBUG, enable);
	return n;
}
DEVICE_ATTR(vow_SetPhase1, S_IWUSR | S_IRUGO, VowDrv_GetPhase1Debug, VowDrv_SetPhase1Debug);

static ssize_t VowDrv_GetPhase2Debug(struct device *kobj, struct device_attribute *attr, char *buf)
{
	unsigned int stat;
	char cstr[35];
	int size = sizeof(cstr);

	stat = (vowserv.force_phase_stage == FORCE_PHASE2) ? 1 : 0;

	return snprintf(buf, size, "Force Phase2 Setting = %s\n", (stat == 0x1) ? "YES" : "NO");
}

static ssize_t VowDrv_SetPhase2Debug(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int enable;

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return n;
	}

	if (kstrtouint(buf, 0, &enable) != 0)
		return -EINVAL;

	VowDrv_SetFlag(VOW_FLAG_FORCE_PHASE2_DEBUG, enable);
	return n;
}
DEVICE_ATTR(vow_SetPhase2, S_IWUSR | S_IRUGO, VowDrv_GetPhase2Debug, VowDrv_SetPhase2Debug);

static ssize_t VowDrv_GetBypassPhase3Flag(struct device *kobj, struct device_attribute *attr, char *buf)
{
	unsigned int stat;
	char cstr[35];
	int size = sizeof(cstr);

	stat = (vowserv.bypass_enter_phase3 == true) ? 1 : 0;

	return snprintf(buf, size, "Enter Phase3 Setting is %s\n", (stat == 0x1) ? "Bypass" : "Allow");
}

static ssize_t VowDrv_SetBypassPhase3Flag(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int enable;

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return n;
	}

	if (kstrtouint(buf, 0, &enable) != 0)
		return -EINVAL;

	if (enable == 0) {
		PRINTK_VOWDRV("Allow enter phase3\n");
		vowserv.bypass_enter_phase3 = false;
	} else {
		PRINTK_VOWDRV("Bypass enter phase3\n");
		vowserv.bypass_enter_phase3 = true;
	}
	return n;
}
DEVICE_ATTR(vow_SetBypassPhase3, S_IWUSR | S_IRUGO, VowDrv_GetBypassPhase3Flag, VowDrv_SetBypassPhase3Flag);

static ssize_t VowDrv_GetEnterPhase3Counter(struct device *kobj, struct device_attribute *attr, char *buf)
{
	char cstr[35];
	int size = sizeof(cstr);

	return snprintf(buf, size, "Enter Phase3 Counter is %u\n", vowserv.enter_phase3_cnt);
}
DEVICE_ATTR(vow_GetEnterPhase3Counter, S_IRUGO, VowDrv_GetEnterPhase3Counter, NULL);

static ssize_t VowDrv_GetSWIPLog(struct device *kobj, struct device_attribute *attr, char *buf)
{
	unsigned int stat;
	char cstr[20];
	int size = sizeof(cstr);

	stat = (vowserv.swip_log_enable == true) ? 1 : 0;
	return snprintf(buf, size, "SWIP LOG is %s\n", (stat == true) ? "YES" : "NO");
}

static ssize_t VowDrv_SetSWIPLog(struct device *kobj, struct device_attribute *attr, const char *buf, size_t n)
{
	unsigned int enable;

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is off, do not support VOW\n");
		return n;
	}

	if (kstrtouint(buf, 0, &enable) != 0)
		return -EINVAL;

	VowDrv_SetFlag(VOW_FLAG_SWIP_LOG_PRINT, enable);
	return n;
}
DEVICE_ATTR(vow_SetLibLog, S_IWUSR | S_IRUGO, VowDrv_GetSWIPLog, VowDrv_SetSWIPLog);

static int VowDrv_SetVowEINTStatus(int status)
{
	int ret = 0;
	int wakeup_event = 0;

	if ((status < NUM_OF_VOW_EINT_STATUS) && (status >= VOW_EINT_DISABLE)) {
		spin_lock(&vowdrv_lock);
		if ((vowserv.eint_status != VOW_EINT_PASS) && (status == VOW_EINT_PASS))
			wakeup_event = 1;
		vowserv.eint_status = status;
		spin_unlock(&vowdrv_lock);
	} else {
		pr_debug("VowDrv_SetVowEINTStatus error input:%x\n", status);
		ret = -1;
	}
	return ret;
}

static int VowDrv_QueryVowEINTStatus(void)
{
	int ret = 0;

	spin_lock(&vowdrv_lock);
	ret = vowserv.eint_status;
	spin_unlock(&vowdrv_lock);
	PRINTK_VOWDRV("VowDrv_QueryVowEINTStatus :%d\n", ret);
	return ret;
}

static int VowDrv_open(struct inode *inode, struct file *fp)
{
	PRINTK_VOWDRV("VowDrv_open do nothing inode:%p, file:%p\n", inode, fp);
	return 0;
}

static int VowDrv_release(struct inode *inode, struct file *fp)
{
	PRINTK_VOWDRV("VowDrv_release inode:%p, file:%p\n", inode, fp);

	if (!(fp->f_mode & FMODE_WRITE || fp->f_mode & FMODE_READ))
		return -ENODEV;
	return 0;
}

static long VowDrv_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	int  ret = 0;

	if (!vow_check_scp_status()) {
		PRINTK_VOWDRV("SCP is Off, do not support VOW\n");
		return 0;
	}

	/*PRINTK_VOWDRV("VowDrv_ioctl cmd = %u arg = %lu\n", cmd, arg);*/
	/*PRINTK_VOWDRV("VowDrv_ioctl check arg = %u %u\n", VOWEINT_GET_BUFSIZE, VOW_SET_CONTROL);*/
	switch ((unsigned int)cmd) {
	case VOWEINT_GET_BUFSIZE:
		pr_debug("VOWEINT_GET_BUFSIZE\n");
		ret = sizeof(VOW_EINT_DATA_STRUCT);
		break;
	case VOW_GET_STATUS:
		pr_debug("VOW_GET_STATUS\n");
		ret = VowDrv_QueryVowEINTStatus();
		break;
	case VOW_SET_CONTROL:
		switch (arg) {
		case VOWControlCmd_Init:
			pr_debug("VOW_SET_CONTROL VOWControlCmd_Init");
			vow_service_Init();
			break;
		case VOWControlCmd_ReadVoiceData:
			if ((vowserv.recording_flag == true) && (vowserv.firstRead == true)) {
				vowserv.firstRead = false;
				VowDrv_SetFlag(VOW_FLAG_DEBUG, true);
			}
			vow_service_ReadVoiceData();
			break;
		case VOWControlCmd_EnableDebug:
			pr_debug("VOW_SET_CONTROL VOWControlCmd_EnableDebug");
			vowserv.voicedata_idx = 0;
			vowserv.recording_flag = true;
			vowserv.suspend_lock = 0;
			vowserv.firstRead = true;
			/*VowDrv_SetFlag(VOW_FLAG_DEBUG, true);*/
			break;
		case VOWControlCmd_DisableDebug:
			pr_debug("VOW_SET_CONTROL VOWControlCmd_DisableDebug");
			VowDrv_SetFlag(VOW_FLAG_DEBUG, false);
			vowserv.recording_flag = false;
			vow_service_getVoiceData();
			break;
		default:
			pr_debug("VOW_SET_CONTROL no such command = %lu", arg);
			break;
		}
		break;
	case VOW_SET_SPEAKER_MODEL:
		pr_debug("VOW_SET_SPEAKER_MODEL(%lu)", arg);
		if (!vow_service_SetSpeakerModel(arg))
			ret = -EFAULT;
		break;
	case VOW_CLR_SPEAKER_MODEL:
		pr_debug("VOW_CLR_SPEAKER_MODEL(%lu)", arg);
		if (!vow_service_ReleaseSpeakerModel(arg))
			ret = -EFAULT;
		break;
	case VOW_SET_INIT_MODEL:
		pr_debug("VOW_SET_INIT_MODEL(%lu)", arg);
		if (!vow_service_SetInitModel(arg))
			ret = -EFAULT;
		break;
	case VOW_SET_FIR_MODEL:
		pr_debug("VOW_SET_FIR_MODEL(%lu)", arg);
		if (!vow_service_SetFirModel(arg))
			ret = -EFAULT;
		break;
	case VOW_SET_NOISE_MODEL:
		pr_debug("VOW_SET_NOISE_MODEL(%lu)", arg);
		if (!vow_service_SetNoiseModel(arg))
			ret = -EFAULT;
		break;
	case VOW_SET_APREG_INFO:
		pr_debug("VOW_SET_APREG_INFO(%lu)", arg);
		if (!vow_service_SetVBufAddr(arg))
			ret = -EFAULT;
		break;
	case VOW_SET_REG_MODE:
		pr_debug("VOW_SET_MODE(%lu)", arg);
		if (!vow_service_SetVowMode(arg))
			ret = -EFAULT;
		break;
	case VOW_FAKE_WAKEUP:
		vow_ipi_reg_ok(0);
		pr_debug("VOW_FAKE_WAKEUP(%lu)", arg);
		break;
	default:
		pr_debug("WrongParameter(%lu)", arg);
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long VowDrv_compat_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	long ret = 0;

	/*int err;*/
	/*PRINTK_VOWDRV("++VowDrv_compat_ioctl cmd = %u arg = %lu\n", cmd, arg);*/
	if (!fp->f_op || !fp->f_op->unlocked_ioctl) {
		(void)ret;
		return -ENOTTY;
	}
	switch (cmd) {
	case VOWEINT_GET_BUFSIZE:
	case VOW_GET_STATUS:
	case VOW_FAKE_WAKEUP:
	case VOW_SET_REG_MODE:
	case VOW_CLR_SPEAKER_MODEL:
	case VOW_SET_CONTROL:
		ret = fp->f_op->unlocked_ioctl(fp, cmd, arg);
		break;
	case VOW_SET_SPEAKER_MODEL:
	case VOW_SET_INIT_MODEL:
	case VOW_SET_FIR_MODEL:
	case VOW_SET_NOISE_MODEL:
	case VOW_SET_APREG_INFO: {
		VOW_MODEL_INFO_KERNEL_T __user *data32;

		VOW_MODEL_INFO_T __user *data;

		int err;
		compat_size_t l;
		compat_uptr_t p;

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));

		err  = get_user(l, &data32->id);
		err |= put_user(l, &data->id);
		err |= get_user(l, &data32->addr);
		err |= put_user(l, &data->addr);
		err |= get_user(l, &data32->size);
		err |= put_user(l, &data->size);
		err |= get_user(l, &data32->return_size_addr);
		err |= put_user(l, &data->return_size_addr);
		err |= get_user(p, (compat_uptr_t *)&data32->data);
		err |= put_user(p, (compat_uptr_t *)&data->data);
		PRINTK_VOWDRV("VowDrv_compat_ioctl_getpar:size: %lu %lu %lu %lu %lu\n",
			      data->id, data->addr, data->size, data->return_size_addr, (unsigned int long)data->data);
		ret = fp->f_op->unlocked_ioctl(fp, cmd, (unsigned long)data);
	}
		break;
	default:
		break;
	}
	/*PRINTK_VOWDRV("--VowDrv_compat_ioctl\n");*/
	return ret;
}
#endif

static ssize_t VowDrv_write(struct file *fp, const char __user *data, size_t count, loff_t *offset)
{
	/*PRINTK_VOWDRV("+VowDrv_write = %p count = %d\n",fp ,count);*/
	return 0;
}

static ssize_t VowDrv_read(struct file *fp,  char __user *data, size_t count, loff_t *offset)
{
	unsigned int read_count = 0;
	int ret = 0;

	PRINTK_VOWDRV("+VowDrv_read+\n");
	VowDrv_SetVowEINTStatus(VOW_EINT_RETRY);

	if (VowDrv_Wait_Queue_flag == 0)
		ret = wait_event_interruptible(VowDrv_Wait_Queue, VowDrv_Wait_Queue_flag);
	if (VowDrv_Wait_Queue_flag == 1) {
		VowDrv_Wait_Queue_flag = 0;
		if (VowDrv_GetHWStatus() == VOW_PWR_OFF) {
			PRINTK_VOWDRV("Enter_phase3_cnt = %d\n", vowserv.enter_phase3_cnt);
			vow_service_Disable();
			vowserv.scp_command_flag = false;
			VowDrv_SetVowEINTStatus(VOW_EINT_DISABLE);
		} else {
			vow_service_Enable();
			if (vowserv.scp_command_flag) {
				VowDrv_SetVowEINTStatus(VOW_EINT_PASS);
				pr_debug("Wakeup by SCP\n");
				if (vowserv.suspend_lock == 0)
					wake_lock_timeout(&VOW_suspend_lock, HZ / 2);
				vowserv.scp_command_flag = false;
			} else {
				pr_debug("Wakeup by other signal(%d,%d)\n",
					 VowDrv_Wait_Queue_flag, VowDrv_GetHWStatus());
			}
		}
	}

	VOW_EINT_DATA_STRUCT.eint_status = VowDrv_QueryVowEINTStatus();
	read_count = copy_to_user((void __user *)data,
				  &VOW_EINT_DATA_STRUCT,
				  sizeof(VOW_EINT_DATA_STRUCT));
	PRINTK_VOWDRV("+VowDrv_read-\n");
	return read_count;
}

static int VowDrv_flush(struct file *flip, fl_owner_t id)
{
	PRINTK_VOWDRV("+VowDrv_flush\n");
	PRINTK_VOWDRV("-VowDrv_flush\n");
	return 0;
}

static int VowDrv_fasync(int fd, struct file *flip, int mode)
{
	PRINTK_VOWDRV("VowDrv_fasync\n");
	/*return fasync_helper(fd, flip, mode, &VowDrv_fasync);*/
	return 0;
}

static int VowDrv_remap_mmap(struct file *flip, struct vm_area_struct *vma)
{
	PRINTK_VOWDRV("VowDrv_remap_mmap\n");
	return -1;
}

int VowDrv_setup_smartdev_eint(void)
{
	int ret;
	unsigned ints[2] = {0, 0};

	/* gpio setting */
	vowserv.pinctrl = devm_pinctrl_get(&VowPltFmDev->dev);
	if (IS_ERR(vowserv.pinctrl)) {
		ret = PTR_ERR(vowserv.pinctrl);
		PRINTK_VOWDRV("Cannot find Vow pinctrl!\n");
		return ret;
	}
	vowserv.pins_eint_on = pinctrl_lookup_state(vowserv.pinctrl, "vow_smartdev_eint_on");
	if (IS_ERR(vowserv.pins_eint_on)) {
		ret = PTR_ERR(vowserv.pins_eint_on);
		PRINTK_VOWDRV("Cannot find alps pinctrl default!\n");
	}

	vowserv.pins_eint_off = pinctrl_lookup_state(vowserv.pinctrl, "vow_smartdev_eint_off");
	if (IS_ERR(vowserv.pins_eint_off)) {
		ret = PTR_ERR(vowserv.pins_eint_off);
		PRINTK_VOWDRV("Cannot find alps pinctrl pin_cfg!\n");
		return ret;
	}
	/* eint setting */
	/* pinctrl_select_state(pinctrl, pins_eint_on); */
	vowserv.node = of_find_compatible_node(NULL, NULL, "mediatek,vow");
	if (vowserv.node) {
		of_property_read_u32_array(vowserv.node, "debounce", ints, ARRAY_SIZE(ints));
		PRINTK_VOWDRV("EINT ID: %x\n", ints[0]);
	} else {
		/* no node here */
		PRINTK_VOWDRV("there is no this node\n");
	}
	return 0;
}

/*****************************************************************************
 * VOW platform driver Registration
*****************************************************************************/
static int VowDrv_probe(struct platform_device *dev)
{
	PRINTK_VOWDRV("+VowDrv_probe\n");
	VowPltFmDev = dev;
	VowDrv_setup_smartdev_eint();
	return 0;
}

static int VowDrv_remove(struct platform_device *dev)
{
	PRINTK_VOWDRV("+VowDrv_remove\n");
	/*[Todo]Add opearations*/
	PRINTK_VOWDRV("-VowDrv_remove\n");
	return 0;
}

static void VowDrv_shutdown(struct platform_device *dev)
{
	PRINTK_VOWDRV("+VowDrv_shutdown\n");
	PRINTK_VOWDRV("-VowDrv_shutdown\n");
}

static int VowDrv_suspend(struct platform_device *dev, pm_message_t state)
{
	/* only one suspend mode */
	PRINTK_VOWDRV("VowDrv_suspend\n");
	return 0;
}

static int VowDrv_resume(struct platform_device *dev) /* wake up */
{
	PRINTK_VOWDRV("VowDrv_resume\n");
	return 0;
}

static const struct file_operations VOW_fops = {
	.owner   = THIS_MODULE,
	.open    = VowDrv_open,
	.release = VowDrv_release,
	.unlocked_ioctl   = VowDrv_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = VowDrv_compat_ioctl,
#endif
	.write   = VowDrv_write,
	.read    = VowDrv_read,
	.flush   = VowDrv_flush,
	.fasync  = VowDrv_fasync,
	.mmap    = VowDrv_remap_mmap
};

static struct miscdevice VowDrv_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VOW_DEVNAME,
	.fops = &VOW_fops,
};

const struct dev_pm_ops VowDrv_pm_ops = {
	.suspend = NULL,
	.resume = NULL,
	.freeze = NULL,
	.thaw = NULL,
	.poweroff = NULL,
	.restore = NULL,
	.restore_noirq = NULL,
};

#ifdef CONFIG_OF
static const struct of_device_id vow_of_match[] = {
	{.compatible = "mediatek,vow"},
	{},
};
#endif

static struct platform_driver VowDrv_driver = {
	.probe    = VowDrv_probe,
	.remove   = VowDrv_remove,
	.shutdown = VowDrv_shutdown,
	.suspend  = VowDrv_suspend,
	.resume   = VowDrv_resume,
	.driver   = {
#ifdef CONFIG_PM
	.pm       = &VowDrv_pm_ops,
#endif
	.name     = vowdrv_name,
#ifdef CONFIG_OF
	.of_match_table = vow_of_match,
#endif
	},
};

static int VowDrv_mod_init(void)
{
	int ret = 0;

	PRINTK_VOWDRV("+VowDrv_mod_init\n");

	/* Register platform DRIVER */
	ret = platform_driver_register(&VowDrv_driver);
	if (ret != 0) {
		PRINTK_VOWDRV("VowDrv Fail:%d - Register DRIVER\n", ret);
		return ret;
	}

	/* register MISC device */
	ret = misc_register(&VowDrv_misc_device);
	if (ret != 0) {
		PRINTK_VOWDRV("VowDrv_probe misc_register Fail:%d\n", ret);
		return ret;
	}

	ret = device_create_file(VowDrv_misc_device.this_device, &dev_attr_vow_SetPhase1);
	if (unlikely(ret != 0))
		return ret;
	ret = device_create_file(VowDrv_misc_device.this_device, &dev_attr_vow_SetPhase2);
	if (unlikely(ret != 0))
		return ret;
	ret = device_create_file(VowDrv_misc_device.this_device, &dev_attr_vow_SetBypassPhase3);
	if (unlikely(ret != 0))
		return ret;
	ret = device_create_file(VowDrv_misc_device.this_device, &dev_attr_vow_GetEnterPhase3Counter);
	if (unlikely(ret != 0))
		return ret;
	ret = device_create_file(VowDrv_misc_device.this_device, &dev_attr_vow_SetLibLog);
	if (unlikely(ret != 0))
		return ret;

	PRINTK_VOWDRV("VowDrv_mod_init: Init Audio WakeLock\n");

	return 0;
}

static void  VowDrv_mod_exit(void)
{
	PRINTK_VOWDRV("+VowDrv_mod_exit\n");
	PRINTK_VOWDRV("-VowDrv_mod_exit\n");
}
module_init(VowDrv_mod_init);
module_exit(VowDrv_mod_exit);


/*****************************************************************************
* License
*****************************************************************************/

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek VOW Driver");
MODULE_AUTHOR("Charlie Lu<charlie.lu@mediatek.com>");