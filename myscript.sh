make
./umount.wfs mnt
rm -f disk
rmdir mnt
mkfs.wfs disk
mkdir mnt
mount.wfs -f -s disk mnt