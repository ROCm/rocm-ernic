# Out-of-tree kernel module Makefile for rocm_ernic
# Usage: make -f Makefile.out-of-tree

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Module name and object files
obj-m := rocm_ernic.o
rocm_ernic-y := rocm_ernic_cmd.o rocm_ernic_cq.o rocm_ernic_doorbell.o \
                rocm_ernic_main.o rocm_ernic_misc.o rocm_ernic_mr.o \
                rocm_ernic_qp.o rocm_ernic_srq.o rocm_ernic_verbs.o

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
	modprobe rocm_ernic

# Unload the module (requires root)
unload:
	modprobe -r rocm_ernic || true

# Reload the module (requires root)
reload: unload load

# Show module info
info:
	modinfo rocm_ernic.ko

# Show build configuration
config:
	@echo "Kernel build directory: $(KDIR)"
	@echo "Module directory: $(PWD)"
	@echo "Current kernel: $(shell uname -r)"

.PHONY: all clean install load unload reload info config

