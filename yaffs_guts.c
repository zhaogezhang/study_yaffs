/*
 * YAFFS: Yet Another Flash File System. A NAND-flash specific file system.
 *
 * Copyright (C) 2002-2018 Aleph One Ltd.
 *
 * Created by Charles Manning <charles@aleph1.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "yportenv.h"
#include "yaffs_trace.h"

#include "yaffs_guts.h"
#include "yaffs_endian.h"
#include "yaffs_getblockinfo.h"
#include "yaffs_tagscompat.h"
#include "yaffs_tagsmarshall.h"
#include "yaffs_nand.h"
#include "yaffs_yaffs1.h"
#include "yaffs_yaffs2.h"
#include "yaffs_bitmap.h"
#include "yaffs_verify.h"
#include "yaffs_nand.h"
#include "yaffs_packedtags2.h"
#include "yaffs_nameval.h"
#include "yaffs_allocator.h"
#include "yaffs_attribs.h"
#include "yaffs_summary.h"

/* Note YAFFS_GC_GOOD_ENOUGH must be <= YAFFS_GC_PASSIVE_THRESHOLD */
#define YAFFS_GC_GOOD_ENOUGH 2
#define YAFFS_GC_PASSIVE_THRESHOLD 4

#include "yaffs_ecc.h"

/* Forward declarations */

static int yaffs_wr_data_obj(struct yaffs_obj *in, int inode_chunk,
			     const u8 *buffer, int n_bytes, int use_reserve);

static void yaffs_fix_null_name(struct yaffs_obj *obj, YCHAR *name,
				int buffer_size);

/* Function to calculate chunk and offset */

/*********************************************************************************************************
** 函数名称: yaffs_addr_to_chunk
** 功能描述: 把文件内的地址偏移量转换成逻辑 chunk(inode_chunk) 以及在逻辑 chunk 内的偏移
** 输     入: dev - yaffs 设备
**         : addr - 文件内偏移地址，从零开始
** 输     出: chunk_out - addr 对应文件的 inode_chunk 值
**         : offset_out - addr 在文件第 inode_chunk 内的偏移量
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_addr_to_chunk(struct yaffs_dev *dev, loff_t addr,
				int *chunk_out, u32 *offset_out)
{
	int chunk;
	u32 offset;

	chunk = (u32) (addr >> dev->chunk_shift);

	if (dev->chunk_div == 1) {
		/* easy power of 2 case */
		offset = (u32) (addr & dev->chunk_mask);
	} else {
		/* Non power-of-2 case */

		loff_t chunk_base;

		chunk /= dev->chunk_div;

		chunk_base = ((loff_t) chunk) * dev->data_bytes_per_chunk;
		offset = (u32) (addr - chunk_base);
	}

	*chunk_out = chunk;
	*offset_out = offset;
}

/* Function to return the number of shifts for a power of 2 greater than or
 * equal to the given number
 * Note we don't try to cater for all possible numbers and this does not have to
 * be hellishly efficient.
 */

/*********************************************************************************************************
** 函数名称: calc_shifts_ceiling
** 功能描述: 计算出当前设备最大 nand_chunk 最少需要多少 bit 表示
** 输	 入: x - 当前设备会出现的最大 nand_chunk 值
** 输     出: shifts - 表示 x 需要的最少 bit 数
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static inline u32 calc_shifts_ceiling(u32 x)
{
	int extra_bits;
	int shifts;

	shifts = extra_bits = 0;

	while (x > 1) {
		if (x & 1)
			extra_bits++;
		x >>= 1;
		shifts++;
	}

	if (extra_bits)
		shifts++;

	return shifts;
}

/* Function to return the number of shifts to get a 1 in bit 0
 */

/*********************************************************************************************************
** 函数名称: calc_shifts
** 功能描述: 计算每个 nand_chunk 存储的数据字节数、bit0 向高位开始一共有多少个连续的 0，例如每个
**         : nand_chunk 存储了 10100000 个字节数据，那么计算的返回值是 5
** 输	 入: x - 每个 nand_chunk 存储的数据字节数
** 输     出: shifts - 每个 nand_chunk 存储数据字节数值在低位连续 0 的个数
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static inline u32 calc_shifts(u32 x)
{
	u32 shifts;

	shifts = 0;

	if (!x)
		return 0;

	while (!(x & 1)) {
		x >>= 1;
		shifts++;
	}

	return shifts;
}

/*
 * Temporary buffer manipulations.
 */

/*********************************************************************************************************
** 函数名称: yaffs_init_tmp_buffers
** 功能描述: 初始化 yaffs 设备临时数据缓冲区，每个缓冲空间可存储一个 chunk 数据，默认申请 6 个缓冲空间
** 输	 入: dev - 需要初始化临时数据缓冲区的 yaffs 设备
** 输     出: YAFFS_OK - 初始化成功
**         : YAFFS_FAIL - 初始化失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_init_tmp_buffers(struct yaffs_dev *dev)
{
	int i;
	u8 *buf = (u8 *) 1;

	memset(dev->temp_buffer, 0, sizeof(dev->temp_buffer));

	for (i = 0; buf && i < YAFFS_N_TEMP_BUFFERS; i++) {
		dev->temp_buffer[i].in_use = 0;
		buf = kmalloc(dev->param.total_bytes_per_chunk, GFP_NOFS);
		dev->temp_buffer[i].buffer = buf;
	}

	return buf ? YAFFS_OK : YAFFS_FAIL;
}

/*********************************************************************************************************
** 函数名称: yaffs_get_temp_buffer
** 功能描述: 获取一个 yaffs 设备临时数据缓冲区空间，如果没有可用缓冲区，则重新创建一个
** 输	 入: dev - yaffs 设备
** 输     出: 申请到的临时数据缓冲区空间地址
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
u8 *yaffs_get_temp_buffer(struct yaffs_dev * dev)
{
	int i;

	dev->temp_in_use++;
	if (dev->temp_in_use > dev->max_temp)
		dev->max_temp = dev->temp_in_use;

	for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
		if (dev->temp_buffer[i].in_use == 0) {
			dev->temp_buffer[i].in_use = 1;
			return dev->temp_buffer[i].buffer;
		}
	}

	yaffs_trace(YAFFS_TRACE_BUFFERS, "Out of temp buffers");
	/*
	 * If we got here then we have to allocate an unmanaged one
	 * This is not good.
	 */

	dev->unmanaged_buffer_allocs++;
	return kmalloc(dev->data_bytes_per_chunk, GFP_NOFS);

}

/*********************************************************************************************************
** 函数名称: yaffs_release_temp_buffer
** 功能描述: 释放一个 yaffs 设备临时数据缓冲区空间，如果需要释放的缓冲区在设备默认缓冲区中，则通过清除标志
**         : 变量回收即可，如果需要释放的缓冲区是通过 kmalloc 额外申请的，则需要通过 kfree 释放
** 输	 入: dev - yaffs 设备
** 输     出: 需要释放的临时数据缓冲区空间地址
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_release_temp_buffer(struct yaffs_dev *dev, u8 *buffer)
{
	int i;

	dev->temp_in_use--;

	for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
		if (dev->temp_buffer[i].buffer == buffer) {
			dev->temp_buffer[i].in_use = 0;
			return;
		}
	}

	if (buffer) {
		/* assume it is an unmanaged one. */
		yaffs_trace(YAFFS_TRACE_BUFFERS,
			"Releasing unmanaged temp buffer");
		kfree(buffer);
		dev->unmanaged_buffer_deallocs++;
	}

}

/*
 * Functions for robustisizing TODO
 *
 */

/*********************************************************************************************************
** 函数名称: yaffs_handle_chunk_wr_ok
** 功能描述: 在 nand_chunk 成功时调用的钩子函数
** 输	 入: dev - yaffs 设备
**         : nand_chunk - 物理空间上的 chunk 值
**         : data - 需要写入 nand_chunk 的数据
**         : tags - 需要写入 nand_chunk 的 tag 信息
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_handle_chunk_wr_ok(struct yaffs_dev *dev, int nand_chunk,
				     const u8 *data,
				     const struct yaffs_ext_tags *tags)
{
	(void) dev;
	(void) nand_chunk;
	(void) data;
	(void) tags;
}

/*********************************************************************************************************
** 函数名称: yaffs_handle_chunk_update
** 功能描述: 在更新 nand_chunk 成功时调用的钩子函数
** 输     入: dev - yaffs 设备
** 		   : nand_chunk - 物理空间上的 chunk 值
** 		   : tags - 需要写入 nand_chunk 的 tag 信息
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_handle_chunk_update(struct yaffs_dev *dev, int nand_chunk,
				      const struct yaffs_ext_tags *tags)
{
	(void) dev;
	(void) nand_chunk;
	(void) tags;
}

/*********************************************************************************************************
** 函数名称: yaffs_handle_chunk_error
** 功能描述: 在读写 nand_chunk 出现错误时需要调用的钩子函数
** 输	 入: dev - yaffs 设备
**		   : bi - 读写失败的 nand_chunk 所在块的 block info
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_handle_chunk_error(struct yaffs_dev *dev,
			      struct yaffs_block_info *bi)
{
	if (!bi->gc_prioritise) {
		bi->gc_prioritise = 1;
		dev->has_pending_prioritised_gc = 1;
		bi->chunk_error_strikes++;

		if (bi->chunk_error_strikes > 3) {
			bi->needs_retiring = 1;	/* Too many stikes, so retire */
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"yaffs: Block struck out");

		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_handle_chunk_wr_error
** 功能描述: 在写 nand_chunk 出现错误时需要调用的钩子函数
** 输     入: dev - yaffs 设备
** 		   : nand_chunk - 物理空间上的 chunk 值
**		   : erased_ok - nand_chunk 擦除是否成功
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_handle_chunk_wr_error(struct yaffs_dev *dev, int nand_chunk,
					int erased_ok)
{
	int flash_block = nand_chunk / dev->param.chunks_per_block;
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, flash_block);

	yaffs_handle_chunk_error(dev, bi);

	if (erased_ok) {
		/* Was an actual write failure,
		 * so mark the block for retirement.*/
		bi->needs_retiring = 1;
		yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
		  "**>> Block %d needs retiring", flash_block);
	}

	/* Delete the chunk */
	yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
	yaffs_skip_rest_of_block(dev);
}

/*
 * Verification code
 */

/*
 *  Simple hash function. Needs to have a reasonable spread
 */

/*********************************************************************************************************
** 函数名称: yaffs_hash_fn
** 功能描述: 计算 obj_id 对应的 obj_bucket 数组下标
** 输     入: n - obj_id
** 输	 出: obj_id 需要放在 obj_bucket 数组中的位置
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static inline int yaffs_hash_fn(int n)
{
	if (n < 0)
		n = -n;
	return n % YAFFS_NOBJECT_BUCKETS;
}

/*
 * Access functions to useful fake objects.
 * Note that root might have a presence in NAND if permissions are set.
 */

/*********************************************************************************************************
** 函数名称: yaffs_root
** 功能描述: 获取当前 yaffs 设备的根目录信息
** 输     入: dev - yaffs 设备
** 输	 出: 当前 yaffs 设备的根目录信息
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_root(struct yaffs_dev *dev)
{
	return dev->root_dir;
}

/*********************************************************************************************************
** 函数名称: yaffs_lost_n_found
** 功能描述: 获取当前 yaffs 设备的 lost_n_found 信息
** 输     入: dev - yaffs 设备
** 输	 出: 当前 yaffs 设备的 lost_n_found 信息
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_lost_n_found(struct yaffs_dev *dev)
{
	return dev->lost_n_found;
}

/*
 *  Erased NAND checking functions
 */

/*********************************************************************************************************
** 函数名称: yaffs_check_ff
** 功能描述: 校验输入缓冲区中的数据是否全是 0xFF
** 输     入: buffer - 需要校验的数据缓冲区
**         : n_bytes - 需要校验的数据长度
** 输	 出: 0 - 数据缓冲区不全是 0xFF
**         : 1 - 数据缓冲区全是 0xFF
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
int yaffs_check_ff(u8 *buffer, int n_bytes)
{
	/* Horrible, slow implementation */
	while (n_bytes--) {
		if (*buffer != 0xff)
			return 0;
		buffer++;
	}
	return 1;
}

/*********************************************************************************************************
** 函数名称: yaffs_check_chunk_erased
** 功能描述: 校验指定 nand_chunk 是否擦除成功，包括数据部分校验和 tags 中 ecc 校验
** 输     入: dev - yaffs 设备
**         : nand_chunk - 物理空间上的 chunk 值
** 输     出: YAFFS_OK - 校验成功
**         : YAFFS_FAIL - 校验失败
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_check_chunk_erased(struct yaffs_dev *dev, int nand_chunk)
{
	int retval = YAFFS_OK;
	u8 *data = yaffs_get_temp_buffer(dev);
	struct yaffs_ext_tags tags;
	int result;

	result = yaffs_rd_chunk_tags_nand(dev, nand_chunk, data, &tags);

	if (result == YAFFS_FAIL ||
	    tags.ecc_result > YAFFS_ECC_RESULT_NO_ERROR)
		retval = YAFFS_FAIL;

	if (!yaffs_check_ff(data, dev->data_bytes_per_chunk) ||
		tags.chunk_used) {
		yaffs_trace(YAFFS_TRACE_NANDACCESS,
			"Chunk %d not erased", nand_chunk);
		retval = YAFFS_FAIL;
	}

	yaffs_release_temp_buffer(dev, data);

	return retval;

}

/*********************************************************************************************************
** 函数名称: yaffs_verify_chunk_written
** 功能描述: 校验向指定 nand_chunk 的写入是否成功，包括数据部分校验和 tags 中 ecc 校验
** 输     入: dev - yaffs 设备
**         : nand_chunk - 物理空间上的 chunk 值
**         : data - 写入的数据内容
**         : tags - 写入的 tags 信息
** 输     出: YAFFS_OK - 校验成功
**         : YAFFS_FAIL - 校验失败
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_verify_chunk_written(struct yaffs_dev *dev,
				      int nand_chunk,
				      const u8 *data,
				      struct yaffs_ext_tags *tags)
{
	int retval = YAFFS_OK;
	struct yaffs_ext_tags temp_tags;
	u8 *buffer = yaffs_get_temp_buffer(dev);
	int result;

	result = yaffs_rd_chunk_tags_nand(dev, nand_chunk, buffer, &temp_tags);
	if (result == YAFFS_FAIL ||
	    memcmp(buffer, data, dev->data_bytes_per_chunk) ||
	    temp_tags.obj_id != tags->obj_id ||
	    temp_tags.chunk_id != tags->chunk_id ||
	    temp_tags.n_bytes != tags->n_bytes)
		retval = YAFFS_FAIL;

	yaffs_release_temp_buffer(dev, buffer);

	return retval;
}

/*********************************************************************************************************
** 函数名称: yaffs_check_alloc_available
** 功能描述: 检查当前设备是否可以申请指定个数的 nand_chunk 用于存储新数据的目的（checkpt_blocks 可以用来
**		   : 存储新数据）
** 输	 入: dev - yaffs 设备
**		   : n_chunks - 想要申请的 nand_chunk 个数
** 输	 出: 1 - 可以申请 n_chunks 个物理 chunk
**		   : 0 - 不可以申请
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
int yaffs_check_alloc_available(struct yaffs_dev *dev, int n_chunks)
{
	int reserved_chunks;
	int reserved_blocks = dev->param.n_reserved_blocks;
	int checkpt_blocks;

	checkpt_blocks = yaffs_calc_checkpt_blocks_required(dev);

	reserved_chunks =
	    (reserved_blocks + checkpt_blocks) * dev->param.chunks_per_block;

	return (dev->n_free_chunks > (reserved_chunks + n_chunks));
}

/*********************************************************************************************************
** 函数名称: yaffs_find_alloc_block
** 功能描述: 从当前设备找出一个空闲块用于存储新数据，同时把查找到的 nand_block 设置到
**		   : dev->alloc_block_finder 中并更新相关状态变量
** 输	 入: dev - yaffs 设备
** 输	 出: 查找到的 nand_block
**		   : -1 - 查找失败，没有可用的块
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_find_alloc_block(struct yaffs_dev *dev)
{
	u32 i;
	struct yaffs_block_info *bi;

	if (dev->n_erased_blocks < 1) {
		/* Hoosterman we've got a problem.
		 * Can't get space to gc
		 */
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "yaffs tragedy: no more erased blocks");

		return -1;
	}

	/* Find an empty block. */

	for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		dev->alloc_block_finder++;
		if (dev->alloc_block_finder < (int)dev->internal_start_block
		    || dev->alloc_block_finder > (int)dev->internal_end_block) {
			dev->alloc_block_finder = dev->internal_start_block;
		}

		bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

		if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY) {
			bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
			dev->seq_number++;
			bi->seq_number = dev->seq_number;
			dev->n_erased_blocks--;
			yaffs_trace(YAFFS_TRACE_ALLOCATE,
			  "Allocated block %d, seq  %d, %d left" ,
			   dev->alloc_block_finder, dev->seq_number,
			   dev->n_erased_blocks);
			return dev->alloc_block_finder;
		}
	}

	yaffs_trace(YAFFS_TRACE_ALWAYS,
		"yaffs tragedy: no more erased blocks, but there should have been %d",
		dev->n_erased_blocks);

	return -1;
}

/*********************************************************************************************************
** 函数名称: yaffs_alloc_chunk
** 功能描述: 从当前设备中申请一个可用的 nand_chunk 并更新相关状态变量
** 输	 入: dev - yaffs 设备
**		   : use_reserver - 是否可以使用 reserve 空间（reserve 空间用于垃圾回收）
** 输	 出: 查找到的 nand_chunk 值
**		   : block_ptr - nand_chunk 所在块的块信息
**		   : -1 - 查找失败，没有可用的块
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_alloc_chunk(struct yaffs_dev *dev, int use_reserver,
			     struct yaffs_block_info **block_ptr)
{
	int ret_val;
	struct yaffs_block_info *bi;

	if (dev->alloc_block < 0) {
		/* Get next block to allocate off */
		dev->alloc_block = yaffs_find_alloc_block(dev);
		dev->alloc_page = 0;
	}

	if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
		/* No space unless we're allowed to use the reserve. */
		return -1;
	}

	if (dev->n_erased_blocks < (int)dev->param.n_reserved_blocks
	    && dev->alloc_page == 0)
		yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

	/* Next page please.... */
	if (dev->alloc_block >= 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block);

		ret_val = (dev->alloc_block * dev->param.chunks_per_block) +
		    dev->alloc_page;
		bi->pages_in_use++;
		yaffs_set_chunk_bit(dev, dev->alloc_block, dev->alloc_page);

		dev->alloc_page++;

		dev->n_free_chunks--;

		/* If the block is full set the state to full */
		if (dev->alloc_page >= dev->param.chunks_per_block) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block = -1;
		}

		if (block_ptr)
			*block_ptr = bi;

		return ret_val;
	}

	yaffs_trace(YAFFS_TRACE_ERROR,
		"!!!!!!!!! Allocator out !!!!!!!!!!!!!!!!!");

	return -1;
}

/*********************************************************************************************************
** 函数名称: yaffs_get_erased_chunks
** 功能描述: 获取当前设备空闲 chunk 的个数
** 输     入: dev - yaffs 设备
** 输     出: 当前设备空闲 chunk 的个数
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_get_erased_chunks(struct yaffs_dev *dev)
{
	int n;

	n = dev->n_erased_blocks * dev->param.chunks_per_block;

	if (dev->alloc_block > 0)
		n += (dev->param.chunks_per_block - dev->alloc_page);

	return n;

}

/*
 * yaffs_skip_rest_of_block() skips over the rest of the allocation block
 * if we don't want to write to it.
 */

/*********************************************************************************************************
** 函数名称: yaffs_skip_rest_of_block
** 功能描述: 跳过设备当前申请块的余下 chunk 空间，如果当前申请块有效，修改其状态为“满”
** 输     入: dev - yaffs 设备
** 输     出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
void yaffs_skip_rest_of_block(struct yaffs_dev *dev)
{
	struct yaffs_block_info *bi;

	if (dev->alloc_block > 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block);
		if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block = -1;
		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_write_new_chunk
** 功能描述: 向设备中写入一个 chunk 的数据，在写入前会检查 chunk 是否为擦除状态，在写入后会检查写入是否成功
**         : 如果写入失败则尝试重新写入，重写次数为 yaffs_wr_attempts（并擦除 checkpt 数据）
**         : 1. 在写数据之前，会从设备中申请一个空闲 nand_chunk 空间用来存储数据
** 输     入: dev - yaffs 设备
**         : data - 需要写入的数据
**         : tags - 需要写入的 tags 信息
**         : use_reserver - 是否使用 reserver 空间
** 输     出: chunk - 成功写入数据的 nand_chunk 号
**         : -1 - 写入失败
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_write_new_chunk(struct yaffs_dev *dev,
				 const u8 *data,
				 struct yaffs_ext_tags *tags, int use_reserver)
{
	u32 attempts = 0;
	int write_ok = 0;
	int chunk;

	yaffs2_checkpt_invalidate(dev);

	do {
		struct yaffs_block_info *bi = 0;
		int erased_ok = 0;

		chunk = yaffs_alloc_chunk(dev, use_reserver, &bi);
		if (chunk < 0) {
			/* no space */
			break;
		}

		/* First check this chunk is erased, if it needs
		 * checking.  The checking policy (unless forced
		 * always on) is as follows:
		 *
		 * Check the first page we try to write in a block.
		 * If the check passes then we don't need to check any
		 * more.        If the check fails, we check again...
		 * If the block has been erased, we don't need to check.
		 *
		 * However, if the block has been prioritised for gc,
		 * then we think there might be something odd about
		 * this block and stop using it.
		 *
		 * Rationale: We should only ever see chunks that have
		 * not been erased if there was a partially written
		 * chunk due to power loss.  This checking policy should
		 * catch that case with very few checks and thus save a
		 * lot of checks that are most likely not needed.
		 *
		 * Mods to the above
		 * If an erase check fails or the write fails we skip the
		 * rest of the block.
		 */

		/* let's give it a try */
		attempts++;

		if (dev->param.always_check_erased)
			bi->skip_erased_check = 0;

		if (!bi->skip_erased_check) {
			erased_ok = yaffs_check_chunk_erased(dev, chunk);
			if (erased_ok != YAFFS_OK) {
				yaffs_trace(YAFFS_TRACE_ERROR,
				  "**>> yaffs chunk %d was not erased",
				  chunk);

				/* If not erased, delete this one,
				 * skip rest of block and
				 * try another chunk */
				yaffs_chunk_del(dev, chunk, 1, __LINE__);
				yaffs_skip_rest_of_block(dev);
				continue;
			}
		}

		write_ok = yaffs_wr_chunk_tags_nand(dev, chunk, data, tags);

		if (!bi->skip_erased_check)
			write_ok =
			    yaffs_verify_chunk_written(dev, chunk, data, tags);

		if (write_ok != YAFFS_OK) {
			/* Clean up aborted write, skip to next block and
			 * try another chunk */
			yaffs_handle_chunk_wr_error(dev, chunk, erased_ok);
			continue;
		}

		bi->skip_erased_check = 1;

		/* Copy the data into the robustification buffer */
		yaffs_handle_chunk_wr_ok(dev, chunk, data, tags);

	} while (write_ok != YAFFS_OK &&
		 (yaffs_wr_attempts == 0 || attempts <= yaffs_wr_attempts));

	if (!write_ok)
		chunk = -1;

	if (attempts > 1) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"**>> yaffs write required %d attempts",
			attempts);
		dev->n_retried_writes += (attempts - 1);
	}

	return chunk;
}

/*
 * Block retiring for handling a broken block.
 */

/*********************************************************************************************************
** 函数名称: yaffs_retire_block
** 功能描述: 把指定的块标志为坏块，并擦除 checkpt 数据、更新 oldest_dirty_seq，同时更新相关状态变量。
**         : 现在标志坏块有两种方式：
**         : 1. 通过 dev->tagger.mark_bad_fn 函数完成
**         : 2. 通过设置 tags.seq_number = YAFFS_SEQUENCE_BAD_BLOCK; 并设置其他 tags 为 0 来完成
** 输     入: dev - yaffs 设备
**         : flash_block - nand_block 块号
** 输     出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_retire_block(struct yaffs_dev *dev, int flash_block)
{
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, flash_block);

	yaffs2_checkpt_invalidate(dev);

	yaffs2_clear_oldest_dirty_seq(dev, bi);

	if (yaffs_mark_bad(dev, flash_block) != YAFFS_OK) {
		if (yaffs_erase_block(dev, flash_block) != YAFFS_OK) {
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"yaffs: Failed to mark bad and erase block %d",
				flash_block);
		} else {
			struct yaffs_ext_tags tags;
			int chunk_id =
			    flash_block * dev->param.chunks_per_block;

			u8 *buffer = yaffs_get_temp_buffer(dev);

			memset(buffer, 0xff, dev->data_bytes_per_chunk);
			memset(&tags, 0, sizeof(tags));
			tags.seq_number = YAFFS_SEQUENCE_BAD_BLOCK;
			if (dev->tagger.write_chunk_tags_fn(dev, chunk_id -
							dev->chunk_offset,
							buffer,
							&tags) != YAFFS_OK)
				yaffs_trace(YAFFS_TRACE_ALWAYS,
					"yaffs: Failed to write bad block marker to block %d",
					flash_block);

			yaffs_release_temp_buffer(dev, buffer);
		}
	}

	bi->block_state = YAFFS_BLOCK_STATE_DEAD;
	bi->gc_prioritise = 0;
	bi->needs_retiring = 0;

	dev->n_retired_blocks++;
}

/*---------------- Name handling functions ------------*/

/*********************************************************************************************************
** 函数名称: yaffs_load_name_from_oh
** 功能描述: 复制 obj header 中的名字到指定缓冲区（如果是ASCII name，则名字数组下标 0 位置的值为 0）
** 输     入: dev - yaffs 设备
**         : name - 保存 name 的缓冲区地址
**         : oh_name - obj header 中的名字
**         : buff_size - obj header 长度
** 输     出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_load_name_from_oh(struct yaffs_dev *dev, YCHAR *name,
				    const YCHAR *oh_name, int buff_size)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE
	if (dev->param.auto_unicode) {
		if (*oh_name) {
			/* It is an ASCII name, do an ASCII to
			 * unicode conversion */
			const char *ascii_oh_name = (const char *)oh_name;
			int n = buff_size - 1;
			while (n > 0 && *ascii_oh_name) {
				*name = *ascii_oh_name;
				name++;
				ascii_oh_name++;
				n--;
			}
		} else {
			strncpy(name, oh_name + 1, buff_size - 1);
		}
	} else {
#else
	(void) dev;
	{
#endif
		strncpy(name, oh_name, buff_size - 1);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_load_oh_from_name
** 功能描述: 复制指定名字到 obj header 中的名字字段中
** 输	 入: dev - yaffs 设备
**		   : oh_name - obj header 中的名字
**		   : name - 保存 name 的缓冲区地址
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_load_oh_from_name(struct yaffs_dev *dev, YCHAR *oh_name,
				    const YCHAR *name)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE

	int is_ascii;
	const YCHAR *w;

	if (dev->param.auto_unicode) {

		is_ascii = 1;
		w = name;

		/* Figure out if the name will fit in ascii character set */
		while (is_ascii && *w) {
			if ((*w) & 0xff00)
				is_ascii = 0;
			w++;
		}

		if (is_ascii) {
			/* It is an ASCII name, so convert unicode to ascii */
			char *ascii_oh_name = (char *)oh_name;
			int n = YAFFS_MAX_NAME_LENGTH - 1;
			while (n > 0 && *name) {
				*ascii_oh_name = *name;
				name++;
				ascii_oh_name++;
				n--;
			}
		} else {
			/* Unicode name, so save starting at the second YCHAR */
			*oh_name = 0;
			strncpy(oh_name + 1, name, YAFFS_MAX_NAME_LENGTH - 2);
		}
	} else {
#else
	dev = dev;
	{
#endif
		strncpy(oh_name, name, YAFFS_MAX_NAME_LENGTH - 1);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_calc_name_sum
** 功能描述: 计算名字的校验和，主要为了提高通过名字搜索的速度
** 输	 入: name - 需要计算校验和的名字
** 输	 出: sum - 计算结果
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static u16 yaffs_calc_name_sum(const YCHAR *name)
{
	u16 sum = 0;
	u16 i = 1;

	if (!name)
		return 0;

	while ((*name) && i < (YAFFS_MAX_NAME_LENGTH / 2)) {

		/* 0x1f mask is case insensitive */
		sum += ((*name) & 0x1f) * i;
		i++;
		name++;
	}
	return sum;
}

/*********************************************************************************************************
** 函数名称: yaffs_set_obj_name
** 功能描述: 设置 obj short_name 字段的值并更新名字校验和值，如果传入的参数为空，则自动生成个系统默认名
** 输	 入: obj - 想要设置名字的 object
** 输	 出: name - 想要设置的名字
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
void yaffs_set_obj_name(struct yaffs_obj *obj, const YCHAR * name)
{
	memset(obj->short_name, 0, sizeof(obj->short_name));

	if (name && !name[0]) {
		yaffs_fix_null_name(obj, obj->short_name,
				YAFFS_SHORT_NAME_LENGTH);
		name = obj->short_name;
	} else if (name &&
		strnlen(name, YAFFS_SHORT_NAME_LENGTH + 1) <=
		YAFFS_SHORT_NAME_LENGTH)  {
		strcpy(obj->short_name, name);
	}

	obj->sum = yaffs_calc_name_sum(name);
}

/*********************************************************************************************************
** 函数名称: yaffs_set_obj_name_from_oh
** 功能描述: 把输入 object header 的名字字段复制到输入 obj 的名字字段中
** 输	 入: obj - 想要设置名字的 object
** 输	 出: oh - 提供名字的 object header
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
void yaffs_set_obj_name_from_oh(struct yaffs_obj *obj,
				const struct yaffs_obj_hdr *oh)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE
	YCHAR tmp_name[YAFFS_MAX_NAME_LENGTH + 1];
	memset(tmp_name, 0, sizeof(tmp_name));
	yaffs_load_name_from_oh(obj->my_dev, tmp_name, oh->name,
				YAFFS_MAX_NAME_LENGTH + 1);
	yaffs_set_obj_name(obj, tmp_name);
#else
	yaffs_set_obj_name(obj, oh->name);
#endif
}

/*********************************************************************************************************
** 函数名称: yaffs_max_file_size
** 功能描述: 获取当前设备可允许的最大文件大小
** 输	 入: dev - yaffs 设备
** 输	 出: 当前设备可允许的最大文件大小
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
loff_t yaffs_max_file_size(struct yaffs_dev *dev)
{
	if (sizeof(loff_t) < 8)
		return YAFFS_MAX_FILE_SIZE_32;
	else
		return ((loff_t) YAFFS_MAX_CHUNK_ID) * dev->data_bytes_per_chunk;
}

/*-------------------- TNODES -------------------

 * List of spare tnodes
 * The list is hooked together using the first pointer
 * in the tnode.
 */

/*********************************************************************************************************
** 函数名称: yaffs_get_tnode
** 功能描述: 从当前设备的 allocator 中申请一个 tnode、清空其内容，并更新相关状态变量
** 输	 入: dev - yaffs 设备
** 输	 出: 申请到的 tnode
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
struct yaffs_tnode *yaffs_get_tnode(struct yaffs_dev *dev)
{
	struct yaffs_tnode *tn = yaffs_alloc_raw_tnode(dev);

	if (tn) {
		memset(tn, 0, dev->tnode_size);
		dev->n_tnodes++;
	}

	dev->checkpoint_blocks_required = 0;	/* force recalculation */

	return tn;
}

/* FreeTnode frees up a tnode and puts it back on the free list */

/*********************************************************************************************************
** 函数名称: yaffs_free_tnode
** 功能描述: 释放一个 tnode 到当前设备的 allocator 中，并更新相关状态变量
** 输	 入: dev - yaffs 设备
**         : 需要释放的 tnode
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_free_tnode(struct yaffs_dev *dev, struct yaffs_tnode *tn)
{
	yaffs_free_raw_tnode(dev, tn);
	dev->n_tnodes--;
	dev->checkpoint_blocks_required = 0;	/* force recalculation */
}

/*********************************************************************************************************
** 函数名称: yaffs_deinit_tnodes_and_objs
** 功能描述: 释放当前设备 tnodes 和 objs 占用的内存空间以及 allocator 占用的内存空间
** 输	 入: dev - yaffs 设备
**         :
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_deinit_tnodes_and_objs(struct yaffs_dev *dev)
{
	yaffs_deinit_raw_tnodes_and_objs(dev);
	dev->n_obj = 0;
	dev->n_tnodes = 0;
}

/*********************************************************************************************************
** 函数名称: yaffs_load_tnode_0
** 功能描述: 设置 level0 中指定 tnode 的值
** 输	 入: dev - yaffs 设备
**         : tn - 需要设置值的 tnode 地址
**         : pos - tnode 在 level0 中的位置
**         : val - 给 tnode 设置的新值
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_load_tnode_0(struct yaffs_dev *dev, struct yaffs_tnode *tn,
			unsigned pos, unsigned val)
{
	u32 *map = (u32 *) tn;
	u32 bit_in_map;
	u32 bit_in_word;
	u32 word_in_map;
	u32 mask;

	pos &= YAFFS_TNODES_LEVEL0_MASK;
	val >>= dev->chunk_grp_bits;

	bit_in_map = pos * dev->tnode_width;
	word_in_map = bit_in_map / 32;
	bit_in_word = bit_in_map & (32 - 1);

	mask = dev->tnode_mask << bit_in_word;

	map[word_in_map] &= ~mask;
	map[word_in_map] |= (mask & (val << bit_in_word));

	if (dev->tnode_width > (32 - bit_in_word)) {
		bit_in_word = (32 - bit_in_word);
		word_in_map++;
		mask =
		    dev->tnode_mask >> bit_in_word;
		map[word_in_map] &= ~mask;
		map[word_in_map] |= (mask & (val >> bit_in_word));
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_get_group_base
** 功能描述: 获取 level0 中指定 tnode 中的 nand_chunk 值
** 输	 入: dev - yaffs 设备
**		   : tn - 存储 nand_chunk 的 tnode 地址
**		   : pos - tnode 在 level0 中的位置
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
u32 yaffs_get_group_base(struct yaffs_dev *dev, struct yaffs_tnode *tn,
			 unsigned pos)
{
	u32 *map = (u32 *) tn;
	u32 bit_in_map;
	u32 bit_in_word;
	u32 word_in_map;
	u32 val;

	pos &= YAFFS_TNODES_LEVEL0_MASK;

	bit_in_map = pos * dev->tnode_width;
	word_in_map = bit_in_map / 32;
	bit_in_word = bit_in_map & (32 - 1);

	val = map[word_in_map] >> bit_in_word;

	if (dev->tnode_width > (32 - bit_in_word)) {
		bit_in_word = (32 - bit_in_word);
		word_in_map++;
		val |= (map[word_in_map] << bit_in_word);
	}

	val &= dev->tnode_mask;
	val <<= dev->chunk_grp_bits;

	return val;
}

/* ------------------- End of individual tnode manipulation -----------------*/

/* ---------Functions to manipulate the look-up tree (made up of tnodes) ------
 * The look up tree is represented by the top tnode and the number of top_level
 * in the tree. 0 means only the level 0 tnode is in the tree.
 */

/* FindLevel0Tnode finds the level 0 tnode, if one exists. */

/*********************************************************************************************************
** 函数名称: yaffs_find_tnode_0
** 功能描述:     查找 tnode tree   level0 中 inode_chunk 为 chunk_id 的 tnode 地址
** 输	 入: dev - yaffs 设备
**		   : file_struct - 需要查找的文件变量
**		   : chunk_id - 需要查找的 inode_chunk 值
** 输	 出: tn - 查找到的 tnode 地址
**		   : NULL - 查找的 tnode 不存在
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
struct yaffs_tnode *yaffs_find_tnode_0(struct yaffs_dev *dev,
				       struct yaffs_file_var *file_struct,
				       u32 chunk_id)
{
	struct yaffs_tnode *tn = file_struct->top;
	u32 i;
	int required_depth;
	int level = file_struct->top_level;

	(void) dev;

	/* Check sane level and chunk Id */
	if (level < 0 || level > YAFFS_TNODES_MAX_LEVEL)
		return NULL;

	if (chunk_id > YAFFS_MAX_CHUNK_ID)
		return NULL;

	/* First check we're tall enough (ie enough top_level) */

	i = chunk_id >> YAFFS_TNODES_LEVEL0_BITS;
	required_depth = 0;
	while (i) {
		i >>= YAFFS_TNODES_INTERNAL_BITS;
		required_depth++;
	}

	if (required_depth > file_struct->top_level)
		return NULL;	/* Not tall enough, so we can't find it */

	/* Traverse down to level 0 */
	while (level > 0 && tn) {
		tn = tn->internal[(chunk_id >>
				   (YAFFS_TNODES_LEVEL0_BITS +
				    (level - 1) *
				    YAFFS_TNODES_INTERNAL_BITS)) &
				  YAFFS_TNODES_INTERNAL_MASK];
		level--;
	}

	return tn;
}

/* add_find_tnode_0 finds the level 0 tnode if it exists,
 * otherwise first expands the tree.
 * This happens in two steps:
 *  1. If the tree isn't tall enough, then make it taller.
 *  2. Scan down the tree towards the level 0 tnode adding tnodes if required.
 *
 * Used when modifying the tree.
 *
 *  If the tn argument is NULL, then a fresh tnode will be added otherwise the
 *  specified tn will be plugged into the ttree.
 */

/*********************************************************************************************************
** 函数名称: yaffs_add_find_tnode_0
** 功能描述: 向 file_struct 文件 tnode tree 中 inode_chunk 为 chunk_id 的位置添加一个新的 tnode 节点，如果
**		   : 节点已经存在，则直接更新值，并释放传入的 passed_tn（passed_tn 不为 NULL，在 passed_tn 为 NULL
**		   : 的时候，无需更新目标节点的值）
** 输	 入: dev - yaffs 设备
**		   : file_struct - 需要查找的文件变量
**		   : chunk_id - 需要查找的 inode_chunk 值
**		   : passed_tn - 需要添加的 tnode 地址，如果为空，则申请一个空的 tnode 添加到树中
** 输	 出: tn - 查找到的 tnode 地址
**		   : NULL - 查找的 tnode 不存在
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
struct yaffs_tnode *yaffs_add_find_tnode_0(struct yaffs_dev *dev,
					   struct yaffs_file_var *file_struct,
					   u32 chunk_id,
					   struct yaffs_tnode *passed_tn)
{
	int required_depth;
	int i;
	int l;
	struct yaffs_tnode *tn;
	u32 x;

	/* Check sane level and page Id */
	if (file_struct->top_level < 0 ||
	    file_struct->top_level > YAFFS_TNODES_MAX_LEVEL)
		return NULL;

	if (chunk_id > YAFFS_MAX_CHUNK_ID)
		return NULL;

	/* First check we're tall enough (ie enough top_level) */

	x = chunk_id >> YAFFS_TNODES_LEVEL0_BITS;
	required_depth = 0;
	while (x) {
		x >>= YAFFS_TNODES_INTERNAL_BITS;
		required_depth++;
	}

	/* 如果当前的树高度不够（不足以容纳 chunk_id 表示的范围），则从 tnode tree 的根部
	   添加新的节点，增加树的高度 */
	if (required_depth > file_struct->top_level) {
		/* Not tall enough, gotta make the tree taller */
		for (i = file_struct->top_level; i < required_depth; i++) {

			tn = yaffs_get_tnode(dev);

			if (tn) {
				tn->internal[0] = file_struct->top;
				file_struct->top = tn;
				file_struct->top_level++;
			} else {
				yaffs_trace(YAFFS_TRACE_ERROR,
					"yaffs: no more tnodes");
				return NULL;
			}
		}
	}

	/* Traverse down to level 0, adding anything we need */

	l = file_struct->top_level;
	tn = file_struct->top;

	if (l > 0) {
		while (l > 0 && tn) {
			x = (chunk_id >>
			     (YAFFS_TNODES_LEVEL0_BITS +
			      (l - 1) * YAFFS_TNODES_INTERNAL_BITS)) &
			    YAFFS_TNODES_INTERNAL_MASK;

			if ((l > 1) && !tn->internal[x]) {
				/* Add missing non-level-zero tnode */
				/* 如果搜索路径中的 tnode        为空，表示当前树不是“满”的状态，为了到达目的位置，我们
				   需要扩展树，添加新的节点、补充缺少的节点数据 */
				tn->internal[x] = yaffs_get_tnode(dev);
				if (!tn->internal[x])
					return NULL;
			} else if (l == 1) {
				/* Looking from level 1 at level 0 */
				/* 如果 level = 1，表示我们已经找到了目标 tnode 的位置（因为 level 0 存储的是 nand_chunk 值）
				   这时，如果我们传入一个新的 tnode 且目标位置经存在一个旧的 tnode，我们需要释放旧的 tnode，
				   然后把传入新的 tnode 插入到目标位置，如果我们没有传入新的 tnode 就可以直接检查目标位置是否
				   已经存在旧的 tnode，如果不存在就申请一个新的插入即可 */
				if (passed_tn) {
					/* If we already have one, release it */
					if (tn->internal[x])
						yaffs_free_tnode(dev,
							tn->internal[x]);
					tn->internal[x] = passed_tn;

				} else if (!tn->internal[x]) {
					/* Don't have one, none passed in */
					tn->internal[x] = yaffs_get_tnode(dev);
					if (!tn->internal[x])
						return NULL;
				}
			}

			tn = tn->internal[x];
			l--;
		}
	} else {
		/* top is level 0 */
		if (passed_tn) {
			memcpy(tn, passed_tn,
			       (dev->tnode_width * YAFFS_NTNODES_LEVEL0) / 8);
			yaffs_free_tnode(dev, passed_tn);
		}
	}

	return tn;
}
					   
/*********************************************************************************************************
** 函数名称: yaffs_tags_match
** 功能描述: 检查传入的 obj_id 和 chunk_obj 和 tags 中的信息是否匹配
** 输	 入: tags - 需要检查的 tags 信息
**		   : obj_id - 希望的 obj_id
**		   : chunk_obj - 希望的 chunk_id
** 输	 出: 1 - 匹配
**		   : 0 - 不匹配
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_tags_match(const struct yaffs_ext_tags *tags, int obj_id,
			    int chunk_obj)
{
	return (tags->chunk_id == (u32)chunk_obj &&
		tags->obj_id == (u32)obj_id &&
		!tags->is_deleted) ? 1 : 0;

}

/*********************************************************************************************************
** 函数名称: yaffs_find_chunk_in_group
** 功能描述: 在 chunk group 中查找指定目标的 nand_chunk
** 输	 入: dev - yaffs 设备
**         : the_chunk - group base 对应的 nand_chunk 值
**         : tags - 
**		   : obj_id - 目标的 obj_id
**		   : inode_chunk - 目标的 inode_chunk
** 输	 出: 目标对象的 nand_chunk
**		   : -1 - 目标不存在
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_find_chunk_in_group(struct yaffs_dev *dev, int the_chunk,
					struct yaffs_ext_tags *tags, int obj_id,
					int inode_chunk)
{
	int j;

	for (j = 0; the_chunk && j < dev->chunk_grp_size; j++) {
		if (yaffs_check_chunk_bit
		    (dev, the_chunk / dev->param.chunks_per_block,
		     the_chunk % dev->param.chunks_per_block)) {

			if (dev->chunk_grp_size == 1)
				return the_chunk;
			else {
				yaffs_rd_chunk_tags_nand(dev, the_chunk, NULL,
							 tags);
				if (yaffs_tags_match(tags,
							obj_id, inode_chunk)) {
					/* found it; */
					return the_chunk;
				}
			}
		}
		the_chunk++;
	}
	return -1;
}

/*********************************************************************************************************
** 函数名称: yaffs_find_chunk_in_file
** 功能描述: 查找指定文件中 inode_chunk 对应的 nand_chunk 值
** 输	 入: in - 需要查找的目标文件
**		   : inode_chunk - 需要查找的 inode_chunk 值
**		   : tags - 在查找是用来存储 tags 信息的变量
** 输	 出: 目标对象的 nand_chunk
**		   : -1 - 目标不存在
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
int yaffs_find_chunk_in_file(struct yaffs_obj *in, int inode_chunk,
				    struct yaffs_ext_tags *tags)
{
	/*Get the Tnode, then get the level 0 offset chunk offset */
	struct yaffs_tnode *tn;
	int the_chunk = -1;
	struct yaffs_ext_tags local_tags;
	int ret_val = -1;
	struct yaffs_dev *dev = in->my_dev;

	if (!tags) {
		/* Passed a NULL, so use our own tags space */
		tags = &local_tags;
	}

	tn = yaffs_find_tnode_0(dev, &in->variant.file_variant, inode_chunk);

	if (!tn)
		return ret_val;

	the_chunk = yaffs_get_group_base(dev, tn, inode_chunk);

	ret_val = yaffs_find_chunk_in_group(dev, the_chunk, tags, in->obj_id,
					      inode_chunk);
	return ret_val;
}

/*********************************************************************************************************
** 函数名称: yaffs_find_del_file_chunk
** 功能描述: 删除指定文件中 inode_chunk 对应的 nand_chunk 值（如果存在设置成 0）
** 输	 入: in - 需要执行删除操作的目标文件
**		   : inode_chunk - 需要查找的 inode_chunk 值
**		   : tags - 在查找是用来存储 tags 信息的变量
** 输	 出: 目标对象的 nand_chunk
**		   : -1 - 目标不存在
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_find_del_file_chunk(struct yaffs_obj *in, int inode_chunk,
				     struct yaffs_ext_tags *tags)
{
	/* Get the Tnode, then get the level 0 offset chunk offset */
	struct yaffs_tnode *tn;
	int the_chunk = -1;
	struct yaffs_ext_tags local_tags;
	struct yaffs_dev *dev = in->my_dev;
	int ret_val = -1;

	if (!tags) {
		/* Passed a NULL, so use our own tags space */
		tags = &local_tags;
	}

	tn = yaffs_find_tnode_0(dev, &in->variant.file_variant, inode_chunk);

	if (!tn)
		return ret_val;

	the_chunk = yaffs_get_group_base(dev, tn, inode_chunk);

	ret_val = yaffs_find_chunk_in_group(dev, the_chunk, tags, in->obj_id,
					      inode_chunk);

	/* Delete the entry in the filestructure (if found) */
	if (ret_val != -1)
		yaffs_load_tnode_0(dev, tn, inode_chunk, 0);

	return ret_val;
}

/*********************************************************************************************************
** 函数名称: yaffs_put_chunk_in_file
** 功能描述: 向指定文件 tnode tree 的 inode_chunk 位置添加一个新节点并设置它的物理 chunk 值为 nand_chunk
**         : 即向文件中添加新数据内容，新的数据存在 nand_chunk 中
**         : （如果 in 不是文件类型，则擦除传入的 nand_chunk 空间）
** 输     入: in - 需要查找的目标文件
** 		   : inode_chunk - 需要查找的 inode_chunk 值
** 		   : nand_chunk - 在查找是用来存储 tags 信息的变量
** 		   : in_scan - 
** 输     出: YAFFS_OK - 操作成功
** 		   : YAFFS_FAIL - 操作失败
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
int yaffs_put_chunk_in_file(struct yaffs_obj *in, int inode_chunk,
			    int nand_chunk, int in_scan)
{
	/* NB in_scan is zero unless scanning.
	 * For forward scanning, in_scan is > 0;
	 * for backward scanning in_scan is < 0
	 *
	 * nand_chunk = 0 is a dummy insert to make sure the tnodes are there.
	 */

	struct yaffs_tnode *tn;
	struct yaffs_dev *dev = in->my_dev;
	int existing_cunk;
	struct yaffs_ext_tags existing_tags;
	struct yaffs_ext_tags new_tags;
	unsigned existing_serial, new_serial;

	if (in->variant_type != YAFFS_OBJECT_TYPE_FILE) {
		/* Just ignore an attempt at putting a chunk into a non-file
		 * during scanning.
		 * If it is not during Scanning then something went wrong!
		 */
		if (!in_scan) {
			yaffs_trace(YAFFS_TRACE_ERROR,
				"yaffs tragedy:attempt to put data chunk into a non-file"
				);
			BUG();
		}

		yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
		return YAFFS_OK;
	}

	tn = yaffs_add_find_tnode_0(dev,
				    &in->variant.file_variant,
				    inode_chunk, NULL);
	if (!tn)
		return YAFFS_FAIL;

	if (!nand_chunk)
		/* Dummy insert, bail now */
		return YAFFS_OK;

	existing_cunk = yaffs_get_group_base(dev, tn, inode_chunk);

	if (in_scan != 0) {
		/* If we're scanning then we need to test for duplicates
		 * NB This does not need to be efficient since it should only
		 * happen when the power fails during a write, then only one
		 * chunk should ever be affected.
		 *
		 * Correction for YAFFS2: This could happen quite a lot and we
		 * need to think about efficiency! TODO
		 * Update: For backward scanning we don't need to re-read tags
		 * so this is quite cheap.
		 */

		if (existing_cunk > 0) {
			/* NB Right now existing chunk will not be real
			 * chunk_id if the chunk group size > 1
			 * thus we have to do a FindChunkInFile to get the
			 * real chunk id.
			 *
			 * We have a duplicate now we need to decide which
			 * one to use:
			 *
			 * Backwards scanning YAFFS2: The old one is what
			 * we use, dump the new one.
			 * YAFFS1: Get both sets of tags and compare serial
			 * numbers.
			 */

			if (in_scan > 0) {
				/* Only do this for forward scanning */
				yaffs_rd_chunk_tags_nand(dev,
							 nand_chunk,
							 NULL, &new_tags);

				/* Do a proper find */
				existing_cunk =
				    yaffs_find_chunk_in_file(in, inode_chunk,
							     &existing_tags);
			}

			if (existing_cunk <= 0) {
				/*Hoosterman - how did this happen? */

				yaffs_trace(YAFFS_TRACE_ERROR,
					"yaffs tragedy: existing chunk < 0 in scan"
					);

			}

			/* NB The deleted flags should be false, otherwise
			 * the chunks will not be loaded during a scan
			 */

			if (in_scan > 0) {
				new_serial = new_tags.serial_number;
				existing_serial = existing_tags.serial_number;
			}

			if ((in_scan > 0) &&
			    (existing_cunk <= 0 ||
			     ((existing_serial + 1) & 3) == new_serial)) {
				/* Forward scanning.
				 * Use new
				 * Delete the old one and drop through to
				 * update the tnode
				 */
				yaffs_chunk_del(dev, existing_cunk, 1,
						__LINE__);
			} else {
				/* Backward scanning or we want to use the
				 * existing one
				 * Delete the new one and return early so that
				 * the tnode isn't changed
				 */
				yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
				return YAFFS_OK;
			}
		}

	}

	if (existing_cunk == 0)
		in->n_data_chunks++;

	yaffs_load_tnode_0(dev, tn, inode_chunk, nand_chunk);

	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_soft_del_chunk
** 功能描述: 软件删除指定的 chunk 并更新相关状态变量
** 输	 入: dev - yaffs 设备
**		   : chunk - nand_chunk
** 输	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_soft_del_chunk(struct yaffs_dev *dev, int chunk)
{
	struct yaffs_block_info *the_block;
	unsigned block_no;

	yaffs_trace(YAFFS_TRACE_DELETION, "soft delete chunk %d", chunk);

	block_no = chunk / dev->param.chunks_per_block;
	the_block = yaffs_get_block_info(dev, block_no);
	if (the_block) {
		the_block->soft_del_pages++;
		dev->n_free_chunks++;
		yaffs2_update_oldest_dirty_seq(dev, block_no, the_block);
	}
}

/* SoftDeleteWorker scans backwards through the tnode tree and soft deletes all
 * the chunks in the file.
 * All soft deleting does is increment the block's softdelete count and pulls
 * the chunk out of the tnode.
 * Thus, essentially this is the same as DeleteWorker except that the chunks
 * are soft deleted.
 */

/*********************************************************************************************************
** 函数名称: yaffs_soft_del_worker
** 功能描述: 软件删除指定 object 的 nand_chunk 和 tnode tree，并释放所有 tnode 给 allocator
** 输	 入: in - 需要操作的 object
**		   : tn - object 对应 tnode tree top 指针
**		   : level - tnode tree top 在 tnode tree 中的 level 级别
**		   : chunk_offset - 默认为 0
** 输	 出: 1 - 下层树已全部遍历完
**		   : 0 - 下层树没遍历完
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_soft_del_worker(struct yaffs_obj *in, struct yaffs_tnode *tn,
				 u32 level, int chunk_offset)
{
	int i;
	int the_chunk;
	int all_done = 1;
	struct yaffs_dev *dev = in->my_dev;

	if (!tn)
		return 1;

	/* 通过递归方式遍历 tnode tree 上所有 tnode 节点，执行删除操作的顺序是从下往上、从后往前 */
	if (level > 0) {
		for (i = YAFFS_NTNODES_INTERNAL - 1;
			all_done && i >= 0;
			i--) {
			if (tn->internal[i]) {
				all_done =
				    yaffs_soft_del_worker(in,
					tn->internal[i],
					level - 1,
					(chunk_offset <<
					YAFFS_TNODES_INTERNAL_BITS)
					+ i);
				if (all_done) {
					yaffs_free_tnode(dev,
						tn->internal[i]);
					tn->internal[i] = NULL;
				} else {
					/* Can this happen? */
				}
			}
		}
		return (all_done) ? 1 : 0;
	}

	/* level 0 */
	/* 释放 tnode tree level 0 上的 tnode 以及软件删除对应的 nand_chunk 数据 */
	 for (i = YAFFS_NTNODES_LEVEL0 - 1; i >= 0; i--) {
		the_chunk = yaffs_get_group_base(dev, tn, i);
		if (the_chunk) {
			yaffs_soft_del_chunk(dev, the_chunk);
			yaffs_load_tnode_0(dev, tn, i, 0);
		}
	}
	return 1;
}

/*********************************************************************************************************
** 函数名称: yaffs_remove_obj_from_dir
** 功能描述: 把 obj 从 obj 的父目录中删除（在执行删除操作前后都做了相应的校验）
** 输     入: obj - 需要删除的 obj 对象
** 输     出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_remove_obj_from_dir(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev = obj->my_dev;
	struct yaffs_obj *parent;

	yaffs_verify_obj_in_dir(obj);
	parent = obj->parent;

	yaffs_verify_dir(parent);

	if (dev && dev->param.remove_obj_fn)
		dev->param.remove_obj_fn(obj);

	list_del_init(&obj->siblings);
	obj->parent = NULL;

	yaffs_verify_dir(parent);
}

/*********************************************************************************************************
** 函数名称: yaffs_add_obj_to_dir
** 功能描述: 把 obj 添加到指定的目录中（在执行删除操作前后都做了相应的校验）
**         : directory - 目标目录
** 输     入: obj - 需要添加的 obj 对象
** 输     出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
void yaffs_add_obj_to_dir(struct yaffs_obj *directory, struct yaffs_obj *obj)
{
	if (!directory) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: Trying to add an object to a null pointer directory"
			);
		BUG();
		return;
	}
	if (directory->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: Trying to add an object to a non-directory"
			);
		BUG();
	}

	if (obj->siblings.prev == NULL) {
		/* Not initialised */
		BUG();
	}

	yaffs_verify_dir(directory);

	yaffs_remove_obj_from_dir(obj);

	/* Now add it */
	list_add(&obj->siblings, &directory->variant.dir_variant.children);
	obj->parent = directory;

	if (directory == obj->my_dev->unlinked_dir
	    || directory == obj->my_dev->del_dir) {
		obj->unlinked = 1;
		obj->my_dev->n_unlinked_files++;
		obj->rename_allowed = 0;
	}

	yaffs_verify_dir(directory);
	yaffs_verify_obj_in_dir(obj);
}

/*********************************************************************************************************
** 函数名称: yaffs_change_obj_name
** 功能描述: 修改 obj 名字（可以跨目录修改）
** 输     入: obj - 需要修改名字的 obj 对象
**         : new_dir - 修改后 obj 所在的新目录
**         : new_name - 修改后 obj 新的名字
**         : force - 
**         : shadows - 
** 输     出: YAFFS_OK - 执行成功
**         : YAFFS_FAIL - 执行失败
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_change_obj_name(struct yaffs_obj *obj,
				 struct yaffs_obj *new_dir,
				 const YCHAR *new_name, int force, int shadows)
{
	int unlink_op;
	int del_op;
	struct yaffs_obj *existing_target;

	if (new_dir == NULL)
		new_dir = obj->parent;	/* use the old directory */

	if (new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_change_obj_name: new_dir is not a directory"
			);
		BUG();
	}

	unlink_op = (new_dir == obj->my_dev->unlinked_dir);
	del_op = (new_dir == obj->my_dev->del_dir);

	existing_target = yaffs_find_by_name(new_dir, new_name);

	/* If the object is a file going into the unlinked directory,
	 *   then it is OK to just stuff it in since duplicate names are OK.
	 *   else only proceed if the new name does not exist and we're putting
	 *   it into a directory.
	 */
	if (!(unlink_op || del_op || force ||
	      shadows > 0 || !existing_target) ||
	      new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY)
		return YAFFS_FAIL;

	yaffs_set_obj_name(obj, new_name);
	obj->dirty = 1;
	yaffs_add_obj_to_dir(new_dir, obj);

	if (unlink_op)
		obj->unlinked = 1;

	/* If it is a deletion then we mark it as a shrink for gc  */
	if (yaffs_update_oh(obj, new_name, 0, del_op, shadows, NULL) >= 0)
		return YAFFS_OK;

	return YAFFS_FAIL;
}

/*------------------------ Short Operations Cache ------------------------------
 *   In many situations where there is no high level buffering  a lot of
 *   reads might be short sequential reads, and a lot of writes may be short
 *   sequential writes. eg. scanning/writing a jpeg file.
 *   In these cases, a short read/write cache can provide a huge perfomance
 *   benefit with dumb-as-a-rock code.
 *   In Linux, the page cache provides read buffering and the short op cache
 *   provides write buffering.
 *
 *   There are a small number (~10) of cache chunks per device so that we don't
 *   need a very intelligent search.
 */

/*********************************************************************************************************
** 函数名称: yaffs_obj_cache_dirty
** 功能描述: 判断当前设备的指定对象在 Short Operations Cache 中是否有 dirty 数据
** 输 	 入: obj - 需要查询的 obj 对象
** 输 	 出: 1 - 有 dirty 数据
**         : 0 - 无 dirty 数据
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static int yaffs_obj_cache_dirty(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev = obj->my_dev;
	int i;
	struct yaffs_cache *cache;
	int n_caches = obj->my_dev->param.n_caches;

	for (i = 0; i < n_caches; i++) {
		cache = &dev->cache[i];
		if (cache->object == obj && cache->dirty)
			return 1;
	}

	return 0;
}

/*********************************************************************************************************
** 函数名称: yaffs_flush_single_cache
** 功能描述: 刷新一个 Short Operations Cache（ONE CHUNK） 的 dirty 数据到 FLASH 中，并更新状态
** 输 	 入: cache - 需要刷新的 Cache 地址
**         : discard - 刷数据后是否需要清除掉当前 Cache 数据
** 输 	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_flush_single_cache(struct yaffs_cache *cache, int discard)
{

	if (!cache || cache->locked)
		return;

	/* Write it out and free it up  if need be.*/
	if (cache->dirty) {
		yaffs_wr_data_obj(cache->object,
				  cache->chunk_id,
				  cache->data,
				  cache->n_bytes,
				  1);

		cache->dirty = 0;
	}

	if (discard)
		cache->object = NULL;
}

/*********************************************************************************************************
** 函数名称: yaffs_flush_file_cache
** 功能描述: 刷新一个文件的 Short Operations Cache 所有 dirty 数据到 FLASH 中
** 输 	 入: obj - 需要刷新的文件对象
**         : discard - 刷数据后是否需要清除掉当前文件在 Cache 中的所有数据
** 输 	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static void yaffs_flush_file_cache(struct yaffs_obj *obj, int discard)
{
	struct yaffs_dev *dev = obj->my_dev;
	int i;
	struct yaffs_cache *cache;
	int n_caches = obj->my_dev->param.n_caches;

	if (n_caches < 1)
		return;


	/* Find the chunks for this object and flush them. */
	for (i = 0; i < n_caches; i++) {
		cache = &dev->cache[i];
		if (cache->object == obj)
			yaffs_flush_single_cache(cache, discard);
	}

}

/*********************************************************************************************************
** 函数名称: yaffs_flush_whole_cache
** 功能描述: 刷新当前设备 Short Operations Cache 中所有对象的 dirty 数据到 FLASH 中
** 输 	 入: obj - 需要刷新的文件对象
**         : discard - 刷数据后是否需要清除掉当前文件在 Cache 中的所有数据
** 输 	 出: 
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
void yaffs_flush_whole_cache(struct yaffs_dev *dev, int discard)
{
	struct yaffs_obj *obj;
	int n_caches = dev->param.n_caches;
	int i;

	/* Find a dirty object in the cache and flush it...
	 * until there are no further dirty objects.
	 */
	do {
		obj = NULL;
		for (i = 0; i < n_caches && !obj; i++) {
			if (dev->cache[i].object && dev->cache[i].dirty)
				obj = dev->cache[i].object;
		}
		if (obj)
			yaffs_flush_file_cache(obj, discard);
	} while (obj);

}

/* Grab us an unused cache chunk for use.
 * First look for an empty one.
 * Then look for the least recently used non-dirty one.
 * Then look for the least recently used dirty one...., flush and look again.
 */

/*********************************************************************************************************
** 函数名称: yaffs_grab_chunk_worker
** 功能描述: 从当前设备 Short Operations Cache 中获取一个未使用的 Cache
** 输 	 入: dev - yaffs 设备
** 输 	 出: dev->cache - 空闲的 Cache
**         : NULL - 没有空闲 Cache
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_cache *yaffs_grab_chunk_worker(struct yaffs_dev *dev)
{
	u32 i;

	if (dev->param.n_caches > 0) {
		for (i = 0; i < dev->param.n_caches; i++) {
			if (!dev->cache[i].object)
				return &dev->cache[i];
		}
	}

	return NULL;
}

/*********************************************************************************************************
** 函数名称: yaffs_grab_chunk_cache
** 功能描述: 从当前设备 Short Operations Cache 中获取 Cache，如果没有空闲 Cache，则找一个最近
**         : 最少使用的 Cache、刷新他的数据到 FLASH，然后返回
** 输 	 入: dev - yaffs 设备
** 输 	 出: cache - 空闲的 Cache
** 全局变量:
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_cache *yaffs_grab_chunk_cache(struct yaffs_dev *dev)
{
	struct yaffs_cache *cache;
	int usage;
	u32 i;

	if (dev->param.n_caches < 1)
		return NULL;

	/* First look for an unused cache */

	cache = yaffs_grab_chunk_worker(dev);

	if (cache)
		return cache;

	/*
	 * Thery were all in use.
	 * Find the LRU cache and flush it if it is dirty.
	 */

	usage = -1;
	cache = NULL;

	for (i = 0; i < dev->param.n_caches; i++) {
		if (dev->cache[i].object &&
		    !dev->cache[i].locked &&
		    (dev->cache[i].last_use < usage || !cache)) {
				usage = dev->cache[i].last_use;
				cache = &dev->cache[i];
		}
	}

#if 1
	yaffs_flush_single_cache(cache, 1);
#else
	yaffs_flush_file_cache(cache->object, 1);
	cache = yaffs_grab_chunk_worker(dev);
#endif

	return cache;
}

/* Find a cached chunk */

/*********************************************************************************************************
** 函数名称: yaffs_find_chunk_cache
** 功能描述: 从当前设备 Short Operations Cache 查找指定对象、指定 inode_chunk 的 Cache 数据
** 输 	 入: obj - 需要查询的目标对象
**         : chunk_id - 需要查询的 inode_chunk 值
** 输 	 出: dev->cache - 命中的 Cache
**         : NULL - 查找未命中
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_cache *yaffs_find_chunk_cache(const struct yaffs_obj *obj,
						  int chunk_id)
{
	struct yaffs_dev *dev = obj->my_dev;
	u32 i;

	if (dev->param.n_caches < 1)
		return NULL;

	for (i = 0; i < dev->param.n_caches; i++) {
		if (dev->cache[i].object == obj &&
		    dev->cache[i].chunk_id == chunk_id) {
			dev->cache_hits++;

			return &dev->cache[i];
		}
	}
	return NULL;
}

/* Mark the chunk for the least recently used algorithym */

/*********************************************************************************************************
** 函数名称: yaffs_use_cache
** 功能描述: 更新当前设备 Cache LRU（least recently used）标记变量算法，这个标志变量在
**         : FLUSH Cache 的时候会使用到。yaffs 每次使用 Cache 的时候会执行 dev->cache_last_use++;
**         : 然后把 dev->cache_last_use 赋值给当前使用的 Cache 的 cache->last_use 变量，来表示每个
**         : Cache 的使用顺序
** 输 	 入: dev - yaffs 设备
**         : cache - 当前使用的 Cache，我们会设置它的 last_use 字段
**         : is_write - 表示当前的 Cache 是否是写数据操作
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_use_cache(struct yaffs_dev *dev, struct yaffs_cache *cache,
			    int is_write)
{
	u32 i;

	if (dev->param.n_caches < 1)
		return;

	if (dev->cache_last_use < 0 ||
		dev->cache_last_use > 100000000) {
		/* Reset the cache usages */
		for (i = 1; i < dev->param.n_caches; i++)
			dev->cache[i].last_use = 0;

		dev->cache_last_use = 0;
	}
	dev->cache_last_use++;
	cache->last_use = dev->cache_last_use;

	if (is_write)
		cache->dirty = 1;
}

/* Invalidate a single cache page.
 * Do this when a whole page gets written,
 * ie the short cache for this page is no longer valid.
 */

/*********************************************************************************************************
** 函数名称: yaffs_invalidate_chunk_cache
** 功能描述: 从当前设备 Short Operations Cache 清除指定对象、指定 inode_chunk 的 Cache 数据
** 输	 入: obj - 需要查询的目标对象
**		   : chunk_id - 需要查询的 inode_chunk 值
** 输	 出: dev->cache - 命中的 Cache
**		   : NULL - 查找未命中
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_invalidate_chunk_cache(struct yaffs_obj *object, int chunk_id)
{
	struct yaffs_cache *cache;

	if (object->my_dev->param.n_caches > 0) {
		cache = yaffs_find_chunk_cache(object, chunk_id);

		if (cache)
			cache->object = NULL;
	}
}

/* Invalidate all the cache pages associated with this object
 * Do this whenever ther file is deleted or resized.
 */
 
 /*********************************************************************************************************
** 函数名称: yaffs_invalidate_whole_cache
** 功能描述: 从当前设备 Short Operations Cache 清除指定对象的所有 Cache 数据
** 输	 入: in - 需要清除 Cache 数据的对象
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_invalidate_whole_cache(struct yaffs_obj *in)
{
	u32 i;
	struct yaffs_dev *dev = in->my_dev;

	if (dev->param.n_caches > 0) {
		/* Invalidate it. */
		for (i = 0; i < dev->param.n_caches; i++) {
			if (dev->cache[i].object == in)
				dev->cache[i].object = NULL;
		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_unhash_obj
** 功能描述: 从 obj_bucket 中删除指定对象，并更新相关状态变量
** 输	 入: obj - 需要删除的 object
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_unhash_obj(struct yaffs_obj *obj)
{
	int bucket;
	struct yaffs_dev *dev = obj->my_dev;

	/* If it is still linked into the bucket list, free from the list */
	if (!list_empty(&obj->hash_link)) {
		list_del_init(&obj->hash_link);
		bucket = yaffs_hash_fn(obj->obj_id);
		dev->obj_bucket[bucket].count--;
	}
}

/*  FreeObject frees up a Object and puts it back on the free list */

/*********************************************************************************************************
** 函数名称: yaffs_free_obj
** 功能描述: 从 obj_bucket 中删除一个指定的对象，并释放相应的 obj 到 allocator 中
** 输	 入: obj - 需要删除的 object
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_free_obj(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev;

	if (!obj) {
		BUG();
		return;
	}
	dev = obj->my_dev;
	yaffs_trace(YAFFS_TRACE_OS, "FreeObject %p inode %p",
		obj, obj->my_inode);
	if (obj->parent)
		BUG();
	if (!list_empty(&obj->siblings))
		BUG();

	if (obj->my_inode) {
		/* We're still hooked up to a cached inode.
		 * Don't delete now, but mark for later deletion
		 */
		obj->defered_free = 1;
		return;
	}

	yaffs_unhash_obj(obj);

	yaffs_free_raw_obj(dev, obj);
	dev->n_obj--;
	dev->checkpoint_blocks_required = 0;	/* force recalculation */
}

/*********************************************************************************************************
** 函数名称: yaffs_handle_defered_free
** 功能描述: 处理对象 “延迟删除” 操作
** 输	 入: obj - 需要删除的 object
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_handle_defered_free(struct yaffs_obj *obj)
{
	if (obj->defered_free)
		yaffs_free_obj(obj);
}

/*********************************************************************************************************
** 函数名称: yaffs_generic_obj_del
** 功能描述: 表示指定对象没有数据（在 FLASH 中无数据，但是在 Cache 中可能有数据），直接删除对象并释放
**         : 相关内存资源（tnode，obj_bucket，从目录中移除）同时会 Invalid 这个对象的所有 Cache 数据
**         : 并删除 FLASH 中的对象头数据
** 输	 入: in - 需要删除的目标对象（目录、文件...）
** 输	 出: YAFFS_OK - 默认执行成功
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_generic_obj_del(struct yaffs_obj *in)
{
	/* Iinvalidate the file's data in the cache, without flushing. */
	yaffs_invalidate_whole_cache(in);

	if (in->my_dev->param.is_yaffs2 && in->parent != in->my_dev->del_dir) {
		/* Move to unlinked directory so we have a deletion record */
		yaffs_change_obj_name(in, in->my_dev->del_dir, _Y("deleted"), 0,
				      0);
	}

	yaffs_remove_obj_from_dir(in);
	yaffs_chunk_del(in->my_dev, in->hdr_chunk, 1, __LINE__);
	in->hdr_chunk = 0;

	yaffs_free_obj(in);
	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_soft_del_file
** 功能描述: 软件删除一个指定文件，如果这个文件在 FLASH 中没有数据（可能在 Cache 中有），那么直接删除
**         : 否则调用 yaffs_soft_del_worker 递归删除 FLASH 中所有数据并释放相关资源到 allocator
** 输	 入: in - 需要删除的目标对象（目录、文件...）
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/

static void yaffs_soft_del_file(struct yaffs_obj *obj)
{
	if (!obj->deleted ||
	    obj->variant_type != YAFFS_OBJECT_TYPE_FILE ||
	    obj->soft_del)
		return;

	if (obj->n_data_chunks <= 0) {
		/* Empty file with no duplicate object headers,
		 * just delete it immediately */
		yaffs_free_tnode(obj->my_dev, obj->variant.file_variant.top);
		obj->variant.file_variant.top = NULL;
		yaffs_trace(YAFFS_TRACE_TRACING,
			"yaffs: Deleting empty file %d",
			obj->obj_id);
		yaffs_generic_obj_del(obj);
	} else {
		yaffs_soft_del_worker(obj,
				      obj->variant.file_variant.top,
				      obj->variant.
				      file_variant.top_level, 0);
		obj->soft_del = 1;
	}
}

/* Pruning removes any part of the file structure tree that is beyond the
 * bounds of the file (ie that does not point to chunks).
 *
 * A file should only get pruned when its size is reduced.
 *
 * Before pruning, the chunks must be pulled from the tree and the
 * level 0 tnode entries must be zeroed out.
 * Could also use this for file deletion, but that's probably better handled
 * by a special case.
 *
 * This function is recursive. For levels > 0 the function is called again on
 * any sub-tree. For level == 0 we just check if the sub-tree has data.
 * If there is no data in a subtree then it is pruned.
 */

/*********************************************************************************************************
** 函数名称: yaffs_prune_worker
** 功能描述: “修剪” 指定 tnode tree 结构，所谓的修剪是指会把传入的 tnode tree 中为 NULL 的叶子节点从
**         : tnode tree 中移除并释放相应的资源到 allocator 中
** 输	 入: dev - yaffs 设备
**         : tn - tnode top 指针
**         : level - tnode top 在 tnode tree 中的 level
**         : del0 - tn 在 tnode 中的索引是否为 0
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_tnode *yaffs_prune_worker(struct yaffs_dev *dev,
					      struct yaffs_tnode *tn, u32 level,
					      int del0)
{
	int i;
	int has_data;

	if (!tn)
		return tn;

	has_data = 0;

	if (level > 0) {
		for (i = 0; i < YAFFS_NTNODES_INTERNAL; i++) {
			if (tn->internal[i]) {
				tn->internal[i] =
				    yaffs_prune_worker(dev,
						tn->internal[i],
						level - 1,
						(i == 0) ? del0 : 1);
			}

			if (tn->internal[i])
				has_data++;
		}
	} else {
		int tnode_size_u32 = dev->tnode_size / sizeof(u32);
		u32 *map = (u32 *) tn;

		for (i = 0; !has_data && i < tnode_size_u32; i++) {
			if (map[i])
				has_data++;
		}
	}

	if (has_data == 0 && del0) {
		/* Free and return NULL */
		yaffs_free_tnode(dev, tn);
		tn = NULL;
	}
	return tn;
}

/*********************************************************************************************************
** 函数名称: yaffs_prune_tree
** 功能描述: “修剪” 指定文件的 tnode tree 结构，所谓的修剪是指会把传入文件的 tnode tree 中
**		   : 为 NULL 的叶子节点从tnode tree 中移除并释放相应的资源到 allocator 中。在修剪后
**		   : 会判断文件 tnode tree 的 top 是否只是索引 0 的位置有数据，其他索引位置为空（在这种场景
**		   : 下，我们可以删除根节点，降低树的高度），如果是，就删除 top 根节点，降低树高度
** 输     入: dev - yaffs 设备
**		   : file_struct - 需要修剪的文件对象
** 输     出: YAFFS_OK - 操作完成
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_prune_tree(struct yaffs_dev *dev,
			    struct yaffs_file_var *file_struct)
{
	int i;
	int has_data;
	int done = 0;
	struct yaffs_tnode *tn;

	if (file_struct->top_level < 1)
		return YAFFS_OK;

	file_struct->top =
	   yaffs_prune_worker(dev, file_struct->top, file_struct->top_level, 0);

	/* Now we have a tree with all the non-zero branches NULL but
	 * the height is the same as it was.
	 * Let's see if we can trim internal tnodes to shorten the tree.
	 * We can do this if only the 0th element in the tnode is in use
	 * (ie all the non-zero are NULL)
	 */

	while (file_struct->top_level && !done) {
		tn = file_struct->top;

		has_data = 0;
		for (i = 1; i < YAFFS_NTNODES_INTERNAL; i++) {
			if (tn->internal[i])
				has_data++;
		}

		if (!has_data) {
			file_struct->top = tn->internal[0];
			file_struct->top_level--;
			yaffs_free_tnode(dev, tn);
		} else {
			done = 1;
		}
	}

	return YAFFS_OK;
}

/*-------------------- End of File Structure functions.-------------------*/

/* alloc_empty_obj gets us a clean Object.*/

/*********************************************************************************************************
** 函数名称: yaffs_alloc_empty_obj
** 功能描述: 从 allocator 申请一个 obj，并做简单成员初始化
** 输	 入: dev - yaffs 设备
** 输     出: obj - 申请到的 object
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_obj *yaffs_alloc_empty_obj(struct yaffs_dev *dev)
{
	struct yaffs_obj *obj = yaffs_alloc_raw_obj(dev);

	if (!obj)
		return obj;

	dev->n_obj++;

	/* Now sweeten it up... */

	memset(obj, 0, sizeof(struct yaffs_obj));
	obj->being_created = 1;

	obj->my_dev = dev;
	obj->hdr_chunk = 0;
	obj->variant_type = YAFFS_OBJECT_TYPE_UNKNOWN;
	INIT_LIST_HEAD(&(obj->hard_links));
	INIT_LIST_HEAD(&(obj->hash_link));
	INIT_LIST_HEAD(&obj->siblings);

	/* Now make the directory sane */
	if (dev->root_dir) {
		obj->parent = dev->root_dir;
		list_add(&(obj->siblings),
			 &dev->root_dir->variant.dir_variant.children);
	}

	/* Add it to the lost and found directory.
	 * NB Can't put root or lost-n-found in lost-n-found so
	 * check if lost-n-found exists first
	 */
	if (dev->lost_n_found)
		yaffs_add_obj_to_dir(dev->lost_n_found, obj);

	obj->being_created = 0;

	dev->checkpoint_blocks_required = 0;	/* force recalculation */

	return obj;
}

/*********************************************************************************************************
** 函数名称: yaffs_find_nice_bucket
** 功能描述: 找出一个“好”的 bucket 位置用于存放新的 object，所谓的“好”指的是目标位置存储的对象个数
**         : 比较少
** 输	 入: dev - yaffs 设备
** 输     出: l - 找到位置对应的 obj_id
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_find_nice_bucket(struct yaffs_dev *dev)
{
	int i;
	int l = 999;
	int lowest = 999999;

	/* Search for the shortest list or one that
	 * isn't too long.
	 */

	for (i = 0; i < 10 && lowest > 4; i++) {
		dev->bucket_finder++;
		dev->bucket_finder %= YAFFS_NOBJECT_BUCKETS;
		if (dev->obj_bucket[dev->bucket_finder].count < lowest) {
			lowest = dev->obj_bucket[dev->bucket_finder].count;
			l = dev->bucket_finder;
		}
	}

	return l;
}

/*********************************************************************************************************
** 函数名称: yaffs_new_obj_id
** 功能描述: 创建一个新的 obj_id，并在创建之后校验了得到的 obj_id 是否已经存在
** 输	 入: dev - yaffs 设备
** 输     出: n - 申请到的 obj_id
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_new_obj_id(struct yaffs_dev *dev)
{
	int bucket = yaffs_find_nice_bucket(dev);
	int found = 0;
	struct list_head *i;
	u32 n = (u32) bucket;

	/*
	 * Now find an object value that has not already been taken
	 * by scanning the list, incrementing each time by number of buckets.
	 */
	while (!found) {
		found = 1;
		n += YAFFS_NOBJECT_BUCKETS;
		list_for_each(i, &dev->obj_bucket[bucket].list) {
			/* Check if this value is already taken. */
			if (i && list_entry(i, struct yaffs_obj,
					    hash_link)->obj_id == n)
				found = 0;
		}
	}
	return n;
}

/*********************************************************************************************************
** 函数名称: yaffs_hash_obj
** 功能描述: 添加指定对象到 obj_bucket 成员链表中并更新相关变量
** 输	 入: in - 需要做 hash 运算的对象
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_hash_obj(struct yaffs_obj *in)
{
	int bucket = yaffs_hash_fn(in->obj_id);
	struct yaffs_dev *dev = in->my_dev;

	list_add(&in->hash_link, &dev->obj_bucket[bucket].list);
	dev->obj_bucket[bucket].count++;
}

/*********************************************************************************************************
** 函数名称: yaffs_find_by_number
** 功能描述: 通过指定的对象 obj_id 到 obj_bucket 中查找对应的 obj 对象
** 输	 入: dev - yaffs 设备
**         : number - 想要查找对象的 obj_id
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_find_by_number(struct yaffs_dev *dev, u32 number)
{
	int bucket = yaffs_hash_fn(number);
	struct list_head *i;
	struct yaffs_obj *in;

	list_for_each(i, &dev->obj_bucket[bucket].list) {
		/* Look if it is in the list */
		in = list_entry(i, struct yaffs_obj, hash_link);
		if (in->obj_id == number) {
			/* Don't show if it is defered free */
			if (in->defered_free)
				return NULL;
			return in;
		}
	}

	return NULL;
}

/*********************************************************************************************************
** 函数名称: yaffs_new_obj
** 功能描述: 创建一个新的对象，并做简单初始化
** 输	 入: dev - yaffs 设备
**         : number - 创建对象的 obj_id，如果 < 0，表示由系统自动分配
**         : type - 创建对象的类型（目录、文件...）
** 输     出: the_obj - 新创建的对象地址
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_obj *yaffs_new_obj(struct yaffs_dev *dev, int number,
				enum yaffs_obj_type type)
{
	struct yaffs_obj *the_obj = NULL;
	struct yaffs_tnode *tn = NULL;

	if (number < 0)
		number = yaffs_new_obj_id(dev);

	if (type == YAFFS_OBJECT_TYPE_FILE) {
		tn = yaffs_get_tnode(dev);
		if (!tn)
			return NULL;
	}

	the_obj = yaffs_alloc_empty_obj(dev);
	if (!the_obj) {
		if (tn)
			yaffs_free_tnode(dev, tn);
		return NULL;
	}

	the_obj->fake = 0;
	the_obj->rename_allowed = 1;
	the_obj->unlink_allowed = 1;
	the_obj->obj_id = number;
	yaffs_hash_obj(the_obj);
	the_obj->variant_type = type;
	yaffs_load_current_time(the_obj, 1, 1);

	switch (type) {
	case YAFFS_OBJECT_TYPE_FILE:
		the_obj->variant.file_variant.file_size = 0;
		the_obj->variant.file_variant.stored_size = 0;
		the_obj->variant.file_variant.shrink_size =
						yaffs_max_file_size(dev);
		the_obj->variant.file_variant.top_level = 0;
		the_obj->variant.file_variant.top = tn;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		INIT_LIST_HEAD(&the_obj->variant.dir_variant.children);
		INIT_LIST_HEAD(&the_obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
	case YAFFS_OBJECT_TYPE_HARDLINK:
	case YAFFS_OBJECT_TYPE_SPECIAL:
		/* No action required */
		break;
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* todo this should not happen */
		break;
	}
	return the_obj;
}

/*********************************************************************************************************
** 函数名称: yaffs_create_fake_dir
** 功能描述: 创建一个假目录对象，所谓的“假”是指这个对象只在内存中，不会存储到 FLASH 中，比如“根目录”
**		   : “lost+found”目录、“deleted”目录以及“unlink”目录都是假对象
** 输	 入: dev - yaffs 设备
**		   : number - 创建对象的 obj_id，如果 < 0，表示由系统自动分配
**		   : mode - 创建对象的权限
** 输	 出: obj - 新创建的对象地址
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_obj *yaffs_create_fake_dir(struct yaffs_dev *dev,
					       int number, u32 mode)
{

	struct yaffs_obj *obj =
	    yaffs_new_obj(dev, number, YAFFS_OBJECT_TYPE_DIRECTORY);

	if (!obj)
		return NULL;

	obj->fake = 1;	/* it is fake so it might not use NAND */
	obj->rename_allowed = 0;
	obj->unlink_allowed = 0;
	obj->deleted = 0;
	obj->unlinked = 0;
	obj->yst_mode = mode;
	obj->my_dev = dev;
	obj->hdr_chunk = 0;	/* Not a valid chunk. */
	return obj;

}

/*********************************************************************************************************
** 函数名称: yaffs_init_tnodes_and_objs
** 功能描述: 初始化 allocator 中的 tnode 和 obj，并初始化设备的 obj_bucket 数组
** 输	 入: dev - yaffs 设备
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_init_tnodes_and_objs(struct yaffs_dev *dev)
{
	int i;

	dev->n_obj = 0;
	dev->n_tnodes = 0;
	yaffs_init_raw_tnodes_and_objs(dev);

	for (i = 0; i < YAFFS_NOBJECT_BUCKETS; i++) {
		INIT_LIST_HEAD(&dev->obj_bucket[i].list);
		dev->obj_bucket[i].count = 0;
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_find_or_create_by_number
** 功能描述: 通过指定的 obj_id 查找与其对应的 obj 对象，如果当前设备中没有和指定 obj_id 对应的
**         : 对象，则自动创建一个新的对象
** 输	 入: dev - yaffs 设备
**         : number - 查找对象的 obj_id
**         : type - 创建对象的类型（目录、文件...）
** 输	 出: the_obj - 查找到的或新创建的对象指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_find_or_create_by_number(struct yaffs_dev *dev,
						 int number,
						 enum yaffs_obj_type type)
{
	struct yaffs_obj *the_obj = NULL;

	if (number > 0)
		the_obj = yaffs_find_by_number(dev, number);

	if (!the_obj)
		the_obj = yaffs_new_obj(dev, number, type);

	return the_obj;

}

/*********************************************************************************************************
** 函数名称: yaffs_clone_str
** 功能描述: 克隆传入的字符串对象并返回新的字符串地址（新克隆的对象空间是通过动态内存分配的）
** 输     入: str - 需要克隆的字符串
** 输     出: new_str - 克隆后的字符串对象
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
YCHAR *yaffs_clone_str(const YCHAR *str)
{
	YCHAR *new_str = NULL;
	int len;

	if (!str)
		str = _Y("");

	len = strnlen(str, YAFFS_MAX_ALIAS_LENGTH);
	new_str = kmalloc((len + 1) * sizeof(YCHAR), GFP_NOFS);
	if (new_str) {
		strncpy(new_str, str, len);
		new_str[len] = 0;
	}
	return new_str;

}
/*
 *yaffs_update_parent() handles fixing a directories mtime and ctime when a new
 * link (ie. name) is created or deleted in the directory.
 *
 * ie.
 *   create dir/a : update dir's mtime/ctime
 *   rm dir/a:   update dir's mtime/ctime
 *   modify dir/a: don't update dir's mtimme/ctime
 *
 * This can be handled immediately or defered. Defering helps reduce the number
 * of updates when many files in a directory are changed within a brief period.
 *
 * If the directory updating is defered then yaffs_update_dirty_dirs must be
 * called periodically.
 */

/*********************************************************************************************************
** 函数名称: yaffs_update_parent
** 功能描述: 更新指定目录的 ctime 和 mtime，并设置相关标志变量（目前支持“延迟”更新和立即更新），当我们
**         : 在目录中执行了删除对象或者创建对象时需要调用此函数
** 输     入: obj - 需要更新的目录
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_update_parent(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev;

	if (!obj)
		return;
	dev = obj->my_dev;
	obj->dirty = 1;
	yaffs_load_current_time(obj, 0, 1);
	if (dev->param.defered_dir_update) {
		struct list_head *link = &obj->variant.dir_variant.dirty;

		if (list_empty(link)) {
			list_add(link, &dev->dirty_dirs);
			yaffs_trace(YAFFS_TRACE_BACKGROUND,
			  "Added object %d to dirty directories",
			   obj->obj_id);
		}

	} else {
		yaffs_update_oh(obj, NULL, 0, 0, 0, NULL);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_update_dirty_dirs
** 功能描述: 更新当前设备在 dirty_dirs 目录中的所有 dirty dir，dirty_dirs 用于存放那些“延迟”
**         : 更新的 dirty 目录对象，如果我们使用了这种方式，必须周期性调用此函数
** 输     入: dev - yaffs 设备
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_update_dirty_dirs(struct yaffs_dev *dev)
{
	struct list_head *link;
	struct yaffs_obj *obj;
	struct yaffs_dir_var *d_s;
	union yaffs_obj_var *o_v;

	yaffs_trace(YAFFS_TRACE_BACKGROUND, "Update dirty directories");

	while (!list_empty(&dev->dirty_dirs)) {
		link = dev->dirty_dirs.next;
		list_del_init(link);

		d_s = list_entry(link, struct yaffs_dir_var, dirty);
		o_v = list_entry(d_s, union yaffs_obj_var, dir_variant);
		obj = list_entry(o_v, struct yaffs_obj, variant);

		yaffs_trace(YAFFS_TRACE_BACKGROUND, "Update directory %d",
			obj->obj_id);

		if (obj->dirty)
			yaffs_update_oh(obj, NULL, 0, 0, 0, NULL);
	}
}

/*
 * Mknod (create) a new object.
 * equiv_obj only has meaning for a hard link;
 * alias_str only has meaning for a symlink.
 * rdev only has meaning for devices (a subset of special objects)
 */

/*********************************************************************************************************
** 函数名称: yaffs_create_obj
** 功能描述: 在指定目录中创建一个新对象，并把新对象头信息写入到 FLASH 中，同时更新“新对象”的父目录信息
** 输     入: type - 创建对象类型（目录、文件...）
**         : parent - 新创建的对象所在目录
**         : name - 新创建的对象名字
**         : mode - 设置权限信息以及用于区分 FIFO、字符设备、块设备和网络设备
**         : uid - 新创建的用户 ID
**         : gid - 新创建的组 ID
**         : equiv_obj - 如果新创建的对象是硬链接，表示创建的硬链接所指向的目标指针
**         : alias_str - 如果新创建的对象是软链接，表示创建的软链接“路径”字符串
**         : rdev - 如果新创建的对象是设备节点，表示设备节点的设备号（主/次）
** 输     出: 新创建对象的指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static struct yaffs_obj *yaffs_create_obj(enum yaffs_obj_type type,
					  struct yaffs_obj *parent,
					  const YCHAR *name,
					  u32 mode,
					  u32 uid,
					  u32 gid,
					  struct yaffs_obj *equiv_obj,
					  const YCHAR *alias_str, u32 rdev)
{
	struct yaffs_obj *in;
	YCHAR *str = NULL;
	struct yaffs_dev *dev = parent->my_dev;

	/* Check if the entry exists.
	 * If it does then fail the call since we don't want a dup. */
	if (yaffs_find_by_name(parent, name))
		return NULL;

	if (type == YAFFS_OBJECT_TYPE_SYMLINK) {
		str = yaffs_clone_str(alias_str);
		if (!str)
			return NULL;
	}

	in = yaffs_new_obj(dev, -1, type);

	if (!in) {
		kfree(str);
		return NULL;
	}

	in->hdr_chunk = 0;
	in->valid = 1;
	in->variant_type = type;

	in->yst_mode = mode;

	yaffs_attribs_init(in, gid, uid, rdev);

	in->n_data_chunks = 0;

	yaffs_set_obj_name(in, name);
	in->dirty = 1;

	yaffs_add_obj_to_dir(parent, in);

	in->my_dev = parent->my_dev;

	switch (type) {
	case YAFFS_OBJECT_TYPE_SYMLINK:
		in->variant.symlink_variant.alias = str;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		in->variant.hardlink_variant.equiv_obj = equiv_obj;
		in->variant.hardlink_variant.equiv_id = equiv_obj->obj_id;
		list_add(&in->hard_links, &equiv_obj->hard_links);
		break;
	case YAFFS_OBJECT_TYPE_FILE:
	case YAFFS_OBJECT_TYPE_DIRECTORY:
	case YAFFS_OBJECT_TYPE_SPECIAL:
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* do nothing */
		break;
	}

	if (yaffs_update_oh(in, name, 0, 0, 0, NULL) < 0) {
		/* Could not create the object header, fail */
		yaffs_del_obj(in);
		in = NULL;
	}

	if (in)
		yaffs_update_parent(parent);

	return in;
}

struct yaffs_obj *yaffs_create_file(struct yaffs_obj *parent,
				    const YCHAR *name, u32 mode, u32 uid,
				    u32 gid)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_FILE, parent, name, mode,
				uid, gid, NULL, NULL, 0);
}

struct yaffs_obj *yaffs_create_dir(struct yaffs_obj *parent, const YCHAR *name,
				   u32 mode, u32 uid, u32 gid)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_DIRECTORY, parent, name,
				mode, uid, gid, NULL, NULL, 0);
}

struct yaffs_obj *yaffs_create_special(struct yaffs_obj *parent,
				       const YCHAR *name, u32 mode, u32 uid,
				       u32 gid, u32 rdev)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_SPECIAL, parent, name, mode,
				uid, gid, NULL, NULL, rdev);
}

struct yaffs_obj *yaffs_create_symlink(struct yaffs_obj *parent,
				       const YCHAR *name, u32 mode, u32 uid,
				       u32 gid, const YCHAR *alias)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_SYMLINK, parent, name, mode,
				uid, gid, NULL, alias, 0);
}

/* yaffs_link_obj returns the object id of the equivalent object.*/
struct yaffs_obj *yaffs_link_obj(struct yaffs_obj *parent, const YCHAR * name,
				 struct yaffs_obj *equiv_obj)
{
	/* Get the real object in case we were fed a hard link obj */
	equiv_obj = yaffs_get_equivalent_obj(equiv_obj);

	if (yaffs_create_obj(YAFFS_OBJECT_TYPE_HARDLINK,
			parent, name, 0, 0, 0,
			equiv_obj, NULL, 0))
		return equiv_obj;

	return NULL;

}



/*---------------------- Block Management and Page Allocation -------------*/

/*********************************************************************************************************
** 函数名称: yaffs_deinit_blocks
** 功能描述: 释放 block_info 数组和 chunk_bits 占用的内存空间，在设备中，每个 block 在内存中都有一个
**         : block info 存储块状态信息，同理，每个 nand chunk 都有一个 bit 在内存中表示是否已经被使用
** 输 	 入: dev - yaffs 设备
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_deinit_blocks(struct yaffs_dev *dev)
{
	if (dev->block_info_alt && dev->block_info)
		vfree(dev->block_info);
	else
		kfree(dev->block_info);

	dev->block_info_alt = 0;

	dev->block_info = NULL;

	if (dev->chunk_bits_alt && dev->chunk_bits)
		vfree(dev->chunk_bits);
	else
		kfree(dev->chunk_bits);
	dev->chunk_bits_alt = 0;
	dev->chunk_bits = NULL;
}

/*********************************************************************************************************
** 函数名称: yaffs_init_blocks
** 功能描述: 为 block_info 数组和 chunk_bits 申请内存空间并清零，在设备中，每个 block 在内存中都有一个
**         : block info 存储块状态信息，同理，每个 nand chunk 都有一个 bit 在内存中表示是否已经被使用
** 输 	 入: dev - yaffs 设备
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_init_blocks(struct yaffs_dev *dev)
{
	int n_blocks = dev->internal_end_block - dev->internal_start_block + 1;

	dev->block_info = NULL;
	dev->chunk_bits = NULL;
	dev->alloc_block = -1;	/* force it to get a new one */

	/* If the first allocation strategy fails, thry the alternate one */
	dev->block_info =
		kmalloc(n_blocks * sizeof(struct yaffs_block_info), GFP_NOFS);
	if (!dev->block_info) {
		dev->block_info =
		    vmalloc(n_blocks * sizeof(struct yaffs_block_info));
		dev->block_info_alt = 1;
	} else {
		dev->block_info_alt = 0;
	}

	if (!dev->block_info)
		goto alloc_error;

	/* Set up dynamic blockinfo stuff. Round up bytes. */
	dev->chunk_bit_stride = (dev->param.chunks_per_block + 7) / 8;
	dev->chunk_bits =
		kmalloc(dev->chunk_bit_stride * n_blocks, GFP_NOFS);
	if (!dev->chunk_bits) {
		dev->chunk_bits =
		    vmalloc(dev->chunk_bit_stride * n_blocks);
		dev->chunk_bits_alt = 1;
	} else {
		dev->chunk_bits_alt = 0;
	}
	if (!dev->chunk_bits)
		goto alloc_error;


	memset(dev->block_info, 0, n_blocks * sizeof(struct yaffs_block_info));
	memset(dev->chunk_bits, 0, dev->chunk_bit_stride * n_blocks);
	return YAFFS_OK;

alloc_error:
	yaffs_deinit_blocks(dev);
	return YAFFS_FAIL;
}

/*********************************************************************************************************
** 函数名称: yaffs_block_became_dirty
** 功能描述: 在指定设备中擦除一个指定存储块数据、清除对应块的 chunk bit 信息以及更新相关状态变量
**         : 如果擦除失败，则标记指定块为坏块。如果启动了擦除校验功能，则校验是否擦除成功并在校
**         : 验失败的时候打印 log 信息。
** 输 	 入: dev - yaffs 设备
**         : block_no - 需要擦除的 FLASH 块号
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_block_became_dirty(struct yaffs_dev *dev, int block_no)
{
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, block_no);
	int erased_ok = 0;
	u32 i;

	/* If the block is still healthy erase it and mark as clean.
	 * If the block has had a data failure, then retire it.
	 */

	yaffs_trace(YAFFS_TRACE_GC | YAFFS_TRACE_ERASE,
		"yaffs_block_became_dirty block %d state %d %s",
		block_no, bi->block_state,
		(bi->needs_retiring) ? "needs retiring" : "");

	yaffs2_clear_oldest_dirty_seq(dev, bi);

	bi->block_state = YAFFS_BLOCK_STATE_DIRTY;

	/* If this is the block being garbage collected then stop gc'ing */
	if (block_no == (int)dev->gc_block)
		dev->gc_block = 0;

	/* If this block is currently the best candidate for gc
	 * then drop as a candidate */
	if (block_no == (int)dev->gc_dirtiest) {
		dev->gc_dirtiest = 0;
		dev->gc_pages_in_use = 0;
	}

	if (!bi->needs_retiring) {
		yaffs2_checkpt_invalidate(dev);
		erased_ok = yaffs_erase_block(dev, block_no);
		if (!erased_ok) {
			dev->n_erase_failures++;
			yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
			  "**>> Erasure failed %d", block_no);
		}
	}

	/* Verify erasure if needed */
	if (erased_ok &&
	    ((yaffs_trace_mask & YAFFS_TRACE_ERASE) ||
	     !yaffs_skip_verification(dev))) {
		for (i = 0; i < dev->param.chunks_per_block; i++) {
			if (!yaffs_check_chunk_erased(dev,
				block_no * dev->param.chunks_per_block + i)) {
				yaffs_trace(YAFFS_TRACE_ERROR,
					">>Block %d erasure supposedly OK, but chunk %d not erased",
					block_no, i);
			}
		}
	}

	if (!erased_ok) {
		/* We lost a block of free space */
		dev->n_free_chunks -= dev->param.chunks_per_block;
		yaffs_retire_block(dev, block_no);
		yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
			"**>> Block %d retired", block_no);
		return;
	}

	/* Clean it up... */
	bi->block_state = YAFFS_BLOCK_STATE_EMPTY;
	bi->seq_number = 0;
	dev->n_erased_blocks++;
	bi->pages_in_use = 0;
	bi->soft_del_pages = 0;
	bi->has_shrink_hdr = 0;
	bi->skip_erased_check = 1;	/* Clean, so no need to check */
	bi->gc_prioritise = 0;
	bi->has_summary = 0;

	yaffs_clear_chunk_bits(dev, block_no);

	yaffs_trace(YAFFS_TRACE_ERASE, "Erased block %d", block_no);
}

/*********************************************************************************************************
** 函数名称: yaffs_gc_process_chunk
** 功能描述: 在 gc 回收空间的时候，有的场景需要“挑选”有效数据 chunk 比较少的块来回收，在回收这个块的
**         : 空间之前，我们需要处理这个块上所有有效的数据 chunk（把有效数据 chunk 复制出来，然后写到
**         : 其他 nand_chunk 中），然后再回收这个块的空间。这个函数就是实现此功能的
** 输 	 入: dev - yaffs 设备
**         : bi - 需要处理的 chunk 所在块的块信息
**         : old_chunk - 需要处理的 chunk 的 nand_chunk 号
**         : buffer - 函数调用者提供的数据缓冲区，缓冲区空间大小应和 nand chunk 相同
** 输 	 出: YAFFS_OK - 执行成功
**         : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static inline int yaffs_gc_process_chunk(struct yaffs_dev *dev,
					struct yaffs_block_info *bi,
					int old_chunk, u8 *buffer)
{
	int new_chunk;
	int mark_flash = 1;
	struct yaffs_ext_tags tags;
	struct yaffs_obj *object;
	int matching_chunk;
	int ret_val = YAFFS_OK;

	/* 读出需要处理的 nand_chunk 相关数据 */
	memset(&tags, 0, sizeof(tags));
	yaffs_rd_chunk_tags_nand(dev, old_chunk,
				 buffer, &tags);
	object = yaffs_find_by_number(dev, tags.obj_id);

	yaffs_trace(YAFFS_TRACE_GC_DETAIL,
		"Collecting chunk in block %d, %d %d %d ",
		dev->gc_chunk, tags.obj_id,
		tags.chunk_id, tags.n_bytes);

	if (object && !yaffs_skip_verification(dev)) {
		if (tags.chunk_id == 0)
			matching_chunk =
			    object->hdr_chunk;
		else if (object->soft_del)
			/* Defeat the test */
			matching_chunk = old_chunk;
		else
			matching_chunk =
			    yaffs_find_chunk_in_file
			    (object, tags.chunk_id,
			     NULL);

		if (old_chunk != matching_chunk)
			yaffs_trace(YAFFS_TRACE_ERROR,
				"gc: page in gc mismatch: %d %d %d %d",
				old_chunk,
				matching_chunk,
				tags.obj_id,
				tags.chunk_id);
	}

	if (!object) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"page %d in gc has no object: %d %d %d ",
			old_chunk,
			tags.obj_id, tags.chunk_id,
			tags.n_bytes);
	}

	if (object &&
	    object->deleted &&
	    object->soft_del && tags.chunk_id != 0) {
		/* Data chunk in a soft deleted file,
		 * throw it away.
		 * It's a soft deleted data chunk,
		 * No need to copy this, just forget
		 * about it and fix up the object.
		 */

		/* Free chunks already includes
		 * softdeleted chunks, how ever this
		 * chunk is going to soon be really
		 * deleted which will increment free
		 * chunks. We have to decrement free
		 * chunks so this works out properly.
		 */

		/* 如果当前的 nand_chunk 属于一个已经“软删除”对象，我们不需要复制它的数据来存储到新的 nand_chunk 中，直接丢弃
		   即可，然后把当前 nand_chunk 所属对象放到 gc_cleanup_list 中，在后面 gc 回收空间的时候会处理掉 gc_cleanup_list
		   数组中记录的对象所占用的空间 */
		dev->n_free_chunks--;
		bi->soft_del_pages--;

		object->n_data_chunks--;
		if (object->n_data_chunks <= 0) {
			/* remeber to clean up obj */
			dev->gc_cleanup_list[dev->n_clean_ups] = tags.obj_id;
			dev->n_clean_ups++;
		}
		mark_flash = 0;
	} else if (object) {
		/* It's either a data chunk in a live
		 * file or an ObjectHeader, so we're
		 * interested in it.
		 * NB Need to keep the ObjectHeaders of
		 * deleted files until the whole file
		 * has been deleted off
		 */
		tags.serial_number++;  /* serial_number 变量表示数据存储的先后顺序，在挂载 yaffs 的时、扫描文件数据的逻辑会用到 */
		dev->n_gc_copies++;

		if (tags.chunk_id == 0) {
			/* It is an object Id,
			 * We need to nuke the shrinkheader flags since its
			 * work is done.
			 * Also need to clean up shadowing.
			 * NB We don't want to do all the work of translating
			 * object header endianism back and forth so we leave
			 * the oh endian in its stored order.
			 */

			/* 如果当前 chunk 数据所属对象没被删除，并且当前 chunk 存储的是数据头，我们在更新
			   完相关字段后直接写入一个新的 nand_chunk 中即可 */
			struct yaffs_obj_hdr *oh;
			oh = (struct yaffs_obj_hdr *) buffer;

			oh->is_shrink = 0;
			tags.extra_is_shrink = 0;
			oh->shadows_obj = 0;
			oh->inband_shadowed_obj_id = 0;
			tags.extra_shadows = 0;

			/* Update file size */
			if (object->variant_type == YAFFS_OBJECT_TYPE_FILE) {
				yaffs_oh_size_load(dev, oh,
				    object->variant.file_variant.stored_size, 1);
				tags.extra_file_size =
				    object->variant.file_variant.stored_size;
			}

			yaffs_verify_oh(object, oh, &tags, 1);
			new_chunk =
			    yaffs_write_new_chunk(dev, (u8 *) oh, &tags, 1);
		} else {
			/* 如果当前 chunk 数据所属对象没被删除，并且当前 chunk 存储的是文件数据，我们直接
		       把复制出来的数据写入一个新的 nand_chunk 中即可 */
			new_chunk =
			    yaffs_write_new_chunk(dev, buffer, &tags, 1);
		}

		if (new_chunk < 0) {
			ret_val = YAFFS_FAIL;
		} else {

			/* Now fix up the Tnodes etc. */

			if (tags.chunk_id == 0) {
				/* It's a header */
				object->hdr_chunk = new_chunk;
				object->serial = tags.serial_number;
			} else {
				/* It's a data chunk */

				/* 如果是文件数据 chunk，我们在上面的代码中已经把原来的数据写入到一个新的 nand_chunk 中了
				   所以这个地方，我们需要更新 tnode tree 中同一个 inode_chunk 对应的 nand_chunk 值 			   */
				yaffs_put_chunk_in_file(object, tags.chunk_id,
							new_chunk, 0);
			}
		}
	}

	/* 如果把指定的数据复制出来并且已经写入了一个新的 nand_chunk 中，我们需要删除旧的 nand_chunk 数据 */
	if (ret_val == YAFFS_OK)
		yaffs_chunk_del(dev, old_chunk, mark_flash, __LINE__);
	return ret_val;
}

/*********************************************************************************************************
** 函数名称: yaffs_gc_block
** 功能描述: 对一个指定的块执行空间回收操作，如果指定的块空间被回收了，那么还需要处理 gc_cleanup_list
**		   : 数组中的对象，如果他们仍然存在于 obj_bucket 中，表示这是之前“软删除”的对象，我们需要删除
**		   : 这个对象在 FLASH 中的对象头以及释放相关资源（Cache、tnode tree top、obj tnode等）
** 输	 入: dev - yaffs 设备
**		   : block - 需要做回收处理的 nand_block 号
**		   : whole_block - 在我们回收块时，这个块中还存在有效数据，我们需要把这些数据复制到其他地方
**		   : 之后才能擦除这个块，whole_block 表示我们最多可以复制的 chunk 数是否可达到整个块大小
** 输	 出: YAFFS_OK - 执行成功
**		   : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_gc_block(struct yaffs_dev *dev, int block, int whole_block)
{
	int old_chunk;
	int ret_val = YAFFS_OK;
	u32 i;
	int is_checkpt_block;
	int max_copies;
	int chunks_before = yaffs_get_erased_chunks(dev);
	int chunks_after;
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, block);

	is_checkpt_block = (bi->block_state == YAFFS_BLOCK_STATE_CHECKPOINT);

	yaffs_trace(YAFFS_TRACE_TRACING,
		"Collecting block %d, in use %d, shrink %d, whole_block %d",
		block, bi->pages_in_use, bi->has_shrink_hdr,
		whole_block);

	/*yaffs_verify_free_chunks(dev); */

	if (bi->block_state == YAFFS_BLOCK_STATE_FULL)
		bi->block_state = YAFFS_BLOCK_STATE_COLLECTING; /* 调整块信息的状态字段值，这样才执行下面回收块空间的逻辑 */

	bi->has_shrink_hdr = 0;	/* clear the flag so that the block can erase */

	dev->gc_disable = 1;

	yaffs_summary_gc(dev, block);

	if (is_checkpt_block || !yaffs_still_some_chunks(dev, block)) {
		/* 整个块没有有效数据，直接回收这个块的空间 */
		yaffs_trace(YAFFS_TRACE_TRACING,
			"Collecting block %d that has no chunks in use",
			block);
		yaffs_block_became_dirty(dev, block);
	} else {
		/* 当前块存在有效数据，我们需要把这些数据转移到其他地方，然后再“尝试”回收这个块的空间 */
		u8 *buffer = yaffs_get_temp_buffer(dev);

		yaffs_verify_blk(dev, bi, block);

		max_copies = (whole_block) ? dev->param.chunks_per_block : 5;
		old_chunk = block * dev->param.chunks_per_block + dev->gc_chunk;

		for (/* init already done */ ;
		     ret_val == YAFFS_OK &&
		     dev->gc_chunk < dev->param.chunks_per_block &&
		     (bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) &&
		     max_copies > 0;
		     dev->gc_chunk++, old_chunk++) {
			if (yaffs_check_chunk_bit(dev, block, dev->gc_chunk)) {
				/* Page is in use and might need to be copied */
				max_copies--;
				ret_val = yaffs_gc_process_chunk(dev, bi,
							old_chunk, buffer);
			}
		}
		yaffs_release_temp_buffer(dev, buffer);
	}

	yaffs_verify_collected_blk(dev, bi, block);

	if (bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) {
		/*
		 * The gc did not complete. Set block state back to FULL
		 * because checkpointing does not restore gc.
		 */

		/* 如果上面因为当前块上存在有效数据而没有回收这个块的空间，我们需要设置状态为 FULL（因为我们在
		   存储数据的时候，所有合法使用过的数据块的状态都应该是 FULL，表示正常存满了数据） */
		bi->block_state = YAFFS_BLOCK_STATE_FULL;
	} else {
		/* The gc completed. */
		/* Do any required cleanups */

		/* 如果在上面回收了这个块的空间，我们需要检查在回收空间的过程中（yaffs_gc_process_chunk）是否添加了需要处理的
		   成员对象到 gc_cleanup_list 中，如果有，则删除相应的对象 */
		for (i = 0; i < dev->n_clean_ups; i++) {
			/* Time to delete the file too */
			struct yaffs_obj *object =
			    yaffs_find_by_number(dev, dev->gc_cleanup_list[i]);
			if (object) {
				yaffs_free_tnode(dev,
					  object->variant.file_variant.top);
				object->variant.file_variant.top = NULL;
				yaffs_trace(YAFFS_TRACE_GC,
					"yaffs: About to finally delete object %d",
					object->obj_id);
				yaffs_generic_obj_del(object);
				object->my_dev->n_deleted_files--;
			}

		}

		/* 检查执行删除操作前后可用空闲 nand_chunk 空间变化是否合法（是否变少了，如果变少了，表示出现了异常） */
		chunks_after = yaffs_get_erased_chunks(dev);
		if (chunks_before >= chunks_after)
			yaffs_trace(YAFFS_TRACE_GC,
				"gc did not increase free chunks before %d after %d",
				chunks_before, chunks_after);
		dev->gc_block = 0;
		dev->gc_chunk = 0;
		dev->n_clean_ups = 0;
	}

	dev->gc_disable = 0;

	return ret_val;
}

/*
 * find_gc_block() selects the dirtiest block (or close enough)
 * for garbage collection.
 */

/*********************************************************************************************************
** 函数名称: yaffs_find_gc_block
** 功能描述: 从指定设备中查找一个用于回收的块来回收它的空间
**		   : 1. 如果当前设备中有“优先”回收的块，则从 dev->block_info 中找出希望优先回收的块号
**		   : 2. 从当前设备中找出一个块，这个块需满足条件是：a.时间比较久远 b.有效数据 chunk 比较少
**		   : 3. 如果多次（10 or 20）调用此函数都没有执行块回收操作、且 dev->oldest_dirty_block 有效
**		   :    则选择 dev->oldest_dirty_block 此块来回收
** 输	 入: dev - yaffs 设备
**		   : aggressive - 表示回收任务是否“紧急”，如果紧急，在搜索的时候不对搜索返回和块内有效数据
**		   :              chunk 个数做限制
**		   : background - 当前函数是否是后台进程调用的（后台进程回收的“力度”比较低，非紧急任务）
** 输	 出: selected - 查找到用于回收的 nand_block 号
**		   : 0 - 表示没找到用于回收的块
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static unsigned yaffs_find_gc_block(struct yaffs_dev *dev,
				    int aggressive, int background)
{
	u32 i;
	u32 iterations;
	u32 selected = 0;
	int prioritised = 0;
	int prioritised_exist = 0;
	struct yaffs_block_info *bi;
	u32 threshold;

	/* First let's see if we need to grab a prioritised block */
	if (dev->has_pending_prioritised_gc && !aggressive) {
		dev->gc_dirtiest = 0;
		bi = dev->block_info;
		for (i = dev->internal_start_block;
		     i <= dev->internal_end_block && !selected; i++) {

			if (bi->gc_prioritise) {
				prioritised_exist = 1;
				if (bi->block_state == YAFFS_BLOCK_STATE_FULL &&
				    yaffs_block_ok_for_gc(dev, bi)) {
				    /* 表示优先回收的块更加久远（和 dev->oldest_dirty_block 相比） */
					selected = i;
					prioritised = 1;
				}
			}
			bi++;
		}

		/*
		 * If there is a prioritised block and none was selected then
		 * this happened because there is at least one old dirty block
		 * gumming up the works. Let's gc the oldest dirty block.
		 */

		/* 表示有优先回收的块，但是优先回收的块不是最久远的块，我们选择回收更加久远的块 */
		if (prioritised_exist &&
		    !selected && dev->oldest_dirty_block > 0)
			selected = dev->oldest_dirty_block;

		/* 如果当前设备不存在优先回收的块，则清除优先回收标记 */
		if (!prioritised_exist)	/* None found, so we can clear this */
			dev->has_pending_prioritised_gc = 0;
	}

	/* If we're doing aggressive GC then we are happy to take a less-dirty
	 * block, and search harder.
	 * else (leasurely gc), then we only bother to do this if the
	 * block has only a few pages in use.
	 */

	if (!selected) {
		/* 表示没有优先回收的块 */
		u32 pages_used;
		int n_blocks =
		    dev->internal_end_block - dev->internal_start_block + 1;

		/* 设置 threshold 和 iterations 变量的值 
		   threshold - 表示当块中有效数据 chunk 数小于这个值的时候，就可以用来回收了
		   iterations - 表示从当前位置开始，我们搜索多少个块信息来找出适合回收的块 */
		if (aggressive) {
			threshold = dev->param.chunks_per_block;
			iterations = n_blocks;
		} else {
			u32 max_threshold;

			if (background)
				max_threshold = dev->param.chunks_per_block / 2;
			else
				max_threshold = dev->param.chunks_per_block / 8;

			if (max_threshold < YAFFS_GC_PASSIVE_THRESHOLD)
				max_threshold = YAFFS_GC_PASSIVE_THRESHOLD;

			threshold = background ? (dev->gc_not_done + 2) * 2 : 0;
			if (threshold < YAFFS_GC_PASSIVE_THRESHOLD)
				threshold = YAFFS_GC_PASSIVE_THRESHOLD;
			if (threshold > max_threshold)
				threshold = max_threshold;

			iterations = n_blocks / 16 + 1;
			if (iterations > 100)
				iterations = 100;
		}

		for (i = 0;
		     i < iterations &&
		     (dev->gc_dirtiest < 1 ||
		      dev->gc_pages_in_use > YAFFS_GC_GOOD_ENOUGH);
		     i++) {
			dev->gc_block_finder++;
			if (dev->gc_block_finder < dev->internal_start_block ||
			    dev->gc_block_finder > dev->internal_end_block)
				dev->gc_block_finder =
				    dev->internal_start_block;

			bi = yaffs_get_block_info(dev, dev->gc_block_finder);

			/* 有效数据 chunk 不包含那些软删除的 chunk */
			pages_used = bi->pages_in_use - bi->soft_del_pages;

			if (bi->block_state == YAFFS_BLOCK_STATE_FULL &&
			    pages_used < dev->param.chunks_per_block &&
			    (dev->gc_dirtiest < 1 ||
			     pages_used < dev->gc_pages_in_use) &&
			    yaffs_block_ok_for_gc(dev, bi)) {
				dev->gc_dirtiest = dev->gc_block_finder;
				dev->gc_pages_in_use = pages_used;
			}
		}

		if (dev->gc_dirtiest > 0 && dev->gc_pages_in_use <= threshold)
			selected = dev->gc_dirtiest;
	}

	/*
	 * If nothing has been selected for a while, try the oldest dirty
	 * because that's gumming up the works.
	 */

	/* 如果当前设备存在有效的、最久远的块记录信息，并且已经连续多次（10 or 20）
	   调用此函数且没有执行块空间回收操作，那么久选择有效的、最久远的块回收 */
	if (!selected && dev->param.is_yaffs2 &&
	    dev->gc_not_done >= (background ? 10 : 20)) {
		yaffs2_find_oldest_dirty_seq(dev);
		if (dev->oldest_dirty_block > 0) {
			selected = dev->oldest_dirty_block;
			dev->gc_dirtiest = selected;
			dev->oldest_dirty_gc_count++;
			bi = yaffs_get_block_info(dev, selected);
			dev->gc_pages_in_use =
			    bi->pages_in_use - bi->soft_del_pages;
		} else {
			dev->gc_not_done = 0;
		}
	}

	if (selected) {
		yaffs_trace(YAFFS_TRACE_GC,
			"GC Selected block %d with %d free, prioritised:%d",
			selected,
			dev->param.chunks_per_block - dev->gc_pages_in_use,
			prioritised);

		dev->n_gc_blocks++;
		if (background)
			dev->bg_gcs++;

		dev->gc_dirtiest = 0;
		dev->gc_pages_in_use = 0;
		dev->gc_not_done = 0;
		if (dev->refresh_skip > 0)
			dev->refresh_skip--;
	} else {
		dev->gc_not_done++;
		yaffs_trace(YAFFS_TRACE_GC,
			"GC none: finder %d skip %d threshold %d dirtiest %d using %d oldest %d%s",
			dev->gc_block_finder, dev->gc_not_done, threshold,
			dev->gc_dirtiest, dev->gc_pages_in_use,
			dev->oldest_dirty_block, background ? " bg" : "");
	}

	return selected;
}

/* New garbage collector
 * If we're very low on erased blocks then we do aggressive garbage collection
 * otherwise we do "leasurely" garbage collection.
 * Aggressive gc looks further (whole array) and will accept less dirty blocks.
 * Passive gc only inspects smaller areas and only accepts more dirty blocks.
 *
 * The idea is to help clear out space in a more spread-out manner.
 * Dunno if it really does anything useful.
 */

/*********************************************************************************************************
** 函数名称: yaffs_check_gc
** 功能描述: 检查指定设备是否需要执行垃圾回收操作，如果需要，则执行垃圾回收操作
** 输	 入: dev - yaffs 设备
**         : background - 当前函数是否是后台进程调用的（后台进程回收的“力度”比较低，非紧急任务）
** 输	 出: YAFFS_OK - 执行成功
**		   : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_check_gc(struct yaffs_dev *dev, int background)
{
	int aggressive = 0;
	int gc_ok = YAFFS_OK;
	int max_tries = 0;
	int min_erased;
	int erased_chunks;
	int checkpt_block_adjust;

	if (dev->param.gc_control_fn &&
		(dev->param.gc_control_fn(dev) & 1) == 0)
		return YAFFS_OK;

	if (dev->gc_disable)
		/* Bail out so we don't get recursive gc */
		return YAFFS_OK;

	/* This loop should pass the first time.
	 * Only loops here if the collection does not increase space.
	 */

	do {
		max_tries++;

		checkpt_block_adjust = yaffs_calc_checkpt_blocks_required(dev);

		min_erased =
		    dev->param.n_reserved_blocks + checkpt_block_adjust + 1;
		erased_chunks =
		    dev->n_erased_blocks * dev->param.chunks_per_block;

		/* If we need a block soon then do aggressive gc. */
		if (dev->n_erased_blocks < min_erased)
			aggressive = 1;
		else {
			if (!background
			    && erased_chunks > (dev->n_free_chunks / 4))
				break;

			if (dev->gc_skip > 20)
				dev->gc_skip = 20;
			if (erased_chunks < dev->n_free_chunks / 2 ||
			    dev->gc_skip < 1 || background)
				aggressive = 0;
			else {
				dev->gc_skip--;
				break;
			}
		}

		dev->gc_skip = 5;

		/* If we don't already have a block being gc'd then see if we
		 * should start another */

		if (dev->gc_block < 1 && !aggressive) {
			dev->gc_block = yaffs2_find_refresh_block(dev);
			dev->gc_chunk = 0;
			dev->n_clean_ups = 0;
		}

		if (dev->gc_block < 1) {
			dev->gc_block =
			    yaffs_find_gc_block(dev, aggressive, background);
			dev->gc_chunk = 0;
			dev->n_clean_ups = 0;
		}

		if (dev->gc_block > 0) {
			dev->all_gcs++;
			if (!aggressive)
				dev->passive_gc_count++;

			yaffs_trace(YAFFS_TRACE_GC,
				"yaffs: GC n_erased_blocks %d aggressive %d",
				dev->n_erased_blocks, aggressive);

			gc_ok = yaffs_gc_block(dev, dev->gc_block, aggressive);
		}

		if (dev->n_erased_blocks < (int)dev->param.n_reserved_blocks &&
		    dev->gc_block > 0) {
			yaffs_trace(YAFFS_TRACE_GC,
				"yaffs: GC !!!no reclaim!!! n_erased_blocks %d after try %d block %d",
				dev->n_erased_blocks, max_tries,
				dev->gc_block);
		}
	} while ((dev->n_erased_blocks < (int)dev->param.n_reserved_blocks) &&
		 (dev->gc_block > 0) && (max_tries < 2));

	return aggressive ? gc_ok : YAFFS_OK;
}

/*
 * yaffs_bg_gc()
 * Garbage collects. Intended to be called from a background thread.
 * Returns non-zero if at least half the free chunks are erased.
 */

/*********************************************************************************************************
** 函数名称: yaffs_bg_gc
** 功能描述: 垃圾回收后台进程执行函数
** 输	 入: dev - yaffs 设备
**		   : urgency - 表示是否紧急
** 输	 出: ？
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/

int yaffs_bg_gc(struct yaffs_dev *dev, unsigned urgency)
{
	int erased_chunks = dev->n_erased_blocks * dev->param.chunks_per_block;

	yaffs_trace(YAFFS_TRACE_BACKGROUND, "Background gc %u", urgency);

	yaffs_check_gc(dev, 1);
	return erased_chunks > dev->n_free_chunks / 2;
}

/*-------------------- Data file manipulation -----------------*/

/*********************************************************************************************************
** 函数名称: yaffs_rd_data_obj
** 功能描述: 读取指定对象、指定 inode_chunk 的数据到指定缓冲区中
** 输	 入: in - 想要读取的对象指针
**		   : inode_chunk - 想要读指定对象的 inode_chunk 号
**		   : buffer - 由调用者提供的，用于存储读出的数据的缓冲区
** 输	 出: 1 - 读取成功
**		   : 0 - 读取失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_rd_data_obj(struct yaffs_obj *in, int inode_chunk, u8 * buffer)
{
	int nand_chunk = yaffs_find_chunk_in_file(in, inode_chunk, NULL);

	if (nand_chunk >= 0)
		return yaffs_rd_chunk_tags_nand(in->my_dev, nand_chunk,
						buffer, NULL);
	else {
		yaffs_trace(YAFFS_TRACE_NANDACCESS,
			"Chunk %d not found zero instead",
			nand_chunk);
		/* get sane (zero) data if you read a hole */
		memset(buffer, 0, in->my_dev->data_bytes_per_chunk);
		return 0;
	}

}

/*********************************************************************************************************
** 函数名称: yaffs_chunk_del
** 功能描述: 删除一个 nand_chunk 的数据（硬件删除），并更新相关状态变量。
**         : 1. 如果擦除的 nand_chunk 所在块已经全部擦除，则擦除整个块数据，并更新相关状态变量来回收空间
**         : 2. 因为 yaffs2 需要实现 once write，所以 yaffs2 使用的是软删除，而 yaffs1 使用的是回写 FLASH
**         :    标记变量实现删除功能
** 输 	 入: dev - yaffs 设备
**         : chunk_id - 删除的 chunk 的 nand_chunk 号
**         : mark_flash - 是否需要在 nand flash 中标记 chunk 已被删除，在 yaffs1 中使用
**         : lyn - 调用此函数在源码文件中的行数
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_chunk_del(struct yaffs_dev *dev, int chunk_id, int mark_flash,
		     int lyn)
{
	int block;
	int page;
	struct yaffs_ext_tags tags;
	struct yaffs_block_info *bi;

	if (chunk_id <= 0)
		return;

	dev->n_deletions++;
	block = chunk_id / dev->param.chunks_per_block;
	page = chunk_id % dev->param.chunks_per_block;

	if (!yaffs_check_chunk_bit(dev, block, page))
		yaffs_trace(YAFFS_TRACE_VERIFY,
			"Deleting invalid chunk %d", chunk_id);

	bi = yaffs_get_block_info(dev, block);

	yaffs2_update_oldest_dirty_seq(dev, block, bi);

	yaffs_trace(YAFFS_TRACE_DELETION,
		"line %d delete of chunk %d",
		lyn, chunk_id);

	if (!dev->param.is_yaffs2 && mark_flash &&
	    bi->block_state != YAFFS_BLOCK_STATE_COLLECTING) {

		memset(&tags, 0, sizeof(tags));
		tags.is_deleted = 1;
		yaffs_wr_chunk_tags_nand(dev, chunk_id, NULL, &tags);
		yaffs_handle_chunk_update(dev, chunk_id, &tags);
	} else {
		dev->n_unmarked_deletions++;
	}

	/* Pull out of the management area.
	 * If the whole block became dirty, this will kick off an erasure.
	 */
	if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING ||
	    bi->block_state == YAFFS_BLOCK_STATE_FULL ||
	    bi->block_state == YAFFS_BLOCK_STATE_NEEDS_SCAN ||
	    bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) {
		dev->n_free_chunks++;
		yaffs_clear_chunk_bit(dev, block, page);
		bi->pages_in_use--;

		if (bi->pages_in_use == 0 &&
		    !bi->has_shrink_hdr &&
		    bi->block_state != YAFFS_BLOCK_STATE_ALLOCATING &&
		    bi->block_state != YAFFS_BLOCK_STATE_NEEDS_SCAN) {
			yaffs_block_became_dirty(dev, block);
		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_wr_data_obj
** 功能描述: 向指定对象、指定 inode_chunk 中写入 n_bytes 个字节数据
** 输     入: in - 想要写入的对象指针
** 		   : inode_chunk - 想要读指定对象的 inode_chunk 号
** 		   : buffer - 需要写入的数据
** 		   : n_bytes - 
** 		   : use_reserve - 
** 输     出: chunk - 成功写入数据的 nand_chunk 号
**         : -1 or 0 - 写入失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_wr_data_obj(struct yaffs_obj *in, int inode_chunk,
			     const u8 *buffer, int n_bytes, int use_reserve)
{
	/* Find old chunk Need to do this to get serial number
	 * Write new one and patch into tree.
	 * Invalidate old tags.
	 */

	int prev_chunk_id;
	struct yaffs_ext_tags prev_tags;
	int new_chunk_id;
	struct yaffs_ext_tags new_tags;
	struct yaffs_dev *dev = in->my_dev;
	loff_t endpos;

	yaffs_check_gc(dev, 0);

	/* Get the previous chunk at this location in the file if it exists.
	 * If it does not exist then put a zero into the tree. This creates
	 * the tnode now, rather than later when it is harder to clean up.
	 */
	prev_chunk_id = yaffs_find_chunk_in_file(in, inode_chunk, &prev_tags);
	if (prev_chunk_id < 1 &&
	    !yaffs_put_chunk_in_file(in, inode_chunk, 0, 0))
		return 0;

	/* Set up new tags */
	memset(&new_tags, 0, sizeof(new_tags));

	new_tags.chunk_id = inode_chunk;
	new_tags.obj_id = in->obj_id;
	new_tags.serial_number =
	    (prev_chunk_id > 0) ? prev_tags.serial_number + 1 : 1;
	new_tags.n_bytes = n_bytes;

	if (n_bytes < 1 || n_bytes > (int)dev->data_bytes_per_chunk) {
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "Writing %d bytes to chunk!!!!!!!!!",
		   n_bytes);
		BUG();
	}

	/*
	 * If this is a data chunk and the write goes past the end of the stored
	 * size then update the stored_size.
	 */
	if (inode_chunk > 0) {
		endpos =  (inode_chunk - 1) * dev->data_bytes_per_chunk +
				n_bytes;
		if (in->variant.file_variant.stored_size < endpos)
			in->variant.file_variant.stored_size = endpos;
	}

	new_chunk_id =
	    yaffs_write_new_chunk(dev, buffer, &new_tags, use_reserve);

	if (new_chunk_id > 0) {
		yaffs_put_chunk_in_file(in, inode_chunk, new_chunk_id, 0);

		if (prev_chunk_id > 0)
			yaffs_chunk_del(dev, prev_chunk_id, 1, __LINE__);

		yaffs_verify_file_sane(in);
	}
	return new_chunk_id;
}

static int yaffs_do_xattrib_mod(struct yaffs_obj *obj, int set,
				const YCHAR *name, const void *value, int size,
				int flags)
{
	struct yaffs_xattr_mod xmod;
	int result;

	xmod.set = set;
	xmod.name = name;
	xmod.data = value;
	xmod.size = size;
	xmod.flags = flags;
	xmod.result = -ENOSPC;

	result = yaffs_update_oh(obj, NULL, 0, 0, 0, &xmod);

	if (result > 0)
		return xmod.result;
	else
		return -ENOSPC;
}

static int yaffs_apply_xattrib_mod(struct yaffs_obj *obj, char *buffer,
				   struct yaffs_xattr_mod *xmod)
{
	int retval = 0;
	int x_offs = sizeof(struct yaffs_obj_hdr);
	struct yaffs_dev *dev = obj->my_dev;
	int x_size = dev->data_bytes_per_chunk - sizeof(struct yaffs_obj_hdr);
	char *x_buffer = buffer + x_offs;

	if (xmod->set)
		retval =
		    nval_set(dev, x_buffer, x_size, xmod->name, xmod->data,
			     xmod->size, xmod->flags);
	else
		retval = nval_del(dev, x_buffer, x_size, xmod->name);

	obj->has_xattr = nval_hasvalues(dev, x_buffer, x_size);
	obj->xattr_known = 1;
	xmod->result = retval;

	return retval;
}

static int yaffs_do_xattrib_fetch(struct yaffs_obj *obj, const YCHAR *name,
				  void *value, int size)
{
	char *buffer = NULL;
	int result;
	struct yaffs_ext_tags tags;
	struct yaffs_dev *dev = obj->my_dev;
	int x_offs = sizeof(struct yaffs_obj_hdr);
	int x_size = dev->data_bytes_per_chunk - sizeof(struct yaffs_obj_hdr);
	char *x_buffer;
	int retval = 0;

	if (obj->hdr_chunk < 1)
		return -ENODATA;

	/* If we know that the object has no xattribs then don't do all the
	 * reading and parsing.
	 */
	if (obj->xattr_known && !obj->has_xattr) {
		if (name)
			return -ENODATA;
		else
			return 0;
	}

	buffer = (char *)yaffs_get_temp_buffer(dev);
	if (!buffer)
		return -ENOMEM;

	result =
	    yaffs_rd_chunk_tags_nand(dev, obj->hdr_chunk, (u8 *) buffer, &tags);

	if (result != YAFFS_OK)
		retval = -ENOENT;
	else {
		x_buffer = buffer + x_offs;

		if (!obj->xattr_known) {
			obj->has_xattr = nval_hasvalues(dev, x_buffer, x_size);
			obj->xattr_known = 1;
		}

		if (name)
			retval = nval_get(dev, x_buffer, x_size,
						name, value, size);
		else
			retval = nval_list(dev, x_buffer, x_size, value, size);
	}
	yaffs_release_temp_buffer(dev, (u8 *) buffer);
	return retval;
}

int yaffs_set_xattrib(struct yaffs_obj *obj, const YCHAR * name,
		      const void *value, int size, int flags)
{
	return yaffs_do_xattrib_mod(obj, 1, name, value, size, flags);
}

int yaffs_remove_xattrib(struct yaffs_obj *obj, const YCHAR * name)
{
	return yaffs_do_xattrib_mod(obj, 0, name, NULL, 0, 0);
}

int yaffs_get_xattrib(struct yaffs_obj *obj, const YCHAR * name, void *value,
		      int size)
{
	return yaffs_do_xattrib_fetch(obj, name, value, size);
}

int yaffs_list_xattrib(struct yaffs_obj *obj, char *buffer, int size)
{
	return yaffs_do_xattrib_fetch(obj, NULL, buffer, size);
}

/*********************************************************************************************************
** 函数名称: yaffs_check_obj_details_loaded
** 功能描述: 从 FLASH 中获取指定对象的头信息并把其中一些属性字段设置到输入对象的相应字段中
** 输     入: in - 需要设置属性信息的对象指针
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_check_obj_details_loaded(struct yaffs_obj *in)
{
	u8 *buf;
	struct yaffs_obj_hdr *oh;
	struct yaffs_dev *dev;
	struct yaffs_ext_tags tags;
	int result;

	if (!in || !in->lazy_loaded || in->hdr_chunk < 1)
		return;

	dev = in->my_dev;
	buf = yaffs_get_temp_buffer(dev);

	result = yaffs_rd_chunk_tags_nand(dev, in->hdr_chunk, buf, &tags);

	if (result == YAFFS_FAIL)
		return;

	oh = (struct yaffs_obj_hdr *)buf;

	yaffs_do_endian_oh(dev, oh);

	in->lazy_loaded = 0;
	in->yst_mode = oh->yst_mode;
	yaffs_load_attribs(in, oh);
	yaffs_set_obj_name_from_oh(in, oh);

	if (in->variant_type == YAFFS_OBJECT_TYPE_SYMLINK)
		in->variant.symlink_variant.alias =
		    yaffs_clone_str(oh->alias);
	yaffs_release_temp_buffer(dev, buf);
}

/* UpdateObjectHeader updates the header on NAND for an object.
 * If name is not NULL, then that new name is used.
 *
 * We're always creating the obj header from scratch (except reading
 * the old name) so first set up in cpu endianness then run it through
 * endian fixing at the end.
 *
 * However, a twist: If there are xattribs we leave them as they were.
 *
 * Careful! The buffer holds the whole chunk. Part of the chunk holds the
 * object header and the rest holds the xattribs, therefore we use a buffer
 * pointer and an oh pointer to point to the same memory.
 */

/*********************************************************************************************************
** 函数名称: yaffs_update_oh
** 功能描述: 更新指定对象的头信息，并写入到 FLASH 中（在更新头信息之前做了垃圾回收检查操作）
** 输     入: in - 需要更新头信息的对象
**         : name - 更新对象的新名字
**         : force - 
**         : is_shrink - 表示新的有信息是否是“收缩”头，收缩头在空洞文件中会用到
**         : shadows - This object header shadows the specified object if > 0
**         : xmod - 
** 输     出: new_chunk_id - 存储新头信息的 nand_chunk
**         : < 0 - 表示更新失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_update_oh(struct yaffs_obj *in, const YCHAR *name, int force,
		    int is_shrink, int shadows, struct yaffs_xattr_mod *xmod)
{

	struct yaffs_block_info *bi;
	struct yaffs_dev *dev = in->my_dev;
	int prev_chunk_id;
	int ret_val = 0;
	int result = 0;
	int new_chunk_id;
	struct yaffs_ext_tags new_tags;
	struct yaffs_ext_tags old_tags;
	const YCHAR *alias = NULL;
	u8 *buffer = NULL;
	YCHAR old_name[YAFFS_MAX_NAME_LENGTH + 1];
	struct yaffs_obj_hdr *oh = NULL;
	loff_t file_size = 0;

	strcpy(old_name, _Y("silly old name"));

	if (in->fake && in != dev->root_dir && !force && !xmod)
		return ret_val;

	yaffs_check_gc(dev, 0);
	yaffs_check_obj_details_loaded(in);

	buffer = yaffs_get_temp_buffer(in->my_dev);
	oh = (struct yaffs_obj_hdr *)buffer;

	prev_chunk_id = in->hdr_chunk;

	if (prev_chunk_id > 0) {
		/* Access the old obj header just to read the name. */
		result = yaffs_rd_chunk_tags_nand(dev, prev_chunk_id,
						  buffer, &old_tags);
		if (result == YAFFS_OK) {
			yaffs_verify_oh(in, oh, &old_tags, 0);
			memcpy(old_name, oh->name, sizeof(oh->name));

			/*
			* NB We only wipe the object header area because the rest of
			* the buffer might contain xattribs.
			*/
			memset(oh, 0xff, sizeof(*oh));
		}
	} else {
		memset(buffer, 0xff, dev->data_bytes_per_chunk);
	}

	oh->type = in->variant_type;
	oh->yst_mode = in->yst_mode;
	oh->shadows_obj = oh->inband_shadowed_obj_id = shadows;

	yaffs_load_attribs_oh(oh, in);

	if (in->parent)
		oh->parent_obj_id = in->parent->obj_id;
	else
		oh->parent_obj_id = 0;

	if (name && *name) {
		memset(oh->name, 0, sizeof(oh->name));
		yaffs_load_oh_from_name(dev, oh->name, name);
	} else if (prev_chunk_id > 0) {
		memcpy(oh->name, old_name, sizeof(oh->name));
	} else {
		memset(oh->name, 0, sizeof(oh->name));
	}

	oh->is_shrink = is_shrink;

	switch (in->variant_type) {
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* Should not happen */
		break;
	case YAFFS_OBJECT_TYPE_FILE:
		if (oh->parent_obj_id != YAFFS_OBJECTID_DELETED &&
		    oh->parent_obj_id != YAFFS_OBJECTID_UNLINKED)
			file_size = in->variant.file_variant.stored_size;
		yaffs_oh_size_load(dev, oh, file_size, 0);
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		oh->equiv_id = in->variant.hardlink_variant.equiv_id;
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		/* Do nothing */
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		/* Do nothing */
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		alias = in->variant.symlink_variant.alias;
		if (!alias)
			alias = _Y("no alias");
		strncpy(oh->alias, alias, YAFFS_MAX_ALIAS_LENGTH);
		oh->alias[YAFFS_MAX_ALIAS_LENGTH] = 0;
		break;
	}

	/* process any xattrib modifications */
	if (xmod)
		yaffs_apply_xattrib_mod(in, (char *)buffer, xmod);

	/* Tags */
	memset(&new_tags, 0, sizeof(new_tags));
	in->serial++;
	new_tags.chunk_id = 0;
	new_tags.obj_id = in->obj_id;
	new_tags.serial_number = in->serial;

	/* Add extra info for file header */
	new_tags.extra_available = 1;
	new_tags.extra_parent_id = oh->parent_obj_id;
	new_tags.extra_file_size = file_size;
	new_tags.extra_is_shrink = oh->is_shrink;
	new_tags.extra_equiv_id = oh->equiv_id;
	new_tags.extra_shadows = (oh->shadows_obj > 0) ? 1 : 0;
	new_tags.extra_obj_type = in->variant_type;

	/* Now endian swizzle the oh if needed. */
	yaffs_do_endian_oh(dev, oh);

	yaffs_verify_oh(in, oh, &new_tags, 1);

	/* Create new chunk in NAND */
	new_chunk_id =
	    yaffs_write_new_chunk(dev, buffer, &new_tags,
				  (prev_chunk_id > 0) ? 1 : 0);

	if (buffer)
		yaffs_release_temp_buffer(dev, buffer);

	if (new_chunk_id < 0)
		return new_chunk_id;

	in->hdr_chunk = new_chunk_id;

	if (prev_chunk_id > 0)
		yaffs_chunk_del(dev, prev_chunk_id, 1, __LINE__);

	if (!yaffs_obj_cache_dirty(in))
		in->dirty = 0;

	/* If this was a shrink, then mark the block
	 * that the chunk lives on */
	if (is_shrink) {
		bi = yaffs_get_block_info(in->my_dev,
					  new_chunk_id /
					  in->my_dev->param.chunks_per_block);
		bi->has_shrink_hdr = 1;
	}


	return new_chunk_id;
}

/*--------------------- File read/write ------------------------
 * Read and write have very similar structures.
 * In general the read/write has three parts to it
 * An incomplete chunk to start with (if the read/write is not chunk-aligned)
 * Some complete chunks
 * An incomplete chunk to end off with
 *
 * Curve-balls: the first chunk might also be the last chunk.
 */

/*********************************************************************************************************
** 函数名称: yaffs_file_rd
** 功能描述: 从指定的文件的指定位置读取 n_bytes 字节数据，在读取数据的逻辑如下：
**         : 1. 处理在读取数据时出现的 chunk 地址不对齐问题
**         : 2. 判断在 Cache 中是否存在要读取的数据，如果命中，直接从 Cache 中读取
**         : 3. a. 如果我们只读取一个 nand_chunk          中的部分数据、开启了 Cache 功能且 Cache 不命中，则需要申请
**         :       一个空闲 Cache 空间，读取指定 nand_chunk 数据到 Cache 中，然后从 Cache 中复制需要的数据
**         :       到用户缓冲区中
**         :    b. 如果我们只读取一个 nand_chunk          中的部分数据、没开启 Cache 功能，则需要从设备中申请一个临时
**         :       数据缓冲区，然后读取指定 nand_chunk 数据到临时缓冲区中，然后从临时缓冲区中复制需要的数据
**         :       到用户缓冲区中并释放临时缓冲区
**         : 4. a. 如果我们设置了 dev->param.inband_tags 标志、开启了 Cache 功能且 Cache 不命中，则需要申请
**         :       一个空闲 Cache 空间，读取指定 nand_chunk 数据到 Cache 中，然后从 Cache 中复制需要的数据
**         :       到用户缓冲区中
**         :    b. 如果我们设置了 dev->param.inband_tags 标志、没开启 Cache 功能，则需要从设备中申请一个临时
**         :       数据缓冲区，然后读取指定 nand_chunk 数据到临时缓冲区中，然后从临时缓冲区中复制需要的数据
**         :       到用户缓冲区中并释放临时缓冲区
**         : 5. 如果我们读取的是整个 nand_chunk 数据，则直接到 FLASH 中读取数据到用户缓冲区中
** 输     入: in - 要读的文件指针
**         : buffer - 存储读出数据的缓冲区
**         : offset - 读取数据在文件中的偏移
**         : n_bytes - 要读取的数据长度
** 输     出: n_done - 实际读取的数据长度
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_file_rd(struct yaffs_obj *in, u8 * buffer, loff_t offset, int n_bytes)
{
	int chunk;
	u32 start;
	int n_copy;
	int n = n_bytes;
	int n_done = 0;
	struct yaffs_cache *cache;
	struct yaffs_dev *dev;

	dev = in->my_dev;

	while (n > 0) {
		yaffs_addr_to_chunk(dev, offset, &chunk, &start);
		chunk++;

		/* OK now check for the curveball where the start and end are in
		 * the same chunk.
		 */
		if ((start + n) < dev->data_bytes_per_chunk)
			n_copy = n;
		else
			n_copy = dev->data_bytes_per_chunk - start;

		cache = yaffs_find_chunk_cache(in, chunk);

		/* If the chunk is already in the cache or it is less than
		 * a whole chunk or we're using inband tags then use the cache
		 * (if there is caching) else bypass the cache.
		 */
		if (cache || n_copy != (int)dev->data_bytes_per_chunk ||
		    dev->param.inband_tags) {
			if (dev->param.n_caches > 0) {

				/* If we can't find the data in the cache,
				 * then load it up. */

				if (!cache) {
					cache =
					    yaffs_grab_chunk_cache(in->my_dev);
					cache->object = in;
					cache->chunk_id = chunk;
					cache->dirty = 0;
					cache->locked = 0;
					yaffs_rd_data_obj(in, chunk,
							  cache->data);
					cache->n_bytes = 0;
				}

				yaffs_use_cache(dev, cache, 0);

				cache->locked = 1;

				memcpy(buffer, &cache->data[start], n_copy);

				cache->locked = 0;
			} else {
				/* Read into the local buffer then copy.. */

				u8 *local_buffer =
				    yaffs_get_temp_buffer(dev);
				yaffs_rd_data_obj(in, chunk, local_buffer);

				memcpy(buffer, &local_buffer[start], n_copy);

				yaffs_release_temp_buffer(dev, local_buffer);
			}
		} else {
			/* A full chunk. Read directly into the buffer. */
			yaffs_rd_data_obj(in, chunk, buffer);
		}
		n -= n_copy;
		offset += n_copy;
		buffer += n_copy;
		n_done += n_copy;
	}
	return n_done;
}

/*********************************************************************************************************
** 函数名称: yaffs_do_file_wr
** 功能描述: 向指定的文件的指定位置写入 n_bytes 字节数据，写入数据的逻辑如下：
**         : 1. 处理在写入数据时出现的 chunk 地址不对齐问题
**         : 2. 首先处理的是在执行写文件数据时，是否会出现“跨” nand_chunk 情况，分别如下：
**         :    a. 如果剩余的、需要写入数据不足一个 nand_chunk 且不会把当前 nand_chunk 写满时（只会修改当
**         :       前 nand_chunk 的中间部分数据），并且我们写入的数据会覆盖原有数据（出现文件修改），那么
**         :       在执行 读 -> 改 -> 写 时需要保证不会出现数据丢失，即满足在回写数据时，要保证新写入的数
**         :       据和 FLASH 原有有效数据都被回写（原有有效数据指的是当前 nand_chunk 结尾部分、那些没被修
**         :       改的原有数据）
**         :    b. 如果当前操作存在“跨” nand_chunk 情况，只需按照 nand_chunk 对其处理即可
**         : 4. 上面的步骤已经处理了“跨” nand_chunk 的情况，接下来需要处理“单次”分解后写入的数据字节数是否
**         :    达到一个 nand_chunk 的情况，分别如下：
**         :    a. 如果单次写入“不足”一个 nand_chunk 或者没设置 dev->param.cache_bypass_aligned 标志或者设
**         :       置了 dev->param.inband_tags 标志的情况，一共有两种情况需要处理，分别如下：
**         :       @1 如果我们开启了 Cache 功能，判断在 Cache 中是否存在要写入 nand_chunk 的数据
                      ①. 如果不命中但是 FLASH 上有可用空间存储新数据，我们申请一个新的 Cache，并读出需要写
**         :             入的 nand_chunk 的数据到 Cache 中，然后通过 读 -> 改 -> 写 操作，把新写入的数据更
**         :             新到 Cache 中，如果是 write_through 模式，还需同步 Cache 数据到 FLASH 中
**         :          ②. 如果命中，但是 FLASH 上“没有”可用空间存储新数据，直接返回写操作失败
**         :       @2 如果我们没开启 Cache 功能，则需要从设备中申请一个临时数据缓冲区，通过临时数据缓冲区进
**         :          读 -> 改 -> 写 操作，把新写入的数据更新到 FLASH 中并释放临时数据缓冲区
**         :    b. 如果单次写入字节数达到了一个 nand_chunk，则直接把用户缓冲区数据写入 FLASH 中，并且把当
**         :       前 nand_chunk 对应的 Cache Invalid 掉
** 输     入: in - 要写的文件指针
**         : buffer - 存储写入数据的缓冲区
**         : offset - 写入数据在文件中的偏移
**         : n_bytes - 要写入的数据长度
**         : write_through - 是否同步写数据到 FLASH 中
** 输     出: n_done - 实际写入的数据长度
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_do_file_wr(struct yaffs_obj *in, const u8 *buffer, loff_t offset,
		     int n_bytes, int write_through)
{

	int chunk;
	u32 start;
	int n_copy;
	int n = n_bytes;
	int n_done = 0;
	int n_writeback;
	loff_t start_write = offset;
	int chunk_written = 0;
	u32 n_bytes_read;
	loff_t chunk_start;
	struct yaffs_dev *dev;

	dev = in->my_dev;

	while (n > 0 && chunk_written >= 0) {
		yaffs_addr_to_chunk(dev, offset, &chunk, &start);

		if (((loff_t)chunk) *
		    dev->data_bytes_per_chunk + start != offset ||
		    start >= dev->data_bytes_per_chunk) {
			yaffs_trace(YAFFS_TRACE_ERROR,
				"AddrToChunk of offset %lld gives chunk %d start %d",
				(long  long)offset, chunk, start);
		}
		chunk++;	/* File pos to chunk in file offset */

		/* OK now check for the curveball where the start and end are in
		 * the same chunk.
		 */

		/* 初始化 n_copy 和 n_writeback 变量值，他们分别表示的含义如下：
		   n_copy - 表示我们新写入的数据长度 
		   n_writeback - 表示本次写入操作需要向 FLASH 中实际回写的数据长度 */
		if ((start + n) < dev->data_bytes_per_chunk) {
			n_copy = n;

			/* Now calculate how many bytes to write back....
			 * If we're overwriting and not writing to then end of
			 * file then we need to write back as much as was there
			 * before.
			 */

			chunk_start = (((loff_t)(chunk - 1)) *
					dev->data_bytes_per_chunk);

			/* 如果我们写入数据位置已经存在数据，表示有部分数据需要修改，所以要要执行的操作流程为 读 -> 改 -> 写 */
			if (chunk_start > in->variant.file_variant.file_size)
				n_bytes_read = 0;	/* Past end of file */
			else
				n_bytes_read =
				    in->variant.file_variant.file_size -
				    chunk_start;

			/* 如果执行到这个地方，说明我们需要写入的数据不超过一个 nand_chunk 所以 n_bytes_read
			   最大等于 dev->data_bytes_per_chunk */
			if (n_bytes_read > dev->data_bytes_per_chunk)
				n_bytes_read = dev->data_bytes_per_chunk;

			/* 因为   n_bytes_read 和 start + n 表示的是从一个 nand_chunk 起始地址开始需要写入的字节数，
			   所以 n_writeback 只需取其中较大的值即可 */
			n_writeback =
			    (n_bytes_read >
			     (start + n)) ? n_bytes_read : (start + n);

			if (n_writeback < 0 ||
			    n_writeback > (int)dev->data_bytes_per_chunk)
				BUG();

		} else {
			n_copy = dev->data_bytes_per_chunk - start;
			n_writeback = dev->data_bytes_per_chunk;
		}

		if (n_copy != (int)dev->data_bytes_per_chunk ||
		    !dev->param.cache_bypass_aligned ||
		    dev->param.inband_tags) {
			/* An incomplete start or end chunk (or maybe both
			 * start and end chunk), or we're using inband tags,
			 * or we're forcing writes through the cache,
			 * so we want to use the cache buffers.
			 */
			if (dev->param.n_caches > 0) {
				struct yaffs_cache *cache;

				/* If we can't find the data in the cache, then
				 * load the cache */
				cache = yaffs_find_chunk_cache(in, chunk);

				if (!cache &&
				    yaffs_check_alloc_available(dev, 1)) {
				    /* 如果 Cache 不命中，且开启了 Cache 功能，我们需要把指定的 nand_chunk 数据缓存到 Cache 中 */
					cache = yaffs_grab_chunk_cache(dev);
					cache->object = in;
					cache->chunk_id = chunk;
					cache->dirty = 0;
					cache->locked = 0;
					yaffs_rd_data_obj(in, chunk,
							  cache->data);
				} else if (cache &&
					   !cache->dirty &&
					   !yaffs_check_alloc_available(dev,
									1)) {
					/* Drop the cache if it was a read cache
					 * item and no space check has been made
					 * for it.
					 */
					cache = NULL;
				}

				if (cache) {
					yaffs_use_cache(dev, cache, 1);
					cache->locked = 1;

					memcpy(&cache->data[start], buffer,
					       n_copy);

					cache->locked = 0;
					cache->n_bytes = n_writeback;

					if (write_through) {
						chunk_written =
						    yaffs_wr_data_obj
						    (cache->object,
						     cache->chunk_id,
						     cache->data,
						     cache->n_bytes, 1);
						cache->dirty = 0;
					}
				} else {
					chunk_written = -1;	/* fail write */
				}
			} else {
				/* An incomplete start or end chunk (or maybe
				 * both start and end chunk). Read into the
				 * local buffer then copy over and write back.
				 */

				u8 *local_buffer = yaffs_get_temp_buffer(dev);

				yaffs_rd_data_obj(in, chunk, local_buffer);
				memcpy(&local_buffer[start], buffer, n_copy);

				chunk_written =
				    yaffs_wr_data_obj(in, chunk,
						      local_buffer,
						      n_writeback, 0);

				yaffs_release_temp_buffer(dev, local_buffer);
			}
		} else {
			/* A full chunk. Write directly from the buffer. */

			chunk_written =
			    yaffs_wr_data_obj(in, chunk, buffer,
					      dev->data_bytes_per_chunk, 0);

			/* Since we've overwritten the cached data,
			 * we better invalidate it. */
			yaffs_invalidate_chunk_cache(in, chunk);
		}

		if (chunk_written >= 0) {
			n -= n_copy;
			offset += n_copy;
			buffer += n_copy;
			n_done += n_copy;
		}
	}

	/* Update file object */

	if ((start_write + n_done) > in->variant.file_variant.file_size)
		in->variant.file_variant.file_size = (start_write + n_done);

	in->dirty = 1;
	return n_done;
}

/*********************************************************************************************************
** 函数名称: yaffs_wr_file
** 功能描述: 向指定文件、指定的位置写入 n_bytes 字节数据，同时处理了“空洞”文件的情况
** 输     入: in - 要写的文件指针
**         : buffer - 存储写入数据的缓冲区
**         : offset - 写入数据在文件中的偏移
**         : n_bytes - 要写入的数据长度
**         : write_through - 是否同步写数据到 FLASH 中
** 输     出: 实际写入的数据长度
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_wr_file(struct yaffs_obj *in, const u8 *buffer, loff_t offset,
		  int n_bytes, int write_through)
{
	yaffs2_handle_hole(in, offset);
	return yaffs_do_file_wr(in, buffer, offset, n_bytes, write_through);
}

/* ---------------------- File resizing stuff ------------------ */

/*********************************************************************************************************
** 函数名称: yaffs_prune_chunks
** 功能描述: 如果我们把一个文件的大小变小了，那么需要删除那些“无效数据”对应 nand_chunk 上的无效数据
**		   : 这个函数就是实现这个功能的
** 输	 入: in - 需要处理的文件对象
**		   : new_size - 新的文件大小
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_prune_chunks(struct yaffs_obj *in, loff_t new_size)
{

	struct yaffs_dev *dev = in->my_dev;
	loff_t old_size = in->variant.file_variant.file_size;
	int i;
	int chunk_id;
	u32 dummy;
	int last_del;
	int start_del;

	if (old_size > 0)
		yaffs_addr_to_chunk(dev, old_size - 1, &last_del, &dummy);
	else
		last_del = 0;

	yaffs_addr_to_chunk(dev, new_size + dev->data_bytes_per_chunk - 1,
				&start_del, &dummy);
	last_del++;
	start_del++;

	/* Delete backwards so that we don't end up with holes if
	 * power is lost part-way through the operation.
	 */
	for (i = last_del; i >= start_del; i--) {
		/* NB this could be optimised somewhat,
		 * eg. could retrieve the tags and write them without
		 * using yaffs_chunk_del
		 */

		chunk_id = yaffs_find_del_file_chunk(in, i, NULL);

		if (chunk_id < 1)
			continue;

		if ((u32)chunk_id <
		    (dev->internal_start_block * dev->param.chunks_per_block) ||
		    (u32)chunk_id >=
		    ((dev->internal_end_block + 1) *
		      dev->param.chunks_per_block)) {
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"Found daft chunk_id %d for %d",
				chunk_id, i);
		} else {
			in->n_data_chunks--;
			yaffs_chunk_del(dev, chunk_id, 1, __LINE__);
		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_resize_file_down
** 功能描述: 缩小指定文件大小，并删除 nand_chunk 上对应的无效数据以及 tnode tree 上无效的 tnode 节点
**		   : 在执行缩小文件操作的时候，不需要 nand_chunk 对其
** 输	 入: in - 需要处理的文件对象
**		   : new_size - 新的文件大小
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_resize_file_down(struct yaffs_obj *obj, loff_t new_size)
{
	int new_full;
	u32 new_partial;
	struct yaffs_dev *dev = obj->my_dev;

	yaffs_addr_to_chunk(dev, new_size, &new_full, &new_partial);

	yaffs_prune_chunks(obj, new_size);

	if (new_partial != 0) {
		int last_chunk = 1 + new_full;
		u8 *local_buffer = yaffs_get_temp_buffer(dev);

		/* Rewrite the last chunk with its new size and zero pad */
		yaffs_rd_data_obj(obj, last_chunk, local_buffer);
		memset(local_buffer + new_partial, 0,
		       dev->data_bytes_per_chunk - new_partial);

		yaffs_wr_data_obj(obj, last_chunk, local_buffer,
				  new_partial, 1);

		yaffs_release_temp_buffer(dev, local_buffer);
	}

	obj->variant.file_variant.file_size = new_size;
	obj->variant.file_variant.stored_size = new_size;

	yaffs_prune_tree(dev, &obj->variant.file_variant);
}

/*********************************************************************************************************
** 函数名称: yaffs_resize_file
** 功能描述: 重新设置文件大小
**		   : 1. 如果文件变大了，则设置其为空洞文件
**		   : 2. 如果文件变小了，则删除无用数据在 nand_chunk 上的无效数据以及删除 tnode tree 上无效 tnode
**		   : 3. 把新的文件大小信息更新到与其对应的头信息中
**		   : 4. 在处理文件大小前，做了 Cache flush、Cache invalid 以及垃圾回收检查操作
** 输	 入: in - 需要处理的文件对象
**		   : new_size - 新的文件大小
** 输	 出: YAFFS_OK - 执行成功
**		   : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_resize_file(struct yaffs_obj *in, loff_t new_size)
{
	struct yaffs_dev *dev = in->my_dev;
	loff_t old_size = in->variant.file_variant.file_size;

	yaffs_flush_file_cache(in, 1);
	yaffs_invalidate_whole_cache(in);

	yaffs_check_gc(dev, 0);

	if (in->variant_type != YAFFS_OBJECT_TYPE_FILE)
		return YAFFS_FAIL;

	if (new_size == old_size)
		return YAFFS_OK;

	if (new_size > old_size) {
		yaffs2_handle_hole(in, new_size);
		in->variant.file_variant.file_size = new_size;
	} else {
		/* new_size < old_size */
		yaffs_resize_file_down(in, new_size);
	}

	/* Write a new object header to reflect the resize.
	 * show we've shrunk the file, if need be
	 * Do this only if the file is not in the deleted directories
	 * and is not shadowed.
	 */
	if (in->parent &&
	    !in->is_shadowed &&
	    in->parent->obj_id != YAFFS_OBJECTID_UNLINKED &&
	    in->parent->obj_id != YAFFS_OBJECTID_DELETED)
		yaffs_update_oh(in, NULL, 0, 0, 0, NULL);

	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_flush_file
** 功能描述: 刷新指定文件的 Cache 数据到 FLASH 中
** 输	 入: in - 需要处理的文件对象
**		   : update_time - 是否需要更新文件时间信息
**		   : data_sync - 是否只执行数据刷新操作
**		   : discard_cache - 刷数据后是否需要清除掉当前文件在 Cache 中的所有数据
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_flush_file(struct yaffs_obj *in,
		     int update_time,
		     int data_sync,
		     int discard_cache)
{
	if (!in->dirty)
		return YAFFS_OK;

	yaffs_flush_file_cache(in, discard_cache);

	if (data_sync)
		return YAFFS_OK;

	if (update_time)
		yaffs_load_current_time(in, 0, 0);

	return (yaffs_update_oh(in, NULL, 0, 0, 0, NULL) >= 0) ?
				YAFFS_OK : YAFFS_FAIL;
}


/* yaffs_del_file deletes the whole file data
 * and the inode associated with the file.
 * It does not delete the links associated with the file.
 */

/*********************************************************************************************************
** 函数名称: yaffs_unlink_file_if_needed
** 功能描述: 删除指定的文件，如果这个文件没有 yaffsfs_Inode 信息，则直接从 FLASH 中删除，如果有
**         : yaffsfs_Inode 信息，则把这个文件“移至” unlinked 目录中
** 输	 入: in - 需要删除的文件对象
** 输     出: YAFFS_OK - 执行成功
**         : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_unlink_file_if_needed(struct yaffs_obj *in)
{
	int ret_val;
	int del_now = 0;
	struct yaffs_dev *dev = in->my_dev;

	if (!in->my_inode)
		del_now = 1;

	if (del_now) {
		ret_val =
		    yaffs_change_obj_name(in, in->my_dev->del_dir,
					  _Y("deleted"), 0, 0);
		yaffs_trace(YAFFS_TRACE_TRACING,
			"yaffs: immediate deletion of file %d",
			in->obj_id);
		in->deleted = 1;
		in->my_dev->n_deleted_files++;
		if (dev->param.disable_soft_del || dev->param.is_yaffs2)
			yaffs_resize_file(in, 0);
		yaffs_soft_del_file(in);
	} else {
		ret_val =
		    yaffs_change_obj_name(in, in->my_dev->unlinked_dir,
					  _Y("unlinked"), 0, 0);
	}
	return ret_val;
}

/*********************************************************************************************************
** 函数名称: yaffs_del_file
** 功能描述: 删除指定的文件，如果这个文件已经是一个“软”删除文件或者当前是 yaffs2，则直接从 FLASH 中
**         : 删除与其相关所有数据。如果文件还有有效数据 chunk 且有人打开这个文件，则执行软删除操作
** 输	 入: in - 需要删除的文件对象
** 输     出: YAFFS_OK - 执行成功
**         : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_del_file(struct yaffs_obj *in)
{
	int ret_val = YAFFS_OK;
	int deleted;	/* Need to cache value on stack if in is freed */
	struct yaffs_dev *dev = in->my_dev;

	if (dev->param.disable_soft_del || dev->param.is_yaffs2)
		yaffs_resize_file(in, 0);

	if (in->n_data_chunks > 0) {
		/* Use soft deletion if there is data in the file.
		 * That won't be the case if it has been resized to zero.
		 */
		if (!in->unlinked)
			ret_val = yaffs_unlink_file_if_needed(in);

		deleted = in->deleted;

		if (ret_val == YAFFS_OK && in->unlinked && !in->deleted) {
			in->deleted = 1;
			deleted = 1;
			in->my_dev->n_deleted_files++;
			yaffs_soft_del_file(in);
		}
		return deleted ? YAFFS_OK : YAFFS_FAIL;
	} else {
		/* The file has no data chunks so we toss it immediately */
		yaffs_free_tnode(in->my_dev, in->variant.file_variant.top);
		in->variant.file_variant.top = NULL;
		yaffs_generic_obj_del(in);

		return YAFFS_OK;
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_is_non_empty_dir
** 功能描述: 判断指定目录是否为空
** 输	 入: obj - 需要判断的目录对象
** 输     出: 1 - 目录为空
**         : 0 - 目录非空
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_is_non_empty_dir(struct yaffs_obj *obj)
{
	return (obj &&
		obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY) &&
		!(list_empty(&obj->variant.dir_variant.children));
}

/*********************************************************************************************************
** 函数名称: yaffs_del_dir
** 功能描述: 删除指定目录，如果目录不为空，则删除失败，如果为空，则直接删除相关所有资源（FLASH、内存）
** 输	 入: obj - 需要删除的目录对象
** 输     出: YAFFS_OK - 默认执行成功
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_del_dir(struct yaffs_obj *obj)
{
	/* First check that the directory is empty. */
	if (yaffs_is_non_empty_dir(obj))
		return YAFFS_FAIL;

	return yaffs_generic_obj_del(obj);
}

/*********************************************************************************************************
** 函数名称: yaffs_del_symlink
** 功能描述: 删除符号链接相关所有资源（FLASH、内存）
** 输	 入: in - 需要删除的符号链接
** 输     出: YAFFS_OK - 默认执行成功
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_del_symlink(struct yaffs_obj *in)
{
	kfree(in->variant.symlink_variant.alias);
	in->variant.symlink_variant.alias = NULL;

	return yaffs_generic_obj_del(in);
}

/*********************************************************************************************************
** 函数名称: yaffs_del_link
** 功能描述: 删除符号链接相关所有资源（FLASH、内存）
** 输	 入: in - 需要删除的硬链接
** 输     出: YAFFS_OK - 默认执行成功
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_del_link(struct yaffs_obj *in)
{
	/* remove this hardlink from the list associated with the equivalent
	 * object
	 */
	list_del_init(&in->hard_links);
	return yaffs_generic_obj_del(in);
}

/*********************************************************************************************************
** 函数名称: yaffs_del_obj
** 功能描述: 删除和指定对象相关的所有资源（FLASH、内存），这个对象可以是目录、文件等等
** 输	 入: obj - 需要删除的对象指针
** 输     出: YAFFS_OK - 默认执行成功
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_del_obj(struct yaffs_obj *obj)
{
	int ret_val = -1;

	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		ret_val = yaffs_del_file(obj);
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		if (!list_empty(&obj->variant.dir_variant.dirty)) {
			yaffs_trace(YAFFS_TRACE_BACKGROUND,
				"Remove object %d from dirty directories",
				obj->obj_id);
			list_del_init(&obj->variant.dir_variant.dirty);
		}
		return yaffs_del_dir(obj);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		ret_val = yaffs_del_symlink(obj);
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		ret_val = yaffs_del_link(obj);
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		ret_val = yaffs_generic_obj_del(obj);
		break;
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		ret_val = 0;
		break;		/* should not happen. */
	}
	return ret_val;
}

/*********************************************************************************************************
** 函数名称: yaffs_empty_dir_to_dir
** 功能描述: 从 from_dir 目录中所有对象复制到 to_dir 目录中，并清空 from_dir 目录
** 输	 入: from_dir - 源目录
**         : to_dir - 目的目录
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_empty_dir_to_dir(struct yaffs_obj *from_dir,
				   struct yaffs_obj *to_dir)
{
	struct yaffs_obj *obj;
	struct list_head *lh;
	struct list_head *n;

	list_for_each_safe(lh, n, &from_dir->variant.dir_variant.children) {
		obj = list_entry(lh, struct yaffs_obj, siblings);
		yaffs_add_obj_to_dir(to_dir, obj);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_retype_obj
** 功能描述: 重新设置指定对象的类型，如果原类型是文件或者目录，先清空它的数据
** 输	 入: obj - 需要重新设置类型的对象
**		   : type - 目的目录
** 输	 出: obj - 修改类型后的对象指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_retype_obj(struct yaffs_obj *obj,
				   enum yaffs_obj_type type)
{
	/* Tear down the old variant */
	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		/* Nuke file data */
		yaffs_resize_file(obj, 0);
		yaffs_free_tnode(obj->my_dev, obj->variant.file_variant.top);
		obj->variant.file_variant.top = NULL;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		/* Put the children in lost and found. */
		yaffs_empty_dir_to_dir(obj, obj->my_dev->lost_n_found);
		if (!list_empty(&obj->variant.dir_variant.dirty))
			list_del_init(&obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		/* Nuke symplink data */
		kfree(obj->variant.symlink_variant.alias);
		obj->variant.symlink_variant.alias = NULL;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		list_del_init(&obj->hard_links);
		break;
	default:
		break;
	}

	memset(&obj->variant, 0, sizeof(obj->variant));

	/*Set up new variant if the memset is not enough. */
	switch (type) {
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		INIT_LIST_HEAD(&obj->variant.dir_variant.children);
		INIT_LIST_HEAD(&obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_FILE:
	case YAFFS_OBJECT_TYPE_SYMLINK:
	case YAFFS_OBJECT_TYPE_HARDLINK:
	default:
		break;
	}

	obj->variant_type = type;

	return obj;

}

/*********************************************************************************************************
** 函数名称: yaffs_unlink_worker
** 功能描述: 删除一个指定对象，这个对象可以是目录、文件等等（如果文件处于使用状态，不会立即执行删除操作）
** 输	 入: obj - 需要删除的对象
** 输	 出: YAFFS_OK - 删除成功
**         : YAFFS_FAIL - 删除失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_unlink_worker(struct yaffs_obj *obj)
{
	int del_now = 0;

	if (!obj)
		return YAFFS_FAIL;

	if (!obj->my_inode)
		del_now = 1;

	yaffs_update_parent(obj->parent);

	if (obj->variant_type == YAFFS_OBJECT_TYPE_HARDLINK) {
		return yaffs_del_link(obj);
	} else if (!list_empty(&obj->hard_links)) {
		/* Curve ball: We're unlinking an object that has a hardlink.
		 *
		 * This problem arises because we are not strictly following
		 * The Linux link/inode model.
		 *
		 * We can't really delete the object.
		 * Instead, we do the following:
		 * - Select a hardlink.
		 * - Unhook it from the hard links
		 * - Move it from its parent directory so that the rename works.
		 * - Rename the object to the hardlink's name.
		 * - Delete the hardlink
		 */

		/* 如果我们删除的对象还有指向它的硬链接 */
		struct yaffs_obj *hl;
		struct yaffs_obj *parent;
		int ret_val;
		YCHAR name[YAFFS_MAX_NAME_LENGTH + 1];

		hl = list_entry(obj->hard_links.next, struct yaffs_obj,
				hard_links);

		yaffs_get_obj_name(hl, name, YAFFS_MAX_NAME_LENGTH + 1);
		parent = hl->parent;

		list_del_init(&hl->hard_links);

		yaffs_add_obj_to_dir(obj->my_dev->unlinked_dir, hl);

		ret_val = yaffs_change_obj_name(obj, parent, name, 0, 0);

		if (ret_val == YAFFS_OK)
			ret_val = yaffs_generic_obj_del(hl);

		return ret_val;

	} else if (del_now) {
		switch (obj->variant_type) {
		case YAFFS_OBJECT_TYPE_FILE:
			return yaffs_del_file(obj);
			break;
		case YAFFS_OBJECT_TYPE_DIRECTORY:
			list_del_init(&obj->variant.dir_variant.dirty);
			return yaffs_del_dir(obj);
			break;
		case YAFFS_OBJECT_TYPE_SYMLINK:
			return yaffs_del_symlink(obj);
			break;
		case YAFFS_OBJECT_TYPE_SPECIAL:
			return yaffs_generic_obj_del(obj);
			break;
		case YAFFS_OBJECT_TYPE_HARDLINK:
		case YAFFS_OBJECT_TYPE_UNKNOWN:
		default:
			return YAFFS_FAIL;
		}
	} else if (yaffs_is_non_empty_dir(obj)) {
		return YAFFS_FAIL;
	} else {
		return yaffs_change_obj_name(obj, obj->my_dev->unlinked_dir,
						_Y("unlinked"), 0, 0);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_unlink_obj
** 功能描述: 如果一个对象允许对它执行 unlink操作，则删除它，这个对象可以是目录、文件等等
** 输	 入: obj - 需要删除的对象
** 输	 出: YAFFS_OK - 删除成功
**         : YAFFS_FAIL - 删除失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_unlink_obj(struct yaffs_obj *obj)
{
	if (obj && obj->unlink_allowed)
		return yaffs_unlink_worker(obj);

	return YAFFS_FAIL;
}

/*********************************************************************************************************
** 函数名称: yaffs_unlinker
** 功能描述: 删除指定目录中的指定对象
** 输	 入: dir - 需要删除对象所在目录
**         : name - 需要删除的对象名字
** 输	 出: YAFFS_OK - 删除成功
**         : YAFFS_FAIL - 删除失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_unlinker(struct yaffs_obj *dir, const YCHAR *name)
{
	struct yaffs_obj *obj;

	obj = yaffs_find_by_name(dir, name);
	return yaffs_unlink_obj(obj);
}

/* Note:
 * If old_name is NULL then we take old_dir as the object to be renamed.
 */

/*********************************************************************************************************
** 函数名称: yaffs_rename_obj
** 功能描述: 把指定目录指定对象修改到新指定的目录中的新指定名字
** 输	 入: old_dir - 需要重新命名对象原来所在目录
**         : old_name - 需要重新命名对象原来名字
**         : new_dir - 新命名对象新的目录
**         : new_name - 新命名对象的新的名字
** 输	 出: YAFFS_OK - 执行成功
**         : YAFFS_FAIL - 执行失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_rename_obj(struct yaffs_obj *old_dir, const YCHAR *old_name,
		     struct yaffs_obj *new_dir, const YCHAR *new_name)
{
	struct yaffs_obj *obj = NULL;
	struct yaffs_obj *existing_target = NULL;
	int force = 0;
	int result;
	struct yaffs_dev *dev;

	if (!old_dir || old_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		BUG();
		return YAFFS_FAIL;
	}
	if (!new_dir || new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		BUG();
		return YAFFS_FAIL;
	}

	dev = old_dir->my_dev;

#ifdef CONFIG_YAFFS_CASE_INSENSITIVE
	/* Special case for case insemsitive systems.
	 * While look-up is case insensitive, the name isn't.
	 * Therefore we might want to change x.txt to X.txt
	 */
	if (old_dir == new_dir &&
		old_name && new_name &&
		strcmp(old_name, new_name) == 0)
		force = 1;
#endif

	if (strnlen(new_name, YAFFS_MAX_NAME_LENGTH + 1) >
	    YAFFS_MAX_NAME_LENGTH)
		/* ENAMETOOLONG */
		return YAFFS_FAIL;

	if (old_name)
		obj = yaffs_find_by_name(old_dir, old_name);
	else{
		obj = old_dir;
		old_dir = obj->parent;
	}

	if (obj && obj->rename_allowed) {
		/* Now handle an existing target, if there is one */
		existing_target = yaffs_find_by_name(new_dir, new_name);
		if (yaffs_is_non_empty_dir(existing_target)) {
			return YAFFS_FAIL;	/* ENOTEMPTY */
		} else if (existing_target && existing_target != obj) {
			/* Nuke the target first, using shadowing,
			 * but only if it isn't the same object.
			 *
			 * Note we must disable gc here otherwise it can mess
			 * up the shadowing.
			 *
			 */
			dev->gc_disable = 1;
			yaffs_change_obj_name(obj, new_dir, new_name, force,
					      existing_target->obj_id);
			existing_target->is_shadowed = 1;
			yaffs_unlink_obj(existing_target);
			dev->gc_disable = 0;
		}

		result = yaffs_change_obj_name(obj, new_dir, new_name, 1, 0);

		yaffs_update_parent(old_dir);
		if (new_dir != old_dir)
			yaffs_update_parent(new_dir);

		return result;
	}
	return YAFFS_FAIL;
}

/*----------------------- Initialisation Scanning ---------------------- */

/*********************************************************************************************************
** 函数名称: yaffs_handle_shadowed_obj
** 功能描述: 
** 输     入: dev - yaffs 设备
** 		   : obj_id - shadow 对象 ID
** 		   : backward_scanning - 是否是从后往前扫描模式
** 输     出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_handle_shadowed_obj(struct yaffs_dev *dev, int obj_id,
			       int backward_scanning)
{
	struct yaffs_obj *obj;

	if (backward_scanning) {
		/* Handle YAFFS2 case (backward scanning)
		 * If the shadowed object exists then ignore.
		 */
		obj = yaffs_find_by_number(dev, obj_id);
		if (obj)
			return;
	}

	/* Let's create it (if it does not exist) assuming it is a file so that
	 * it can do shrinking etc.
	 * We put it in unlinked dir to be cleaned up after the scanning
	 */
	obj =
	    yaffs_find_or_create_by_number(dev, obj_id, YAFFS_OBJECT_TYPE_FILE);
	if (!obj)
		return;
	obj->is_shadowed = 1;
	yaffs_add_obj_to_dir(dev->unlinked_dir, obj);
	obj->variant.file_variant.shrink_size = 0;
	obj->valid = 1;		/* So that we don't read any other info. */
}

/*********************************************************************************************************
** 函数名称: yaffs_link_fixup
** 功能描述: 把指定的硬链接链表成员分别添加到它指向目标的 hard_links 链表中，即把硬链接对象添加到
**		   : 目标对象的硬链接链表中
** 输	 入: dev - yaffs 设备
**		   : hard_list - 需要处理的硬链接链表，这些硬链接“不”需要是一个对象的
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_link_fixup(struct yaffs_dev *dev, struct list_head *hard_list)
{
	struct list_head *lh;
	struct list_head *save;
	struct yaffs_obj *hl;
	struct yaffs_obj *in;

	list_for_each_safe(lh, save, hard_list) {
		hl = list_entry(lh, struct yaffs_obj, hard_links);
		in = yaffs_find_by_number(dev,
					hl->variant.hardlink_variant.equiv_id);

		if (in) {
			/* Add the hardlink pointers */
			hl->variant.hardlink_variant.equiv_obj = in;
			list_add(&hl->hard_links, &in->hard_links);
		} else {
			/* Todo Need to report/handle this better.
			 * Got a problem... hardlink to a non-existant object
			 */
			hl->variant.hardlink_variant.equiv_obj = NULL;
			INIT_LIST_HEAD(&hl->hard_links);
		}
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_strip_deleted_objs
** 功能描述: 删除指定设备 unlinked 目录和 deleted 目录中所有对象占用的所有资源（FLASH、内存）
** 输	 入: dev - yaffs 设备
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_strip_deleted_objs(struct yaffs_dev *dev)
{
	/*
	 *  Sort out state of unlinked and deleted objects after scanning.
	 */
	struct list_head *i;
	struct list_head *n;
	struct yaffs_obj *l;

	if (dev->read_only)
		return;

	/* Soft delete all the unlinked files */
	list_for_each_safe(i, n,
			   &dev->unlinked_dir->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);
		yaffs_del_obj(l);
	}

	list_for_each_safe(i, n, &dev->del_dir->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);
		yaffs_del_obj(l);
	}
}

/*
 * This code iterates through all the objects making sure that they are rooted.
 * Any unrooted objects are re-rooted in lost+found.
 * An object needs to be in one of:
 * - Directly under deleted, unlinked
 * - Directly or indirectly under root.
 *
 * Note:
 *  This code assumes that we don't ever change the current relationships
 *  between directories:
 *   root_dir->parent == unlinked_dir->parent == del_dir->parent == NULL
 *   lost-n-found->parent == root_dir
 *
 * This fixes the problem where directories might have inadvertently been
 * deleted leaving the object "hanging" without being rooted in the
 * directory tree.
 */

/*********************************************************************************************************
** 函数名称: yaffs_has_null_parent
** 功能描述: 判断指定对象的父目录是否为空（只有 deleted 目录、unlinked 目录和 root 目录满足 ）
** 输	 入: dev - yaffs 设备
**         : obj - 需要判断的对象指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_has_null_parent(struct yaffs_dev *dev, struct yaffs_obj *obj)
{
	return (obj == dev->del_dir ||
		obj == dev->unlinked_dir || obj == dev->root_dir);
}

/*********************************************************************************************************
** 函数名称: yaffs_fix_hanging_objs
** 功能描述: 处理那些被 hanging 的对象，把他们都放到 dev->lost_n_found 目录中。所谓的 hanging 指的
**         : 是这些对象没有挂载点
** 输	 入: dev - yaffs 设备
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_fix_hanging_objs(struct yaffs_dev *dev)
{
	struct yaffs_obj *obj;
	struct yaffs_obj *parent;
	int i;
	struct list_head *lh;
	struct list_head *n;
	int depth_limit;
	int hanging;

	if (dev->read_only)
		return;

	/* Iterate through the objects in each hash entry,
	 * looking at each object.
	 * Make sure it is rooted.
	 */

	for (i = 0; i < YAFFS_NOBJECT_BUCKETS; i++) {
		list_for_each_safe(lh, n, &dev->obj_bucket[i].list) {
			obj = list_entry(lh, struct yaffs_obj, hash_link);
			parent = obj->parent;

			if (yaffs_has_null_parent(dev, obj)) {
				/* These directories are not hanging */
				hanging = 0;
			} else if (!parent ||
				   parent->variant_type !=
				   YAFFS_OBJECT_TYPE_DIRECTORY) {
				hanging = 1;
			} else if (yaffs_has_null_parent(dev, parent)) {
				hanging = 0;
			} else {
				/*
				 * Need to follow the parent chain to
				 * see if it is hanging.
				 */
				hanging = 0;
				depth_limit = 100;

				while (parent != dev->root_dir &&
				       parent->parent &&
				       parent->parent->variant_type ==
				       YAFFS_OBJECT_TYPE_DIRECTORY &&
				       depth_limit > 0) {
					parent = parent->parent;
					depth_limit--;
				}
				if (parent != dev->root_dir)
					hanging = 1;
			}
			if (hanging) {
				yaffs_trace(YAFFS_TRACE_SCAN,
					"Hanging object %d moved to lost and found",
					obj->obj_id);
				yaffs_add_obj_to_dir(dev->lost_n_found, obj);
			}
		}
	}
}

/*
 * Delete directory contents for cleaning up lost and found.
 */

/*********************************************************************************************************
** 函数名称: yaffs_del_dir_contents
** 功能描述: 递归删除指定目录的所有内容，有了递归操作，我们就可以删除那些存在目录嵌套的对象了
** 输	 入: dir - 需要删除的目录
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_del_dir_contents(struct yaffs_obj *dir)
{
	struct yaffs_obj *obj;
	struct list_head *lh;
	struct list_head *n;

	if (dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY)
		BUG();

	list_for_each_safe(lh, n, &dir->variant.dir_variant.children) {
		obj = list_entry(lh, struct yaffs_obj, siblings);
		if (obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY)
			yaffs_del_dir_contents(obj);
		yaffs_trace(YAFFS_TRACE_SCAN,
			"Deleting lost_found object %d",
			obj->obj_id);
		yaffs_unlink_obj(obj);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_empty_l_n_f
** 功能描述: 递归删除指定设备 lost_n_found 目录的所有内容
** 输	 入: dev - yaffs 设备
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_empty_l_n_f(struct yaffs_dev *dev)
{
	yaffs_del_dir_contents(dev->lost_n_found);
}

/*********************************************************************************************************
** 函数名称: yaffs_find_by_name
** 功能描述: 在指定目录下查找指定目标对象
** 输     入: directory - 需要查找对象所在目录
**         : name - 需要查找的目标名字
** 输     出: l - 查找到的目标对象指针
**         : NULL - 没找到目标对象
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_find_by_name(struct yaffs_obj *directory,
				     const YCHAR *name)
{
	int sum;
	struct list_head *i;
	YCHAR buffer[YAFFS_MAX_NAME_LENGTH + 1];
	struct yaffs_obj *l;

	if (!name)
		return NULL;

	if (!directory) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_find_by_name: null pointer directory"
			);
		BUG();
		return NULL;
	}
	if (directory->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_find_by_name: non-directory"
			);
		BUG();
	}

	sum = yaffs_calc_name_sum(name);

	list_for_each(i, &directory->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);

		if (l->parent != directory)
			BUG();

		yaffs_check_obj_details_loaded(l);

		/* Special case for lost-n-found */
		if (l->obj_id == YAFFS_OBJECTID_LOSTNFOUND) {
			if (!strcmp(name, YAFFS_LOSTNFOUND_NAME))
				return l;
		} else if (l->sum == sum || l->hdr_chunk <= 0) {
			/* LostnFound chunk called Objxxx
			 * Do a real check
			 */
			yaffs_get_obj_name(l, buffer,
				YAFFS_MAX_NAME_LENGTH + 1);
			if (!strncmp(name, buffer, YAFFS_MAX_NAME_LENGTH))
				return l;
		}
	}
	return NULL;
}

/* GetEquivalentObject dereferences any hard links to get to the
 * actual object.
 */

/*********************************************************************************************************
** 函数名称: yaffs_get_equivalent_obj
** 功能描述: 如果输入对象是硬链接，则转换成硬链接所指向的对象，否则什么都不做
** 输	 入: obj - 需要查询的硬链接对象
** 输	 出: obj - 找到的目标对象指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
struct yaffs_obj *yaffs_get_equivalent_obj(struct yaffs_obj *obj)
{
	if (obj && obj->variant_type == YAFFS_OBJECT_TYPE_HARDLINK) {
		obj = obj->variant.hardlink_variant.equiv_obj;
		yaffs_check_obj_details_loaded(obj);
	}
	return obj;
}

/*
 *  A note or two on object names.
 *  * If the object name is missing, we then make one up in the form objnnn
 *
 *  * ASCII names are stored in the object header's name field from byte zero
 *  * Unicode names are historically stored starting from byte zero.
 *
 * Then there are automatic Unicode names...
 * The purpose of these is to save names in a way that can be read as
 * ASCII or Unicode names as appropriate, thus allowing a Unicode and ASCII
 * system to share files.
 *
 * These automatic unicode are stored slightly differently...
 *  - If the name can fit in the ASCII character space then they are saved as
 *    ascii names as per above.
 *  - If the name needs Unicode then the name is saved in Unicode
 *    starting at oh->name[1].
 */

/*********************************************************************************************************
** 函数名称: yaffs_fix_null_name
** 功能描述: 如果输入的 name 为空，那么就生成一个默认的名字存储到 name 缓冲区中，默认名字格式如下：
**         : "obj" + "obj_id"
** 输	 入: obj - 需要处理的对象
**         : name - 存储名字的缓冲区
**         : buffer_size - 名字缓冲区大小
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void yaffs_fix_null_name(struct yaffs_obj *obj, YCHAR *name,
				int buffer_size)
{
	/* Create an object name if we could not find one. */
	if (strnlen(name, YAFFS_MAX_NAME_LENGTH) == 0) {
		YCHAR local_name[20];
		YCHAR num_string[20];
		YCHAR *x = &num_string[19];
		unsigned v = obj->obj_id;
		num_string[19] = 0;
		while (v > 0) {
			x--;
			*x = '0' + (v % 10);
			v /= 10;
		}
		/* make up a name */
		strcpy(local_name, YAFFS_LOSTNFOUND_PREFIX);
		strcat(local_name, x);
		strncpy(name, local_name, buffer_size - 1);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_get_obj_name
** 功能描述: 获取指定对象的名字
** 输	 入: obj - 需要获取名字的对象
**		   : buffer_size - 名字缓冲区大小
** 输	 出: name - 存储名字的缓冲区
**		   : 名字字符串长度
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_get_obj_name(struct yaffs_obj *obj, YCHAR *name, int buffer_size)
{
	memset(name, 0, buffer_size * sizeof(YCHAR));
	yaffs_check_obj_details_loaded(obj);
	if (obj->obj_id == YAFFS_OBJECTID_LOSTNFOUND) {
		strncpy(name, YAFFS_LOSTNFOUND_NAME, buffer_size - 1);
	} else if (obj->short_name[0]) {
		strcpy(name, obj->short_name);
	} else if (obj->hdr_chunk > 0) {
		int result;
		u8 *buffer = yaffs_get_temp_buffer(obj->my_dev);

		struct yaffs_obj_hdr *oh = (struct yaffs_obj_hdr *)buffer;

		memset(buffer, 0, obj->my_dev->data_bytes_per_chunk);

		if (obj->hdr_chunk > 0) {
			result = yaffs_rd_chunk_tags_nand(obj->my_dev,
							  obj->hdr_chunk,
							  buffer, NULL);
		}
		if (result == YAFFS_OK)
			yaffs_load_name_from_oh(obj->my_dev, name, oh->name,
					buffer_size);

		yaffs_release_temp_buffer(obj->my_dev, buffer);
	}

	yaffs_fix_null_name(obj, name, buffer_size);

	return strnlen(name, YAFFS_MAX_NAME_LENGTH);
}

/*********************************************************************************************************
** 函数名称: yaffs_get_obj_length
** 功能描述: 获取指定对象的长度
**         : 1. 如果对象是文件，则返回文件长度
**         : 2. 如果对象是软链接，则返回软链接字符串长度
**         : 3. 其他返回一个 nand_chunk 字节数
** 输	 入: obj - 需要获取长度的对象
** 输	 出: 目标对象的长度
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
loff_t yaffs_get_obj_length(struct yaffs_obj *obj)
{
	/* Dereference any hard linking */
	obj = yaffs_get_equivalent_obj(obj);

	if (obj->variant_type == YAFFS_OBJECT_TYPE_FILE)
		return obj->variant.file_variant.file_size;
	if (obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK) {
		if (!obj->variant.symlink_variant.alias)
			return 0;
		return strnlen(obj->variant.symlink_variant.alias,
				     YAFFS_MAX_ALIAS_LENGTH);
	} else {
		/* Only a directory should drop through to here */
		return obj->my_dev->data_bytes_per_chunk;
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_get_obj_length
** 功能描述: 获取指定对象硬链接个数
** 输	 入: obj - 查询的对象
** 输	 出: 目标对象硬链接个数
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_get_obj_link_count(struct yaffs_obj *obj)
{
	int count = 0;
	struct list_head *i;

	if (!obj->unlinked)
		count++;	/* the object itself */

	list_for_each(i, &obj->hard_links)
	    count++;		/* add the hard links; */

	return count;
}

/*********************************************************************************************************
** 函数名称: yaffs_get_obj_length
** 功能描述: 获取指定对象的对象 ID
** 输	 入: obj - 查询的对象
** 输	 出: 目标对象的对象 ID
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_get_obj_inode(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);

	return obj->obj_id;
}

/*********************************************************************************************************
** 函数名称: yaffs_get_obj_length
** 功能描述: 获取指定对象的类型
** 输	 入: obj - 查询的对象
** 输	 出: 目标对象的类型
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
unsigned yaffs_get_obj_type(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);

	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		return DT_REG;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		return DT_DIR;
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		return DT_LNK;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		return DT_REG;
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		if (S_ISFIFO(obj->yst_mode))
			return DT_FIFO;
		if (S_ISCHR(obj->yst_mode))
			return DT_CHR;
		if (S_ISBLK(obj->yst_mode))
			return DT_BLK;
		if (S_ISSOCK(obj->yst_mode))
			return DT_SOCK;
		return DT_REG;
		break;
	default:
		return DT_REG;
		break;
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_get_symlink_alias
** 功能描述: 获取指定符号链接对象的符号链接字符串并返回，返回的字符串是存储在动态分配的内存中
** 输	 入: obj - 需要获取的符号链接对象
** 输	 出: 符号链接字符串
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
YCHAR *yaffs_get_symlink_alias(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);
	if (obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK)
		return yaffs_clone_str(obj->variant.symlink_variant.alias);
	else
		return yaffs_clone_str(_Y(""));
}

/*--------------------------- Initialisation code -------------------------- */

/*********************************************************************************************************
** 函数名称: yaffs_check_dev_fns
** 功能描述: 检查并初始化指定设备必要的函数指针
** 输	 入: dev - yaffs 设备
** 输	 出: 1 - 成功
**         : 0 - 失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_check_dev_fns(struct yaffs_dev *dev)
{
	struct yaffs_driver *drv = &dev->drv;
	struct yaffs_tags_handler *tagger = &dev->tagger;

	/* Common functions, gotta have */
	if (!drv->drv_read_chunk_fn ||
	    !drv->drv_write_chunk_fn ||
	    !drv->drv_erase_fn)
		return 0;

	if (dev->param.is_yaffs2 &&
	     (!drv->drv_mark_bad_fn  || !drv->drv_check_bad_fn))
		return 0;

	/* Install the default tags marshalling functions if needed. */
	yaffs_tags_compat_install(dev);   /* yaffs1 使用这组函数 */
	yaffs_tags_marshall_install(dev); /* yaffs2 使用这组函数 */

	/* Check we now have the marshalling functions required. */
	if (!tagger->write_chunk_tags_fn ||
	    !tagger->read_chunk_tags_fn ||
	    !tagger->query_block_fn ||
	    !tagger->mark_bad_fn)
		return 0;

	return 1;
}

/*********************************************************************************************************
** 函数名称: yaffs_create_initial_dir
** 功能描述: 创建指定设备的 fake 目录，分别是 unlinked 目录、deleted 目录、root 目录和 lost_n_found 目录
**         : 并把 lost_n_found 目录添加到 root 目录中，另外两个目录不显示（终端看不见）
** 输	 入: dev - yaffs 设备
** 输	 出: YAFFS_OK - 创建成功
**         : YAFFS_FAIL - 创建失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static int yaffs_create_initial_dir(struct yaffs_dev *dev)
{
	/* Initialise the unlinked, deleted, root and lost+found directories */
	dev->lost_n_found = NULL;
	dev->root_dir = NULL;
	dev->unlinked_dir = NULL;
	dev->del_dir = NULL;

	dev->unlinked_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_UNLINKED, S_IFDIR);
	dev->del_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_DELETED, S_IFDIR);
	dev->root_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_ROOT,
				  YAFFS_ROOT_MODE | S_IFDIR);
	dev->lost_n_found =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_LOSTNFOUND,
				  YAFFS_LOSTNFOUND_MODE | S_IFDIR);

	if (dev->lost_n_found &&
		dev->root_dir &&
		dev->unlinked_dir &&
		dev->del_dir) {
			/* If lost-n-found is hidden then yank it out of the directory tree. */
			if (dev->param.hide_lost_n_found)
				list_del_init(&dev->lost_n_found->siblings);
			else
				yaffs_add_obj_to_dir(dev->root_dir, dev->lost_n_found);
		return YAFFS_OK;
	}
	return YAFFS_FAIL;
}

/* Low level init.
 * Typically only used by yaffs_guts_initialise, but also used by the
 * Low level yaffs driver tests.
 */

/*********************************************************************************************************
** 函数名称: yaffs_guts_ll_init
** 功能描述: low level 初始化代码
** 输	 入: dev - yaffs 设备
** 输	 出: YAFFS_OK - 创建成功
**         : YAFFS_FAIL - 创建失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_guts_ll_init(struct yaffs_dev *dev)
{


	yaffs_trace(YAFFS_TRACE_TRACING, "yaffs: yaffs_ll_init()");

	if (!dev) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"yaffs: Need a device"
			);
		return YAFFS_FAIL;
	}

	if (dev->ll_init)
		return YAFFS_OK;

	dev->internal_start_block = dev->param.start_block;
	dev->internal_end_block = dev->param.end_block;
	dev->block_offset = 0;
	dev->chunk_offset = 0;
	dev->n_free_chunks = 0;

	dev->gc_block = 0;

	if (dev->param.start_block == 0) {
		dev->internal_start_block = dev->param.start_block + 1;
		dev->internal_end_block = dev->param.end_block + 1;
		dev->block_offset = 1;
		dev->chunk_offset = dev->param.chunks_per_block;
	}

	/* Check geometry parameters. */

	if ((!dev->param.inband_tags && dev->param.is_yaffs2 &&
		dev->param.total_bytes_per_chunk < 1024) ||
		(!dev->param.is_yaffs2 &&
			dev->param.total_bytes_per_chunk < 512) ||
		(dev->param.inband_tags && !dev->param.is_yaffs2) ||   /* 只有 yaffs2 支持 inband_tags */
		 dev->param.chunks_per_block < 2 ||
		 dev->param.n_reserved_blocks < 2 ||   
		dev->internal_start_block <= 0 ||
		dev->internal_end_block <= 0 ||
		dev->internal_end_block <=
		(dev->internal_start_block + dev->param.n_reserved_blocks + 2)
		) {
		/* otherwise it is too small */
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"NAND geometry problems: chunk size %d, type is yaffs%s, inband_tags %d ",
			dev->param.total_bytes_per_chunk,
			dev->param.is_yaffs2 ? "2" : "",
			dev->param.inband_tags);
		return YAFFS_FAIL;
	}

	/* Sort out space for inband tags, if required */
	if (dev->param.inband_tags)
		dev->data_bytes_per_chunk =
		    dev->param.total_bytes_per_chunk -
		    sizeof(struct yaffs_packed_tags2_tags_only);
	else
		dev->data_bytes_per_chunk = dev->param.total_bytes_per_chunk;

	/* Got the right mix of functions? */
	if (!yaffs_check_dev_fns(dev)) {
		/* Function missing */
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"device function(s) missing or wrong");

		return YAFFS_FAIL;
	}

	if (yaffs_init_nand(dev) != YAFFS_OK) {
		yaffs_trace(YAFFS_TRACE_ALWAYS, "InitialiseNAND failed");
		return YAFFS_FAIL;
	}

	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_guts_format_dev
** 功能描述: 把指定设备所有不是坏块的 FLASH 块擦除
** 输	 入: dev - yaffs 设备
** 输	 出: YAFFS_OK - 创建成功
**         : YAFFS_FAIL - 创建失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_guts_format_dev(struct yaffs_dev *dev)
{
	u32 i;
	enum yaffs_block_state state;
	u32 dummy;

	if(yaffs_guts_ll_init(dev) != YAFFS_OK)
		return YAFFS_FAIL;

	if(dev->is_mounted)
		return YAFFS_FAIL;

	for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		yaffs_query_init_block_state(dev, i, &state, &dummy);
		if (state != YAFFS_BLOCK_STATE_DEAD)
			yaffs_erase_block(dev, i);
	}

	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_guts_initialise
** 功能描述: yaffs 核心初始化
** 输	 入: dev - yaffs 设备
** 输	 出: YAFFS_OK - 创建成功
**         : YAFFS_FAIL - 创建失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_guts_initialise(struct yaffs_dev *dev)
{
	int init_failed = 0;
	u32 x;
	u32 bits;

	if(yaffs_guts_ll_init(dev) != YAFFS_OK)
		return YAFFS_FAIL;

	if (dev->is_mounted) {
		yaffs_trace(YAFFS_TRACE_ALWAYS, "device already mounted");
		return YAFFS_FAIL;
	}

	dev->is_mounted = 1;

	/* OK now calculate a few things for the device */

	/*
	 *  Calculate all the chunk size manipulation numbers:
	 */
	x = dev->data_bytes_per_chunk;
	/* We always use dev->chunk_shift and dev->chunk_div */
	dev->chunk_shift = calc_shifts(x);
	x >>= dev->chunk_shift;
	dev->chunk_div = x;
	/* We only use chunk mask if chunk_div is 1 */
	dev->chunk_mask = (1 << dev->chunk_shift) - 1;

	/*
	 * Calculate chunk_grp_bits.
	 * We need to find the next power of 2 > than internal_end_block
	 */

	x = dev->param.chunks_per_block * (dev->internal_end_block + 1);

	bits = calc_shifts_ceiling(x);

	/* Set up tnode width if wide tnodes are enabled. */
	if (!dev->param.wide_tnodes_disabled) {
		/* bits must be even so that we end up with 32-bit words */
		if (bits & 1)
			bits++;
		if (bits < 16)
			dev->tnode_width = 16;
		else
			dev->tnode_width = bits;
	} else {
		dev->tnode_width = 16;
	}

	dev->tnode_mask = (1 << dev->tnode_width) - 1;

	/* Level0 Tnodes are 16 bits or wider (if wide tnodes are enabled),
	 * so if the bitwidth of the
	 * chunk range we're using is greater than 16 we need
	 * to figure out chunk shift and chunk_grp_size
	 */

	if (bits <= dev->tnode_width)
		dev->chunk_grp_bits = 0;
	else
		dev->chunk_grp_bits = bits - dev->tnode_width;

	dev->tnode_size = (dev->tnode_width * YAFFS_NTNODES_LEVEL0) / 8;
	if (dev->tnode_size < sizeof(struct yaffs_tnode))
		dev->tnode_size = sizeof(struct yaffs_tnode);

	dev->chunk_grp_size = 1 << dev->chunk_grp_bits;

	if (dev->param.chunks_per_block < dev->chunk_grp_size) {
		/* We have a problem because the soft delete won't work if
		 * the chunk group size > chunks per block.
		 * This can be remedied by using larger "virtual blocks".
		 */
		yaffs_trace(YAFFS_TRACE_ALWAYS, "chunk group too large");

		return YAFFS_FAIL;
	}

	/* Finished verifying the device, continue with initialisation */

	/* More device initialisation */
	dev->all_gcs = 0;
	dev->passive_gc_count = 0;
	dev->oldest_dirty_gc_count = 0;
	dev->bg_gcs = 0;
	dev->gc_block_finder = 0;
	dev->buffered_block = -1;
	dev->doing_buffered_block_rewrite = 0;
	dev->n_deleted_files = 0;
	dev->n_bg_deletions = 0;
	dev->n_unlinked_files = 0;
	dev->n_ecc_fixed = 0;
	dev->n_ecc_unfixed = 0;
	dev->n_tags_ecc_fixed = 0;
	dev->n_tags_ecc_unfixed = 0;
	dev->n_erase_failures = 0;
	dev->n_erased_blocks = 0;
	dev->gc_disable = 0;
	dev->has_pending_prioritised_gc = 1; /* Assume the worst for now,
					      * will get fixed on first GC */
	INIT_LIST_HEAD(&dev->dirty_dirs);
	dev->oldest_dirty_seq = 0;
	dev->oldest_dirty_block = 0;

	yaffs_endian_config(dev);

	/* Initialise temporary buffers and caches. */
	if (!yaffs_init_tmp_buffers(dev))
		init_failed = 1;

	dev->cache = NULL;
	dev->gc_cleanup_list = NULL;

	if (!init_failed && dev->param.n_caches > 0) {
		u32 i;
		void *buf;
		u32 cache_bytes =
		    dev->param.n_caches * sizeof(struct yaffs_cache);

		if (dev->param.n_caches > YAFFS_MAX_SHORT_OP_CACHES)
			dev->param.n_caches = YAFFS_MAX_SHORT_OP_CACHES;

		dev->cache = kmalloc(cache_bytes, GFP_NOFS);

		buf = (u8 *) dev->cache;

		if (dev->cache)
			memset(dev->cache, 0, cache_bytes);

		for (i = 0; i < dev->param.n_caches && buf; i++) {
			dev->cache[i].object = NULL;
			dev->cache[i].last_use = 0;
			dev->cache[i].dirty = 0;
			dev->cache[i].data = buf =
			    kmalloc(dev->param.total_bytes_per_chunk, GFP_NOFS);
		}
		if (!buf)
			init_failed = 1;

		dev->cache_last_use = 0;
	}

	dev->cache_hits = 0;

	if (!init_failed) {
		dev->gc_cleanup_list =
		    kmalloc(dev->param.chunks_per_block * sizeof(u32),
					GFP_NOFS);
		if (!dev->gc_cleanup_list)
			init_failed = 1;
	}

	if (dev->param.is_yaffs2)
		dev->param.use_header_file_size = 1;

	if (!init_failed && !yaffs_init_blocks(dev))
		init_failed = 1;

	yaffs_init_tnodes_and_objs(dev);

	if (!init_failed && !yaffs_create_initial_dir(dev))
		init_failed = 1;

	if (!init_failed && dev->param.is_yaffs2 &&
		!dev->param.disable_summary &&
		!yaffs_summary_init(dev))
		init_failed = 1;

	if (!init_failed) {
		/* Now scan the flash. */
		if (dev->param.is_yaffs2) {
			if (yaffs2_checkpt_restore(dev)) {
				yaffs_check_obj_details_loaded(dev->root_dir);
				yaffs_trace(YAFFS_TRACE_CHECKPOINT |
					YAFFS_TRACE_MOUNT,
					"yaffs: restored from checkpoint"
					);
			} else {

				/* Clean up the mess caused by an aborted
				 * checkpoint load then scan backwards.
				 */
				yaffs_deinit_blocks(dev);

				yaffs_deinit_tnodes_and_objs(dev);

				dev->n_erased_blocks = 0;
				dev->n_free_chunks = 0;
				dev->alloc_block = -1;
				dev->alloc_page = -1;
				dev->n_deleted_files = 0;
				dev->n_unlinked_files = 0;
				dev->n_bg_deletions = 0;

				if (!init_failed && !yaffs_init_blocks(dev))
					init_failed = 1;

				yaffs_init_tnodes_and_objs(dev);

				if (!init_failed
				    && !yaffs_create_initial_dir(dev))
					init_failed = 1;

				if (!init_failed && !yaffs2_scan_backwards(dev))
					init_failed = 1;
			}
		} else if (!yaffs1_scan(dev)) {
			init_failed = 1;
		}

		yaffs_strip_deleted_objs(dev);
		yaffs_fix_hanging_objs(dev);
		if (dev->param.empty_lost_n_found)
			yaffs_empty_l_n_f(dev);
	}

	if (init_failed) {
		/* Clean up the mess */
		yaffs_trace(YAFFS_TRACE_TRACING,
		  "yaffs: yaffs_guts_initialise() aborted.");

		yaffs_deinitialise(dev);
		return YAFFS_FAIL;
	}

	/* Zero out stats */
	dev->n_page_reads = 0;
	dev->n_page_writes = 0;
	dev->n_erasures = 0;
	dev->n_gc_copies = 0;
	dev->n_retried_writes = 0;

	dev->n_retired_blocks = 0;

	yaffs_verify_free_chunks(dev);
	yaffs_verify_blocks(dev);

	/* Clean up any aborted checkpoint data */
	if (!dev->is_checkpointed && dev->blocks_in_checkpt > 0)
		yaffs2_checkpt_invalidate(dev);

	yaffs_trace(YAFFS_TRACE_TRACING,
	  "yaffs: yaffs_guts_initialise() done.");
	return YAFFS_OK;
}

/*********************************************************************************************************
** 函数名称: yaffs_guts_initialise
** 功能描述: yaffs 核心反初始化
** 输	 入: dev - yaffs 设备
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_deinitialise(struct yaffs_dev *dev)
{
	if (dev->is_mounted) {
		u32 i;

		yaffs_deinit_blocks(dev);
		yaffs_deinit_tnodes_and_objs(dev);
		yaffs_summary_deinit(dev);

		if (dev->param.n_caches > 0 && dev->cache) {

			for (i = 0; i < dev->param.n_caches; i++) {
				kfree(dev->cache[i].data);
				dev->cache[i].data = NULL;
			}

			kfree(dev->cache);
			dev->cache = NULL;
		}

		kfree(dev->gc_cleanup_list);

		for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
			kfree(dev->temp_buffer[i].buffer);
			dev->temp_buffer[i].buffer = NULL;
		}

		kfree(dev->checkpt_buffer);
		dev->checkpt_buffer = NULL;
		kfree(dev->checkpt_block_list);
		dev->checkpt_block_list = NULL;

		dev->is_mounted = 0;

		yaffs_deinit_nand(dev);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_count_free_chunks
** 功能描述: 统计指定设备可用空闲 nand_chunk 个数
** 输	 入: dev - yaffs 设备
** 输	 出: n_free - 空闲 nand_chunk 个数
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_count_free_chunks(struct yaffs_dev *dev)
{
	int n_free = 0;
	u32 b;
	struct yaffs_block_info *blk;

	blk = dev->block_info;
	for (b = dev->internal_start_block; b <= dev->internal_end_block; b++) {
		switch (blk->block_state) {
		case YAFFS_BLOCK_STATE_EMPTY:
		case YAFFS_BLOCK_STATE_ALLOCATING:
		case YAFFS_BLOCK_STATE_COLLECTING:
		case YAFFS_BLOCK_STATE_FULL:
			n_free +=
			    (dev->param.chunks_per_block - blk->pages_in_use +
			     blk->soft_del_pages);
			break;
		default:
			break;
		}
		blk++;
	}
	return n_free;
}

/*********************************************************************************************************
** 函数名称: yaffs_get_n_free_chunks
** 功能描述: 
** 输	 入: dev - yaffs 设备
** 输	 出: n_free - 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_get_n_free_chunks(struct yaffs_dev *dev)
{
	/* This is what we report to the outside world */
	int n_free;
	int n_dirty_caches;
	int blocks_for_checkpt;
	u32 i;

	n_free = dev->n_free_chunks;
	n_free += dev->n_deleted_files;

	/* Now count and subtract the number of dirty chunks in the cache. */

	for (n_dirty_caches = 0, i = 0; i < dev->param.n_caches; i++) {
		if (dev->cache[i].dirty)
			n_dirty_caches++;
	}

	n_free -= n_dirty_caches;

	n_free -=
	    ((dev->param.n_reserved_blocks + 1) * dev->param.chunks_per_block);

	/* Now figure checkpoint space and report that... */
	blocks_for_checkpt = yaffs_calc_checkpt_blocks_required(dev);

	n_free -= (blocks_for_checkpt * dev->param.chunks_per_block);

	if (n_free < 0)
		n_free = 0;

	return n_free;
}


/*
 * Marshalling functions to get loff_t file sizes into and out of
 * object headers.
 */

/*********************************************************************************************************
** 函数名称: yaffs_oh_size_load
** 功能描述: 设置指定对象头中“文件大小”字段的值
** 输 	 入: dev - yaffs 设备
**         : oh - 需要设置的对象头
**         : fsize - 新的文件大小
**         : do_endian - 知否需要做大小端处理
** 输 	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_oh_size_load(struct yaffs_dev *dev,
			struct yaffs_obj_hdr *oh,
			loff_t fsize,
			int do_endian)
{
	oh->file_size_low = FSIZE_LOW(fsize);

	oh->file_size_high = FSIZE_HIGH(fsize);

	if (do_endian) {
		yaffs_do_endian_u32(dev, &oh->file_size_low);
		yaffs_do_endian_u32(dev, &oh->file_size_high);
	}
}

/*********************************************************************************************************
** 函数名称: yaffs_oh_to_size
** 功能描述: 从指定对象头中获取文件长度信息
** 输 	 入: dev - yaffs 设备
**         : oh - 需要设置的对象头
**         : do_endian - 知否需要做大小端处理
** 输	 出: retval - 头中长度值
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
loff_t yaffs_oh_to_size(struct yaffs_dev *dev, struct yaffs_obj_hdr *oh,
			int do_endian)
{
	loff_t retval;


	if (sizeof(loff_t) >= 8 && ~(oh->file_size_high)) {
		u32 low = oh->file_size_low;
		u32 high = oh->file_size_high;

		if (do_endian) {
			yaffs_do_endian_u32 (dev, &low);
			yaffs_do_endian_u32 (dev, &high);
		}
		retval = FSIZE_COMBINE(high, low);
	} else {
		u32 low = oh->file_size_low;

		if (do_endian)
			yaffs_do_endian_u32(dev, &low);
		retval = (loff_t)low;
	}

	return retval;
}

/*********************************************************************************************************
** 函数名称: yaffs_count_blocks_by_state
** 功能描述: 扫描指定设备所有空间，统计每个状态中的块数（yaffs_block_state 每个状态值对应一个数组元素）
**		   : 1. 数组下标 0 统计了 YAFFS_BLOCK_STATE_UNKNOWN 状态以及超出状态可表示范围的块数
** 输	 入: dev - yaffs 设备
**		   : bs - 每个状态包含的块数
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void yaffs_count_blocks_by_state(struct yaffs_dev *dev, int bs[10])
{
	u32 i;
	struct yaffs_block_info *bi;
	int s;

	for(i = 0; i < 10; i++)
		bs[i] = 0;

	for(i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		bi = yaffs_get_block_info(dev, i);
		s = bi->block_state;
		if(s > YAFFS_BLOCK_STATE_DEAD || s < YAFFS_BLOCK_STATE_UNKNOWN)
			bs[0]++;
		else
			bs[s]++;
	}
}
