#ifndef SSR_INTERNAL_H
#define SSR_INTERNAL_H

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/genhd.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include "ssr.h"

struct ssr_dev {
	struct gendisk *gd;
	struct block_device *bdev1;
	struct block_device *bdev2;
	struct workqueue_struct *wq;
	struct request_queue *queue;
	sector_t data_sectors;
};

struct ssr_work {
	struct work_struct work;
	struct ssr_dev *dev;
	struct bio *bio;
};

blk_qc_t ssr_submit_bio(struct bio *bio);

#endif