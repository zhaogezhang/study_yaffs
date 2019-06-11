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

#include "yaffs_bitmap.h"
#include "yaffs_trace.h"
/*
 * Chunk bitmap manipulations
 */

static inline u8 *yaffs_block_bits(struct yaffs_dev *dev, int blk)
{
	if (blk < (int)dev->internal_start_block ||
	    blk > (int)dev->internal_end_block) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"BlockBits block %d is not valid",
			blk);
		BUG();
	}
	return dev->chunk_bits +
	    (dev->chunk_bit_stride * (blk - dev->internal_start_block));
}

void yaffs_verify_chunk_bit_id(struct yaffs_dev *dev, int blk, int chunk)
{
	if (blk < (int)dev->internal_start_block ||
	    blk > (int)dev->internal_end_block ||
	    chunk < 0 || chunk >= (int)dev->param.chunks_per_block) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"Chunk Id (%d:%d) invalid",
			blk, chunk);
		BUG();
	}
}

void yaffs_clear_chunk_bits(struct yaffs_dev *dev, int blk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);

	memset(blk_bits, 0, dev->chunk_bit_stride);
}

void yaffs_clear_chunk_bit(struct yaffs_dev *dev, int blk, int chunk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);

	yaffs_verify_chunk_bit_id(dev, blk, chunk);
	blk_bits[chunk / 8] &= ~(1 << (chunk & 7));
}

void yaffs_set_chunk_bit(struct yaffs_dev *dev, int blk, int chunk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);

	yaffs_verify_chunk_bit_id(dev, blk, chunk);
	blk_bits[chunk / 8] |= (1 << (chunk & 7));
}

int yaffs_check_chunk_bit(struct yaffs_dev *dev, int blk, int chunk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);

	yaffs_verify_chunk_bit_id(dev, blk, chunk);
	return (blk_bits[chunk / 8] & (1 << (chunk & 7))) ? 1 : 0;
}

/*********************************************************************************************************
** 函数名称: yaffs_still_some_chunks
** 功能描述: 通过 chunk_bits 判断指定的块中是否还有数据存在
** 输	 入: dev - yaffs 设备
**		   : blk - 判断的 nand_block 号
** 输	 出: 1 - 还有数据
**		   : 0 - 没有数据
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
int yaffs_still_some_chunks(struct yaffs_dev *dev, int blk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);
	int i;

	for (i = 0; i < dev->chunk_bit_stride; i++) {
		if (*blk_bits)
			return 1;
		blk_bits++;
	}
	return 0;
}

int yaffs_count_chunk_bits(struct yaffs_dev *dev, int blk)
{
	u8 *blk_bits = yaffs_block_bits(dev, blk);
	int i;
	int n = 0;

	for (i = 0; i < dev->chunk_bit_stride; i++, blk_bits++)
		n += hweight8(*blk_bits);

	return n;
}
