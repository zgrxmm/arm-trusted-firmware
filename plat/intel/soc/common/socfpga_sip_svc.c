/*
 * Copyright (c) 2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <tools_share/uuid.h>

#include "socfpga_mailbox.h"
#include "socfpga_sip_svc.h"

/* Number of SiP Calls implemented */
#define SIP_NUM_CALLS		0x3

/* Total buffer the driver can hold */
#define FPGA_CONFIG_BUFFER_SIZE 4

static int current_block;
static int read_block;
static int current_buffer;
static int send_id;
static int rcv_id;
static int max_blocks;
static uint32_t bytes_per_block;
static uint32_t blocks_submitted;

struct fpga_config_info {
	uint32_t addr;
	int size;
	int size_written;
	uint32_t write_requested;
	int subblocks_sent;
	int block_number;
};

/*  SiP Service UUID */
DEFINE_SVC_UUID2(intl_svc_uid,
		0xa85273b0, 0xe85a, 0x4862, 0xa6, 0x2a,
		0xfa, 0x88, 0x88, 0x17, 0x68, 0x81);

uint64_t socfpga_sip_handler(uint32_t smc_fid,
				   uint64_t x1,
				   uint64_t x2,
				   uint64_t x3,
				   uint64_t x4,
				   void *cookie,
				   void *handle,
				   uint64_t flags)
{
	ERROR("%s: unhandled SMC (0x%x)\n", __func__, smc_fid);
	SMC_RET1(handle, SMC_UNK);
}

struct fpga_config_info fpga_config_buffers[FPGA_CONFIG_BUFFER_SIZE];

static int intel_fpga_sdm_write_buffer(struct fpga_config_info *buffer)
{
	uint32_t args[3];

	while (max_blocks > 0 && buffer->size > buffer->size_written) {
		args[0] = (1<<8);
		args[1] = buffer->addr + buffer->size_written;
		if (buffer->size - buffer->size_written <= bytes_per_block) {
			args[2] = buffer->size - buffer->size_written;
			current_buffer++;
			current_buffer %= FPGA_CONFIG_BUFFER_SIZE;
		} else
			args[2] = bytes_per_block;

		buffer->size_written += args[2];
		mailbox_send_cmd_async(
			send_id++ % MBOX_MAX_JOB_ID,
			MBOX_RECONFIG_DATA,
			args, 3, 0);

		buffer->subblocks_sent++;
		max_blocks--;
	}

	return !max_blocks;
}

static int intel_fpga_sdm_write_all(void)
{
	for (int i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++)
		if (intel_fpga_sdm_write_buffer(
			&fpga_config_buffers[current_buffer]))
			break;
	return 0;
}

uint32_t intel_mailbox_fpga_config_isdone(void)
{
	uint32_t ret = intel_mailbox_get_config_status(MBOX_RECONFIG_STATUS);

	if (ret) {
		if (ret == MBOX_CFGSTAT_STATE_CONFIG)
			return INTEL_SIP_SMC_STATUS_BUSY;
		else
			return INTEL_SIP_SMC_STATUS_ERROR;
	}

	return INTEL_SIP_SMC_STATUS_OK;
}

static int mark_last_buffer_xfer_completed(uint32_t *buffer_addr_completed)
{
	int i;

	for (i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++) {
		if (fpga_config_buffers[i].block_number == current_block) {
			fpga_config_buffers[i].subblocks_sent--;
			if (fpga_config_buffers[i].subblocks_sent == 0
			&& fpga_config_buffers[i].size <=
			fpga_config_buffers[i].size_written) {
				fpga_config_buffers[i].write_requested = 0;
				current_block++;
				*buffer_addr_completed =
					fpga_config_buffers[i].addr;
				return 0;
			}
		}
	}

	return -1;
}

int intel_fpga_config_completed_write(uint32_t *completed_addr,
					uint32_t *count)
{
	uint32_t status = INTEL_SIP_SMC_STATUS_OK;
	*count = 0;
	int resp_len = 0;
	uint32_t resp[5];
	int all_completed = 1;

	while (*count < 3) {

		resp_len = mailbox_read_response(rcv_id % MBOX_MAX_JOB_ID,
				resp, sizeof(resp) / sizeof(resp[0]));

		if (resp_len < 0)
			break;

		max_blocks++;
		rcv_id++;

		if (mark_last_buffer_xfer_completed(
			&completed_addr[*count]) == 0)
			*count = *count + 1;
		else
			break;
	}

	if (*count <= 0) {
		if (resp_len != MBOX_NO_RESPONSE &&
			resp_len != MBOX_TIMEOUT && resp_len != 0) {
			mailbox_clear_response();
			return INTEL_SIP_SMC_STATUS_ERROR;
		}

		*count = 0;
	}

	intel_fpga_sdm_write_all();

	if (*count > 0)
		status = INTEL_SIP_SMC_STATUS_OK;
	else if (*count == 0)
		status = INTEL_SIP_SMC_STATUS_BUSY;

	for (int i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++) {
		if (fpga_config_buffers[i].write_requested != 0) {
			all_completed = 0;
			break;
		}
	}

	if (all_completed == 1)
		return INTEL_SIP_SMC_STATUS_OK;

	return status;
}

int intel_fpga_config_start(uint32_t config_type)
{
	uint32_t response[3];
	int status = 0;

	mailbox_clear_response();

	mailbox_send_cmd(1, MBOX_CMD_CANCEL, 0, 0, 0, NULL, 0);

	status = mailbox_send_cmd(1, MBOX_RECONFIG, 0, 0, 0,
			response, sizeof(response) / sizeof(response[0]));

	if (status < 0)
		return status;

	max_blocks = response[0];
	bytes_per_block = response[1];

	for (int i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++) {
		fpga_config_buffers[i].size = 0;
		fpga_config_buffers[i].size_written = 0;
		fpga_config_buffers[i].addr = 0;
		fpga_config_buffers[i].write_requested = 0;
		fpga_config_buffers[i].block_number = 0;
		fpga_config_buffers[i].subblocks_sent = 0;
	}

	blocks_submitted = 0;
	current_block = 0;
	read_block = 0;
	current_buffer = 0;
	send_id = 0;
	rcv_id = 0;

	return 0;
}

static bool is_fpga_config_buffer_full(void)
{
	for (int i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++)
		if (!fpga_config_buffers[i].write_requested)
			return false;
	return true;
}

static bool is_address_in_ddr_range(uint64_t addr)
{
	if (addr >= DRAM_BASE && addr <= DRAM_BASE + DRAM_SIZE)
		return true;

	return false;
}

uint32_t intel_fpga_config_write(uint64_t mem, uint64_t size)
{
	int i;

	intel_fpga_sdm_write_all();

	if (!is_address_in_ddr_range(mem) ||
		!is_address_in_ddr_range(mem + size) ||
		is_fpga_config_buffer_full())
		return INTEL_SIP_SMC_STATUS_REJECTED;

	for (i = 0; i < FPGA_CONFIG_BUFFER_SIZE; i++) {
		int j = (i + current_buffer) % FPGA_CONFIG_BUFFER_SIZE;

		if (!fpga_config_buffers[j].write_requested) {
			fpga_config_buffers[j].addr = mem;
			fpga_config_buffers[j].size = size;
			fpga_config_buffers[j].size_written = 0;
			fpga_config_buffers[j].write_requested = 1;
			fpga_config_buffers[j].block_number =
				blocks_submitted++;
			fpga_config_buffers[j].subblocks_sent = 0;
			break;
		}
	}

	if (is_fpga_config_buffer_full())
		return INTEL_SIP_SMC_STATUS_BUSY;

	return INTEL_SIP_SMC_STATUS_OK;
}

/*
 * This function is responsible for handling all SiP calls from the NS world
 */

uintptr_t sip_smc_handler(uint32_t smc_fid,
			 u_register_t x1,
			 u_register_t x2,
			 u_register_t x3,
			 u_register_t x4,
			 void *cookie,
			 void *handle,
			 u_register_t flags)
{
	uint32_t status = INTEL_SIP_SMC_STATUS_OK;
	uint32_t completed_addr[3];
	uint32_t count = 0;

	switch (smc_fid) {
	case SIP_SVC_UID:
		/* Return UID to the caller */
		SMC_UUID_RET(handle, intl_svc_uid);
		break;
	case INTEL_SIP_SMC_FPGA_CONFIG_ISDONE:
		status = intel_mailbox_fpga_config_isdone();
		SMC_RET4(handle, status, 0, 0, 0);
		break;
	case INTEL_SIP_SMC_FPGA_CONFIG_GET_MEM:
		SMC_RET3(handle, INTEL_SIP_SMC_STATUS_OK,
			INTEL_SIP_SMC_FPGA_CONFIG_ADDR,
			INTEL_SIP_SMC_FPGA_CONFIG_SIZE -
				INTEL_SIP_SMC_FPGA_CONFIG_ADDR);
		break;
	case INTEL_SIP_SMC_FPGA_CONFIG_START:
		status = intel_fpga_config_start(x1);
		SMC_RET4(handle, status, 0, 0, 0);
		break;
	case INTEL_SIP_SMC_FPGA_CONFIG_WRITE:
		status = intel_fpga_config_write(x1, x2);
		SMC_RET4(handle, status, 0, 0, 0);
		break;
	case INTEL_SIP_SMC_FPGA_CONFIG_COMPLETED_WRITE:
		status = intel_fpga_config_completed_write(completed_addr,
								&count);
		switch (count) {
		case 1:
			SMC_RET4(handle, INTEL_SIP_SMC_STATUS_OK,
				completed_addr[0], 0, 0);
			break;
		case 2:
			SMC_RET4(handle, INTEL_SIP_SMC_STATUS_OK,
				completed_addr[0],
				completed_addr[1], 0);
			break;
		case 3:
			SMC_RET4(handle, INTEL_SIP_SMC_STATUS_OK,
				completed_addr[0],
				completed_addr[1],
				completed_addr[2]);
			break;
		case 0:
			SMC_RET4(handle, status, 0, 0, 0);
			break;
		default:
			mailbox_clear_response();
			SMC_RET1(handle, INTEL_SIP_SMC_STATUS_ERROR);
		}
		break;

	default:
		return socfpga_sip_handler(smc_fid, x1, x2, x3, x4,
			cookie, handle, flags);
	}
}

DECLARE_RT_SVC(
	socfpga_sip_svc,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	NULL,
	sip_smc_handler
);

DECLARE_RT_SVC(
	socfpga_sip_svc_std,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_YIELD,
	NULL,
	sip_smc_handler
);
