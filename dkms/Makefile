# Kbuild wrapper for the out-of-tree virtio_bt driver.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# DKMS invokes this via its default `make -C <kernelbuild> M=<dir> modules`,
# and it also works for a manual build:
#     make            # build virtio_bt.ko against the running kernel
#     make KVER=6.6.8 # ...against a specific installed kernel
#     make clean

obj-m := virtio_bt.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) modules clean

.PHONY: all clean
