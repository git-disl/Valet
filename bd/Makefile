SHELL = /bin/sh
INSTALL = /usr/bin/install -c
mkdir_p = mkdir -p
build_dir = /lib/modules/$(shell uname -r)
OFED_CFLAGS = -I/var/lib/dkms/mlnx-ofed-kernel/3.1/build/include -include /var/lib/dkms/mlnx-ofed-kernel/3.1/build/include/linux/compat-2.6.h
KSYMVERS = /var/lib/dkms/mlnx-ofed-kernel/3.1/build/Module.symvers
NOSTDINC_FLAGS += -I/var/lib/dkms/mlnx-ofed-kernel/3.1/build/include -include /var/lib/dkms/mlnx-ofed-kernel/3.1/build/include/linux/compat-2.6.h
ccflags-y  += -w

valetmoduledir = /lib/modules/$(shell uname -r)/extra/valet
valetmodule := valet.ko
src_dir := ./
nbdxadm := nbdxadm

all: all-spec

install: all
	$(mkdir_p) $(DESTDIR)$(valetmoduledir)
	$(INSTALL) -m 644 $(src_dir)/$(valetmodule) $(DESTDIR)$(valetmoduledir)/$(valetmodule)
	-/sbin/depmod -a
	-\cp $(nbdxadm)/nbdxadm /usr/local/bin/

uninstall:
	rm -f $(DESTDIR)$(valetmoduledir)/$(valetmodule)
	-/sbin/depmod -a
	-rm -f /usr/local/bin/nbdxadm

clean:
	$(MAKE) -C $(build_dir)/build SUBDIRS=`pwd` clean

obj-m := valet.o

valet-objs := 	\
	alf_queue.o	\
	rdmabox.o	\
	diskbox.o	\
	valet_mempool.o	\
	gpt.o	\
	valet_configfs.o	\
	valet_drv.o

all-spec:
	$(MAKE) -C $(build_dir)/build SUBDIRS=`pwd` KBUILD_EXTRA_SYMBOLS="$(KSYMVERS)" modules
