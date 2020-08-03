sudo insmod ~/valet/bd/valet.ko
sudo mount -t configfs none /sys/kernel/config

sudo ~/valet/bd/nbdxadm/nbdxadm -o create_host -i $1 -p ~/valet/setup/portal.list
sudo ~/valet/bd/nbdxadm/nbdxadm -o create_device -i $1 -d $1

ls /dev/valet$1
sudo mkswap /dev/valet$1
sudo swapon /dev/valet$1

