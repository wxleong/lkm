obj-m = mod.o
mod-objs += lkm.o
mod-objs += realtime.o
mod-objs += resource-manager.o
mod-objs += chardev.o

ccflags-y := -std=gnu99 -Wno-vla

all:
	make -C /lib/modules/`uname -r`/build/ M=$(PWD) modules
	gcc test.c -o test
clean:
	make -C /lib/modules/`uname -r`/build M=$(PWD) clean
	rm -f test