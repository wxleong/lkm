obj-m = lkm.o
lkm-objs := chardev.o
lkm-objs += operator.o

ccflags-y := -std=gnu99 -Wno-vla

all:
	make -C /lib/modules/`uname -r`/build/ M=$(PWD) modules
	gcc test.c -o test
clean:
	make -C /lib/modules/`uname -r`/build M=$(PWD) clean
	rm test