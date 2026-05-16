KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m := snd-roland-mc707.o
snd-roland-mc707-y := src/main.o

ccflags-y += -DDEBUG -Wall

.PHONY: all modules clean load unload reload dmesg

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: modules
	sudo insmod ./snd-roland-mc707.ko

unload:
	sudo rmmod snd_roland_mc707 || true

reload: unload load

dmesg:
	dmesg --color=always | tail -40
