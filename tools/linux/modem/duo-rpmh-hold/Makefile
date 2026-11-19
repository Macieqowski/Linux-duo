# SPDX-License-Identifier: GPL-2.0-or-later
# make KDIR=<kernel build dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
obj-m := duo-rpmh-hold.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
