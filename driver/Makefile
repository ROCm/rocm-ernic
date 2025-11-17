# Out-of-tree kernel module Makefile for amd_emrdma
# Usage: make -f Makefile.out-of-tree

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Module name and object files (must match in-tree Makefile)
obj-m := amd_emrdma.o
amd_emrdma-y := amd_emrdma_cmd.o amd_emrdma_cq.o amd_emrdma_doorbell.o \
                amd_emrdma_main.o amd_emrdma_misc.o amd_emrdma_mr.o \
                amd_emrdma_qp.o amd_emrdma_srq.o amd_emrdma_verbs.o

# Current directory (where the driver source is)
PWD := $(shell pwd)

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install the module (requires root)
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Load the module (requires root)
load:
	modprobe amd_emrdma

# Unload the module (requires root)
unload:
	modprobe -r amd_emrdma || true

# Reload the module (requires root)
reload: unload load

# Show module info
info:
	modinfo amd_emrdma.ko

# Show build configuration
config:
	@echo "Kernel build directory: $(KDIR)"
	@echo "Module directory: $(PWD)"
	@echo "Current kernel: $(shell uname -r)"

.PHONY: all clean install load unload reload info config

