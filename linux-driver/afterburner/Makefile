# Kbuild — Apple Afterburner production driver (Phase 3u, plan.md §8)
obj-m += afterburner.o
afterburner-y := ab_pci.o ab_regs.o ab_engine.o ab_dma.o ab_chardev.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
