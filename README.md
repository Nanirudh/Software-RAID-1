# Software-RAID-1
Kernel module implementing RAID-1 mirroring and correction using crc

/*
rmmod ssr 2>/dev/null

dd if=/dev/zero of=/dev/vdb bs=1M count=10
dd if=/dev/zero of=/dev/vdc bs=1M count=10
sync

insmod ssr.ko
chmod 666 /dev/ssr

echo "HELLO_SSR" | dd of=/dev/ssr bs=512 count=1
dd if=/dev/ssr bs=512 count=1 | hexdump -C

*/
