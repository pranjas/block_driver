#obj-m includes the list of files which are going to be part of the module.
#Since we've only one file, the module's name will be same that is hello-world.ko.
#For modules spanning multiple files we can specify a phony target which gets the name
#of the module.

obj-m += pks-blk-driver.o
EXTRAFLAGS := -I ../include
all:
	make $(EXTRAFLAGS) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
