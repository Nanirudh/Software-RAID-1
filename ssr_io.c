#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/blk_types.h>
#include "ssr_internal.h"

static sector_t ssr_crc_sector(struct ssr_dev *dev, sector_t logical_sector)
{
	return logical_sector + dev->data_sectors;
}

static bool ssr_crc_empty(u32 crc)
{
	return crc == 0;
}


static u32 ssr_crc_buf(const void *buf, size_t len)
{
	const u8 *p = buf;
	u32 crc = 0;
	size_t i;

	for (i = 0; i < len; i++)
		crc += p[i];

	return crc;
}

static u32 ssr_crc(const void *buf)
{
	return ssr_crc_buf(buf, KERNEL_SECTOR_SIZE);
}

static u32 ssr_crc_bio(struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	u32 crc = 0;

	bio_for_each_segment(bvec, bio, iter) {
		void *kaddr = kmap(bvec.bv_page);
		void *data = kaddr + bvec.bv_offset;

		crc ^= ssr_crc_buf(data, bvec.bv_len);

		kunmap(bvec.bv_page);
	}

	return crc;
}



static int ssr_rw_phys(struct block_device *bdev,
		       sector_t sector, void *buf, bool write)
{
	struct bio *bio;
	struct page *page;
	int ret;
	pr_info("phys %s dev=%s sector=%llu\n",
		write ? "WRITE" : "READ",
		bdev->bd_disk->disk_name,
		(unsigned long long)sector);

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio)
		return -ENOMEM;

	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = sector;
	bio_set_op_attrs(bio, write ? REQ_OP_WRITE : REQ_OP_READ, 0);

	page = alloc_page(GFP_NOIO);
	if (!page) {
		bio_put(bio);
		return -ENOMEM;
	}

	if (write)
		memcpy(page_address(page), buf, KERNEL_SECTOR_SIZE);

	ret = bio_add_page(bio, page, KERNEL_SECTOR_SIZE, 0);
	if (ret != KERNEL_SECTOR_SIZE) {
		pr_err("bio_add_page failed: ret=%d\n", ret);
		__free_page(page);
		bio_put(bio);
		return -EIO;
	}

	ret = submit_bio_wait(bio);
    if (ret)
		pr_err("submit_bio_wait failed: %d\n", ret);

	if (!write && ret == 0)
		memcpy(buf, page_address(page), KERNEL_SECTOR_SIZE);

	__free_page(page);
	bio_put(bio);
	return ret;
}


static void ssr_copy_from_bio(struct bio *bio, u8 *dst)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	size_t copied = 0;

	bio_for_each_segment(bvec, bio, iter) {
		void *src = kmap_atomic(bvec.bv_page);
		memcpy(dst + copied,
		       src + bvec.bv_offset,
		       bvec.bv_len);
		kunmap_atomic(src);
		copied += bvec.bv_len;
	}
}


static void ssr_copy_to_bio(struct bio *bio, const u8 *src)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	size_t copied = 0;

	bio_for_each_segment(bvec, bio, iter) {
		void *dst = kmap_atomic(bvec.bv_page);
		memcpy(dst + bvec.bv_offset,
		       src + copied,
		       bvec.bv_len);
		kunmap_atomic(dst);
		copied += bvec.bv_len;
	}
}

static void ssr_handle_read(struct ssr_dev *dev, struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;

	// u8 d1[512], d2[512];
	// u8 crcbuf[512];
	u8 *d1;
	u8 *d2;
	u8 *crcbuf;

	d1 = kmalloc(512, GFP_NOIO);
	d2 = kmalloc(512, GFP_NOIO);
	crcbuf = kmalloc(512, GFP_NOIO);
	if (!d1 || !d2 || !crcbuf) {
		//bio_io_error(bio);
		goto err;
	}



	u32 c1, c2;
	bool v1, v2;
	int ret;
	pr_info("read sector=%llu\n", (unsigned long long)sector);

	/* Read data */
	ret = ssr_rw_phys(dev->bdev1, sector, d1, false);
	if (ret) goto err;

	ret = ssr_rw_phys(dev->bdev2, sector, d2, false);
	if (ret) goto err;

	/* Read CRCs */
	ret = ssr_rw_phys(dev->bdev1, ssr_crc_sector(dev, sector), crcbuf, false);
	if (ret) goto err;
	c1 = *(u32 *)crcbuf;

	ret = ssr_rw_phys(dev->bdev2, ssr_crc_sector(dev, sector), crcbuf, false);
	if (ret) goto err;
	c2 = *(u32 *)crcbuf;

	pr_info("crc1=%u crc2=%u calc1=%u calc2=%u\n",
		c1, c2, ssr_crc(d1), ssr_crc(d2));

	u32 calc1 = ssr_crc(d1);
	u32 calc2 = ssr_crc(d2);

	v1 = ssr_crc_empty(calc1) || (calc1 == c1);
	v2 = ssr_crc_empty(calc2) || (calc2 == c2);

	if (v1 && v2) {
		/* both valid OR both empty */
		ssr_copy_to_bio(bio, d1);
		bio_endio(bio);
		return;
	}

	if (v1 && !v2) {
		ssr_copy_to_bio(bio, d1);
		*(u32 *)crcbuf = c1;
		ret = ssr_rw_phys(dev->bdev2, sector, d1, true);
		if (ret)
			goto err;
		ret = ssr_rw_phys(dev->bdev2, ssr_crc_sector(dev, sector), crcbuf, true);
		if (ret)
			goto err;
		bio_endio(bio);
		return;
	}

	if (!v1 && v2) {
		ssr_copy_to_bio(bio, d2);
		*(u32 *)crcbuf = c2;
		ret = ssr_rw_phys(dev->bdev1, sector, d2, true);
		if (ret)
			goto err;
		ret = ssr_rw_phys(dev->bdev1, ssr_crc_sector(dev, sector), crcbuf, true);
		if (ret)
			goto err;
		bio_endio(bio);
		return;
	}

err:
	pr_err("CRC invalid on both disks, sector=%llu\n",
	       (unsigned long long)sector);
    ssr_copy_to_bio(bio, d1);
	bio_io_error(bio);
}

static void ssr_handle_write(struct ssr_dev *dev, struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;
	u8 *data = NULL;
	u8 *cbuf = NULL;
	u32 crc;
	int ret;

	/* Reject anything other than 1 sector */
	if (bio->bi_iter.bi_size != KERNEL_SECTOR_SIZE) {
		pr_err("ssr: unsupported bio size %u\n",
		       bio->bi_iter.bi_size);
		bio_io_error(bio);
		return;
	}

	data = kmalloc(KERNEL_SECTOR_SIZE, GFP_NOIO);
	cbuf = kmalloc(KERNEL_SECTOR_SIZE, GFP_NOIO);
	if (!data || !cbuf) {
		bio_io_error(bio);
		goto out;
	}

	ssr_copy_from_bio(bio, data);

	crc = ssr_crc(data);
	memset(cbuf, 0, KERNEL_SECTOR_SIZE);
	*(u32 *)cbuf = crc;

	ret = ssr_rw_phys(dev->bdev1, sector, data, true);
	if (ret) goto err;

	ret = ssr_rw_phys(dev->bdev2, sector, data, true);
	if (ret) goto err;

	ret = ssr_rw_phys(dev->bdev1,
	                  ssr_crc_sector(dev, sector), cbuf, true);
	if (ret) goto err;

	ret = ssr_rw_phys(dev->bdev2,
	                  ssr_crc_sector(dev, sector), cbuf, true);
	if (ret) goto err;

	bio_endio(bio);
	goto out;

err:
	bio_io_error(bio);

out:
	kfree(data);
	kfree(cbuf);
}


static void ssr_workfn(struct work_struct *work)
{
	struct ssr_work *w =
		container_of(work, struct ssr_work, work);

	pr_info("workfn: bio=%p op=%s\n", w->bio,
		bio_data_dir(w->bio) == READ ? "READ" : "WRITE");

	if (bio_data_dir(w->bio) == READ)
		ssr_handle_read(w->dev, w->bio);
	else
		ssr_handle_write(w->dev, w->bio);

	kfree(w);
}

blk_qc_t ssr_submit_bio(struct bio *bio)
{
	struct ssr_dev *dev = bio->bi_disk->private_data;
	struct ssr_work *w;

	/* BUG #3 FIX: bounds check */
	if (bio->bi_iter.bi_sector >= dev->data_sectors) {
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	w = kmalloc(sizeof(*w), GFP_NOIO);
	if (!w) {
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	w->dev = dev;
	w->bio = bio;

	INIT_WORK(&w->work, ssr_workfn);
	queue_work(dev->wq, &w->work);

	return BLK_QC_T_NONE;
}
