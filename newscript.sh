make
./umount.wfs mnt
rmdir mnt
mkfs.wfs prebuilt_disk
mkdir mnt
mount.wfs -f -s prebuilt_disk mnt