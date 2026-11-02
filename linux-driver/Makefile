# Kbuild for the Apple Afterburner Phase 0 probe module.
obj-m += afterburner_probe.o afterburner_map.o afterburner_name.o afterburner_full.o afterburner_flash.o afterburner_flash2.o afterburner_irq.o afterburner_dma.o afterburner_submit.o afterburner_frame.o afterburner_bar1.o afterburner_bar1_test.o afterburner_dma_test.o afterburner_init_seq.o afterburner_submit2.o ab_reset.o ab_poll.o ab_busreset.o ab_find.o ab_d3.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo cp afterburner_probe.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
