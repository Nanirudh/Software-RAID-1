#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include "ssr_internal.h"

static struct ssr_dev ssr;

static const struct block_device_operations ssr_fops = {
	.owner      = THIS_MODULE,
	.submit_bio = ssr_submit_bio,
};


static int __init ssr_init(void)
{
	int ret;
	sector_t phys_sectors;

	ret = register_blkdev(SSR_MAJOR, "ssr");
	if (ret < 0)
		return ret;

	ssr.bdev1 = blkdev_get_by_path(PHYSICAL_DISK1_NAME,
		FMODE_READ | FMODE_WRITE, NULL);
	ssr.bdev2 = blkdev_get_by_path(PHYSICAL_DISK2_NAME,
		FMODE_READ | FMODE_WRITE, NULL);

	if (IS_ERR(ssr.bdev1) || IS_ERR(ssr.bdev2)) {
		ret = -ENODEV;
		goto err_blk;
	}

	/* Allocate queue (MANDATORY even with submit_bio) */
	ssr.queue = blk_alloc_queue(GFP_KERNEL);
	if (!ssr.queue) {
		ret = -ENOMEM;
		goto err_bdev;
	}

	blk_queue_logical_block_size(ssr.queue, KERNEL_SECTOR_SIZE);

	/* Allocate disk */
	ssr.gd = alloc_disk(SSR_NUM_MINORS);
	if (!ssr.gd) {
		ret = -ENOMEM;
		goto err_queue;
	}

	ssr.gd->major = SSR_MAJOR;
	ssr.gd->first_minor = SSR_FIRST_MINOR;
	ssr.gd->fops = &ssr_fops;
	ssr.gd->private_data = &ssr;
	ssr.gd->queue = ssr.queue;
	snprintf(ssr.gd->disk_name, 32, "ssr");

	/* Capacity: half data, half CRC */
	phys_sectors = get_capacity(ssr.bdev1->bd_disk);
	ssr.data_sectors = phys_sectors / 2;
	set_capacity(ssr.gd, ssr.data_sectors);

	/* Workqueue BEFORE add_disk */
	ssr.wq = alloc_workqueue("ssr_wq", WQ_UNBOUND, 0);
	if (!ssr.wq) {
		ret = -ENOMEM;
		goto err_disk;
	}

	add_disk(ssr.gd);

	pr_info("ssr: loaded (%llu sectors)\n",
		(unsigned long long)ssr.data_sectors);
	return 0;

err_disk:
	put_disk(ssr.gd);
err_queue:
	blk_cleanup_queue(ssr.queue);
err_bdev:
	blkdev_put(ssr.bdev1, FMODE_READ | FMODE_WRITE);
	blkdev_put(ssr.bdev2, FMODE_READ | FMODE_WRITE);
err_blk:
	unregister_blkdev(SSR_MAJOR, "ssr");
	return ret;
}



static void __exit ssr_exit(void)
{
	del_gendisk(ssr.gd);
	blk_cleanup_queue(ssr.gd->queue);
	put_disk(ssr.gd);
	destroy_workqueue(ssr.wq);
	blkdev_put(ssr.bdev1, FMODE_READ | FMODE_WRITE);
	blkdev_put(ssr.bdev2, FMODE_READ | FMODE_WRITE);
	unregister_blkdev(SSR_MAJOR, "ssr");
}


module_init(ssr_init);
module_exit(ssr_exit);
MODULE_LICENSE("GPL");

/*
rmmod ssr 2>/dev/null

dd if=/dev/zero of=/dev/vdb bs=1M count=10
dd if=/dev/zero of=/dev/vdc bs=1M count=10
sync

insmod ssr.ko
mknod /dev/ssr b 240 0
chmod 666 /dev/ssr

echo "HELLO_SSR" | dd of=/dev/ssr bs=512 count=1
dd if=/dev/ssr bs=512 count=1 | hexdump -C

*/