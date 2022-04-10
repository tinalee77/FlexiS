obj-m := tcp_flexis.o
IDIR= /lib/modules/$(shell uname -r)/kernel/net/ipv4/
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install:
	install -v -m 644 tcp_flexis.ko $(IDIR)
	depmod
	modprobe tcp_flexis
	
uninstall:
	modprobe -r tcp_flexis
	
clean:
	rm -rf Module.markers modules.order Module.symvers tcp_flexis.ko tcp_flexis.mod.c tcp_flexis.mod.o tcp_flexis.o tcp_flexis.mod tcp_flexis.dwo tcp_flexis.mod.dwo
