obj-m = chardev.o
ccflags-y := -std=gnu99 -Wno-vla

all:
	make -C /lib/modules/`uname -r`/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/`uname -r`/build M=$(PWD) clean