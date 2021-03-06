/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "smumgr.h"
#include "vega10_inc.h"
#include "pp_soc15.h"
#include "vega10_smumgr.h"
#include "vega10_ppsmc.h"
#include "smu9_driver_if.h"

#include "ppatomctrl.h"
#include "pp_debug.h"
#include "smu_ucode_xfer_vi.h"
#include "smu7_smumgr.h"

#define AVFS_EN_MSB		1568
#define AVFS_EN_LSB		1568

#define VOLTAGE_SCALE	4

/* Microcode file is stored in this buffer */
#define BUFFER_SIZE                 80000
#define MAX_STRING_SIZE             15
#define BUFFER_SIZETWO              131072 /* 128 *1024 */

/* MP Apertures */
#define MP0_Public                  0x03800000
#define MP0_SRAM                    0x03900000
#define MP1_Public                  0x03b00000
#define MP1_SRAM                    0x03c00004

#define smnMP1_FIRMWARE_FLAGS                                                                           0x3010028
#define smnMP0_FW_INTF                                                                                  0x3010104
#define smnMP1_PUB_CTRL                                                                                 0x3010b14

static bool vega10_is_smc_ram_running(struct pp_hwmgr *hwmgr)
{
	uint32_t mp1_fw_flags, reg;

	reg = soc15_get_register_offset(NBIF_HWID, 0,
			mmPCIE_INDEX2_BASE_IDX, mmPCIE_INDEX2);

	cgs_write_register(hwmgr->device, reg,
			(MP1_Public | (smnMP1_FIRMWARE_FLAGS & 0xffffffff)));

	reg = soc15_get_register_offset(NBIF_HWID, 0,
			mmPCIE_DATA2_BASE_IDX, mmPCIE_DATA2);

	mp1_fw_flags = cgs_read_register(hwmgr->device, reg);

	if (mp1_fw_flags & MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK)
		return true;

	return false;
}

/*
 * Check if SMC has responded to previous message.
 *
 * @param    smumgr  the address of the powerplay hardware manager.
 * @return   TRUE    SMC has responded, FALSE otherwise.
 */
static uint32_t vega10_wait_for_response(struct pp_hwmgr *hwmgr)
{
	uint32_t reg;
	uint32_t ret;

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_90_BASE_IDX, mmMP1_SMN_C2PMSG_90);

	ret = phm_wait_for_register_unequal(hwmgr, reg,
			0, MP1_C2PMSG_90__CONTENT_MASK);

	if (ret)
		pr_err("No response from smu\n");

	return cgs_read_register(hwmgr->device, reg);
}

/*
 * Send a message to the SMC, and do not wait for its response.
 * @param    smumgr  the address of the powerplay hardware manager.
 * @param    msg the message to send.
 * @return   Always return 0.
 */
int vega10_send_msg_to_smc_without_waiting(struct pp_hwmgr *hwmgr,
		uint16_t msg)
{
	uint32_t reg;

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_66_BASE_IDX, mmMP1_SMN_C2PMSG_66);
	cgs_write_register(hwmgr->device, reg, msg);

	return 0;
}

/*
 * Send a message to the SMC, and wait for its response.
 * @param    hwmgr  the address of the powerplay hardware manager.
 * @param    msg the message to send.
 * @return   Always return 0.
 */
int vega10_send_msg_to_smc(struct pp_hwmgr *hwmgr, uint16_t msg)
{
	uint32_t reg;
	uint32_t ret;

	vega10_wait_for_response(hwmgr);

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_90_BASE_IDX, mmMP1_SMN_C2PMSG_90);
	cgs_write_register(hwmgr->device, reg, 0);

	vega10_send_msg_to_smc_without_waiting(hwmgr, msg);

	ret = vega10_wait_for_response(hwmgr);
	if (ret != 1)
		pr_err("Failed to send message: 0x%x, ret value: 0x%x\n", msg, ret);

	return 0;
}

/*
 * Send a message to the SMC with parameter
 * @param    hwmgr:  the address of the powerplay hardware manager.
 * @param    msg: the message to send.
 * @param    parameter: the parameter to send
 * @return   Always return 0.
 */
int vega10_send_msg_to_smc_with_parameter(struct pp_hwmgr *hwmgr,
		uint16_t msg, uint32_t parameter)
{
	uint32_t reg;
	uint32_t ret;

	vega10_wait_for_response(hwmgr);

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_90_BASE_IDX, mmMP1_SMN_C2PMSG_90);
	cgs_write_register(hwmgr->device, reg, 0);

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_82_BASE_IDX, mmMP1_SMN_C2PMSG_82);
	cgs_write_register(hwmgr->device, reg, parameter);

	vega10_send_msg_to_smc_without_waiting(hwmgr, msg);

	ret = vega10_wait_for_response(hwmgr);
	if (ret != 1)
		pr_err("Failed message: 0x%x, input parameter: 0x%x, error code: 0x%x\n", msg, parameter, ret);

	return 0;
}


/*
 * Send a message to the SMC with parameter, do not wait for response
 * @param    hwmgr:  the address of the powerplay hardware manager.
 * @param    msg: the message to send.
 * @param    parameter: the parameter to send
 * @return   The response that came from the SMC.
 */
int vega10_send_msg_to_smc_with_parameter_without_waiting(
		struct pp_hwmgr *hwmgr, uint16_t msg, uint32_t parameter)
{
	uint32_t reg;

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_82_BASE_IDX, mmMP1_SMN_C2PMSG_82);
	cgs_write_register(hwmgr->device, reg, parameter);

	return vega10_send_msg_to_smc_without_waiting(hwmgr, msg);
}

/*
 * Retrieve an argument from SMC.
 * @param    hwmgr  the address of the powerplay hardware manager.
 * @param    arg     pointer to store the argument from SMC.
 * @return   Always return 0.
 */
int vega10_read_arg_from_smc(struct pp_hwmgr *hwmgr, uint32_t *arg)
{
	uint32_t reg;

	reg = soc15_get_register_offset(MP1_HWID, 0,
			mmMP1_SMN_C2PMSG_82_BASE_IDX, mmMP1_SMN_C2PMSG_82);

	*arg = cgs_read_register(hwmgr->device, reg);

	return 0;
}

/*
 * Copy table from SMC into driver FB
 * @param   hwmgr    the address of the HW manager
 * @param   table_id    the driver's table ID to copy from
 */
int vega10_copy_table_from_smc(struct pp_hwmgr *hwmgr,
		uint8_t *table, int16_t table_id)
{
	struct vega10_smumgr *priv =
			(struct vega10_smumgr *)(hwmgr->smu_backend);

	PP_ASSERT_WITH_CODE(table_id < MAX_SMU_TABLE,
			"Invalid SMU Table ID!", return -EINVAL);
	PP_ASSERT_WITH_CODE(priv->smu_tables.entry[table_id].version != 0,
			"Invalid SMU Table version!", return -EINVAL);
	PP_ASSERT_WITH_CODE(priv->smu_tables.entry[table_id].size != 0,
			"Invalid SMU Table Length!", return -EINVAL);
	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetDriverDramAddrHigh,
			upper_32_bits(priv->smu_tables.entry[table_id].mc_addr));
	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetDriverDramAddrLow,
			lower_32_bits(priv->smu_tables.entry[table_id].mc_addr));
	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_TransferTableSmu2Dram,
			priv->smu_tables.entry[table_id].table_id);

	memcpy(table, priv->smu_tables.entry[table_id].table,
			priv->smu_tables.entry[table_id].size);

	return 0;
}

/*
 * Copy table from Driver FB into SMC
 * @param   hwmgr    the address of the HW manager
 * @param   table_id    the table to copy from
 */
int vega10_copy_table_to_smc(struct pp_hwmgr *hwmgr,
		uint8_t *table, int16_t table_id)
{
	struct vega10_smumgr *priv =
			(struct vega10_smumgr *)(hwmgr->smu_backend);

	PP_ASSERT_WITH_CODE(table_id < MAX_SMU_TABLE,
			"Invalid SMU Table ID!", return -EINVAL);
	PP_ASSERT_WITH_CODE(priv->smu_tables.entry[table_id].version != 0,
			"Invalid SMU Table version!", return -EINVAL);
	PP_ASSERT_WITH_CODE(priv->smu_tables.entry[table_id].size != 0,
			"Invalid SMU Table Length!", return -EINVAL);

	memcpy(priv->smu_tables.entry[table_id].table, table,
			priv->smu_tables.entry[table_id].size);

	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetDriverDramAddrHigh,
			upper_32_bits(priv->smu_tables.entry[table_id].mc_addr));
	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetDriverDramAddrLow,
			lower_32_bits(priv->smu_tables.entry[table_id].mc_addr));
	vega10_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_TransferTableDram2Smu,
			priv->smu_tables.entry[table_id].table_id);

	return 0;
}

int vega10_save_vft_table(struct pp_hwmgr *hwmgr, uint8_t *avfs_table)
{
	PP_ASSERT_WITH_CODE(avfs_table,
			"No access to SMC AVFS Table",
			return -EINVAL);

	return vega10_copy_table_from_smc(hwmgr, avfs_table, AVFSTABLE);
}

int vega10_restore_vft_table(struct pp_hwmgr *hwmgr, uint8_t *avfs_table)
{
	PP_ASSERT_WITH_CODE(avfs_table,
			"No access to SMC AVFS Table",
			return -EINVAL);

	return vega10_copy_table_to_smc(hwmgr, avfs_table, AVFSTABLE);
}

int vega10_enable_smc_features(struct pp_hwmgr *hwmgr,
		bool enable, uint32_t feature_mask)
{
	int msg = enable ? PPSMC_MSG_EnableSmuFeatures :
			PPSMC_MSG_DisableSmuFeatures;

	return vega10_send_msg_to_smc_with_parameter(hwmgr,
			msg, feature_mask);
}

int vega10_get_smc_features(struct pp_hwmgr *hwmgr,
		uint32_t *features_enabled)
{
	if (features_enabled == NULL)
		return -EINVAL;

	vega10_send_msg_to_smc(hwmgr, PPSMC_MSG_GetEnabledSmuFeatures);
	vega10_read_arg_from_smc(hwmgr, features_enabled);
	return 0;
}

static bool vega10_is_dpm_running(struct pp_hwmgr *hwmgr)
{
	uint32_t features_enabled = 0;

	vega10_get_smc_features(hwmgr, &features_enabled);

	if (features_enabled & SMC_DPM_FEATURES)
		return true;
	else
		return false;
}

int vega10_set_tools_address(struct pp_hwmgr *hwmgr)
{
	struct vega10_smumgr *priv =
			(struct vega10_smumgr *)(hwmgr->smu_backend);

	if (priv->smu_tables.entry[TOOLSTABLE].mc_addr) {
		vega10_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetToolsDramAddrHigh,
				upper_32_bits(priv->smu_tables.entry[TOOLSTABLE].mc_addr));
		vega10_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetToolsDramAddrLow,
				lower_32_bits(priv->smu_tables.entry[TOOLSTABLE].mc_addr));
	}
	return 0;
}

static int vega10_verify_smc_interface(struct pp_hwmgr *hwmgr)
{
	uint32_t smc_driver_if_version;
	struct amdgpu_device *adev = hwmgr->adev;
	uint32_t dev_id;
	uint32_t rev_id;

	PP_ASSERT_WITH_CODE(!vega10_send_msg_to_smc(hwmgr,
			PPSMC_MSG_GetDriverIfVersion),
			"Attempt to get SMC IF Version Number Failed!",
			return -EINVAL);
	vega10_read_arg_from_smc(hwmgr, &smc_driver_if_version);

	dev_id = adev->pdev->device;
	rev_id = adev->pdev->revision;

	if (!((dev_id == 0x687f) &&
		((rev_id == 0xc0) ||
		(rev_id == 0xc1) ||
		(rev_id == 0xc3)))) {
		if (smc_driver_if_version != SMU9_DRIVER_IF_VERSION) {
			pr_err("Your firmware(0x%x) doesn't match SMU9_DRIVER_IF_VERSION(0x%x). Please update your firmware!\n",
			       smc_driver_if_version, SMU9_DRIVER_IF_VERSION);
			return -EINVAL;
		}
	}

	return 0;
}

static int vega10_smu_init(struct pp_hwmgr *hwmgr)
{
	struct amdgpu_bo *handle = NULL;
	struct vega10_smumgr *priv;
	uint64_t mc_addr;
	void *kaddr = NULL;
	unsigned long tools_size;
	int ret;
	struct cgs_firmware_info info = {0};

	ret = cgs_get_firmware_info(hwmgr->device,
				    smu7_convert_fw_type_to_cgs(UCODE_ID_SMU),
				    &info);
	if (ret || !info.kptr)
		return -EINVAL;

	priv = kzalloc(sizeof(struct vega10_smumgr), GFP_KERNEL);

	if (!priv)
		return -ENOMEM;

	hwmgr->smu_backend = priv;

	/* allocate space for pptable */
	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
			sizeof(PPTable_t),
			PAGE_SIZE,
			AMDGPU_GEM_DOMAIN_VRAM,
			&handle,
			&mc_addr,
			&kaddr);

	if (ret)
		return -EINVAL;

	priv->smu_tables.entry[PPTABLE].version = 0x01;
	priv->smu_tables.entry[PPTABLE].size = sizeof(PPTable_t);
	priv->smu_tables.entry[PPTABLE].table_id = TABLE_PPTABLE;
	priv->smu_tables.entry[PPTABLE].mc_addr = mc_addr;
	priv->smu_tables.entry[PPTABLE].table = kaddr;
	priv->smu_tables.entry[PPTABLE].handle = handle;

	/* allocate space for watermarks table */
	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
			sizeof(Watermarks_t),
			PAGE_SIZE,
			AMDGPU_GEM_DOMAIN_VRAM,
			&handle,
			&mc_addr,
			&kaddr);

	if (ret)
		goto err0;

	priv->smu_tables.entry[WMTABLE].version = 0x01;
	priv->smu_tables.entry[WMTABLE].size = sizeof(Watermarks_t);
	priv->smu_tables.entry[WMTABLE].table_id = TABLE_WATERMARKS;
	priv->smu_tables.entry[WMTABLE].mc_addr = mc_addr;
	priv->smu_tables.entry[WMTABLE].table = kaddr;
	priv->smu_tables.entry[WMTABLE].handle = handle;

	/* allocate space for AVFS table */
	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
			sizeof(AvfsTable_t),
			PAGE_SIZE,
			AMDGPU_GEM_DOMAIN_VRAM,
			&handle,
			&mc_addr,
			&kaddr);

	if (ret)
		goto err1;

	priv->smu_tables.entry[AVFSTABLE].version = 0x01;
	priv->smu_tables.entry[AVFSTABLE].size = sizeof(AvfsTable_t);
	priv->smu_tables.entry[AVFSTABLE].table_id = TABLE_AVFS;
	priv->smu_tables.entry[AVFSTABLE].mc_addr = mc_addr;
	priv->smu_tables.entry[AVFSTABLE].table = kaddr;
	priv->smu_tables.entry[AVFSTABLE].handle = handle;

	tools_size = 0x19000;
	if (tools_size) {
		ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
				tools_size,
				PAGE_SIZE,
				AMDGPU_GEM_DOMAIN_VRAM,
				&handle,
				&mc_addr,
				&kaddr);
		if (ret)
			goto err2;
		priv->smu_tables.entry[TOOLSTABLE].version = 0x01;
		priv->smu_tables.entry[TOOLSTABLE].size = tools_size;
		priv->smu_tables.entry[TOOLSTABLE].table_id = TABLE_PMSTATUSLOG;
		priv->smu_tables.entry[TOOLSTABLE].mc_addr = mc_addr;
		priv->smu_tables.entry[TOOLSTABLE].table = kaddr;
		priv->smu_tables.entry[TOOLSTABLE].handle = handle;
	}

	/* allocate space for AVFS Fuse table */
	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
			sizeof(AvfsFuseOverride_t),
			PAGE_SIZE,
			AMDGPU_GEM_DOMAIN_VRAM,
			&handle,
			&mc_addr,
			&kaddr);
	if (ret)
		goto err3;

	priv->smu_tables.entry[AVFSFUSETABLE].version = 0x01;
	priv->smu_tables.entry[AVFSFUSETABLE].size = sizeof(AvfsFuseOverride_t);
	priv->smu_tables.entry[AVFSFUSETABLE].table_id = TABLE_AVFS_FUSE_OVERRIDE;
	priv->smu_tables.entry[AVFSFUSETABLE].mc_addr = mc_addr;
	priv->smu_tables.entry[AVFSFUSETABLE].table = kaddr;
	priv->smu_tables.entry[AVFSFUSETABLE].handle = handle;

	return 0;

err3:
	if (priv->smu_tables.entry[TOOLSTABLE].table)
		amdgpu_bo_free_kernel(&priv->smu_tables.entry[TOOLSTABLE].handle,
				&priv->smu_tables.entry[TOOLSTABLE].mc_addr,
				&priv->smu_tables.entry[TOOLSTABLE].table);
err2:
	amdgpu_bo_free_kernel(&priv->smu_tables.entry[AVFSTABLE].handle,
				&priv->smu_tables.entry[AVFSTABLE].mc_addr,
				&priv->smu_tables.entry[AVFSTABLE].table);
err1:
	amdgpu_bo_free_kernel(&priv->smu_tables.entry[WMTABLE].handle,
				&priv->smu_tables.entry[WMTABLE].mc_addr,
				&priv->smu_tables.entry[WMTABLE].table);
err0:
	amdgpu_bo_free_kernel(&priv->smu_tables.entry[PPTABLE].handle,
			&priv->smu_tables.entry[PPTABLE].mc_addr,
			&priv->smu_tables.entry[PPTABLE].table);
	return -EINVAL;
}

static int vega10_smu_fini(struct pp_hwmgr *hwmgr)
{
	struct vega10_smumgr *priv =
			(struct vega10_smumgr *)(hwmgr->smu_backend);

	if (priv) {
		amdgpu_bo_free_kernel(&priv->smu_tables.entry[PPTABLE].handle,
				&priv->smu_tables.entry[PPTABLE].mc_addr,
				&priv->smu_tables.entry[PPTABLE].table);
		amdgpu_bo_free_kernel(&priv->smu_tables.entry[WMTABLE].handle,
					&priv->smu_tables.entry[WMTABLE].mc_addr,
					&priv->smu_tables.entry[WMTABLE].table);
		amdgpu_bo_free_kernel(&priv->smu_tables.entry[AVFSTABLE].handle,
					&priv->smu_tables.entry[AVFSTABLE].mc_addr,
					&priv->smu_tables.entry[AVFSTABLE].table);
		if (priv->smu_tables.entry[TOOLSTABLE].table)
			amdgpu_bo_free_kernel(&priv->smu_tables.entry[TOOLSTABLE].handle,
					&priv->smu_tables.entry[TOOLSTABLE].mc_addr,
					&priv->smu_tables.entry[TOOLSTABLE].table);
		amdgpu_bo_free_kernel(&priv->smu_tables.entry[AVFSFUSETABLE].handle,
					&priv->smu_tables.entry[AVFSFUSETABLE].mc_addr,
					&priv->smu_tables.entry[AVFSFUSETABLE].table);
		kfree(hwmgr->smu_backend);
		hwmgr->smu_backend = NULL;
	}
	return 0;
}

static int vega10_start_smu(struct pp_hwmgr *hwmgr)
{
	if (!vega10_is_smc_ram_running(hwmgr))
		return -EINVAL;

	PP_ASSERT_WITH_CODE(!vega10_verify_smc_interface(hwmgr),
			"Failed to verify SMC interface!",
			return -EINVAL);

	vega10_set_tools_address(hwmgr);

	return 0;
}

const struct pp_smumgr_func vega10_smu_funcs = {
	.smu_init = &vega10_smu_init,
	.smu_fini = &vega10_smu_fini,
	.start_smu = &vega10_start_smu,
	.request_smu_load_specific_fw = NULL,
	.send_msg_to_smc = &vega10_send_msg_to_smc,
	.send_msg_to_smc_with_parameter = &vega10_send_msg_to_smc_with_parameter,
	.download_pptable_settings = NULL,
	.upload_pptable_settings = NULL,
	.is_dpm_running = vega10_is_dpm_running,
};
