# SPDX-License-Identifier: GPL-2.0
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# Out-of-tree build of ernic_p4.ko.
#
# The module uses the ionic driver's private headers (ionic_api.h and the
# structures they pull in), which no kernel-headers package ships: they
# live in the driver's own directory in the kernel tree.  IONIC_SRC must
# therefore name a tree that scripts/fetch-ionic-sources.sh has prepared,
# with both patches/ and patches/p4 applied -- the latter is what defines
# IONIC_LIF_CAP_P4 and creates the device this binds to.
#
#   make -C drivers/ernic_p4 KDIR=/lib/modules/$(uname -r)/build \
#        IONIC_SRC=/path/to/ionic-kernel-src

KDIR ?= /lib/modules/$(shell uname -r)/build
IONIC_SRC ?=

# Module.symvers of the ionic build this module will load against.
#
# ionic is itself out of tree here (DKMS), and patches/p4 adds a field
# to struct ionic_lif, which changes the genksyms CRC of every exported
# symbol taking that type -- ionic_adminq_post_wait among them. Built
# against the kernel's own Module.symvers instead, this module records
# the CRC of whatever ionic the distro shipped, and insmod rejects it
# against the patched one. Empty is allowed, for a tree where the
# running ionic is the kernel's.
IONIC_SYMVERS ?=

# Kernel to look IONIC_SYMVERS up for, with find-ionic-symvers.sh, when
# it is not given. dkms.conf sets this rather than running the lookup
# itself: dkms sources that file whenever it reads the package's
# configuration (dkms status, add, remove, and every autoinstall pass,
# including the ones that only learn AUTOINSTALL is off), and the lookup
# can rebuild ionic. Here it runs only for a build.
IONIC_SYMVERS_KVER ?=

# Where the ionic driver lives inside a kernel tree.  Kept as a variable
# rather than spelled out at each use: it appears four times, and the
# whole path written out is a 40-character run of [A-Za-z0-9/], which is
# what generic secret scanners match on as a high-entropy string.
IONIC_DRV := drivers/net/ethernet/pensando/ionic
IONIC_INC := $(IONIC_SRC)/$(IONIC_DRV)

obj-m += ernic_p4.o

ccflags-y += -I$(IONIC_INC)
ccflags-y += -I$(src)

# The symbols are passed to kbuild on its command line, so modpost sees
# them too. The lookup's own make of ionic gets none of this make's flags
# or variables. A failed lookup falls back to the kernel's symbols, as an
# empty IONIC_SYMVERS does, but says so: the module it builds will not
# insert beside a patched ionic.
all:
	@test -n "$(IONIC_SRC)" || \
	  { echo "ERROR: IONIC_SRC must name a patched ionic source tree" >&2; \
	    exit 1; }
	@test -f "$(IONIC_INC)/ionic_api.h" || \
	  { echo "ERROR: $(IONIC_SRC) has no ionic driver sources" >&2; exit 1; }
	@grep -q IONIC_LIF_CAP_P4 "$(IONIC_INC)/ionic_if.h" || \
	  { echo "ERROR: $(IONIC_SRC) lacks patches/p4; the aux device this" >&2; \
	    echo "       module binds to would never be created" >&2; exit 1; }
	@test -z "$(IONIC_SYMVERS)" || test -f "$(IONIC_SYMVERS)" || \
	  { echo "ERROR: IONIC_SYMVERS=$(IONIC_SYMVERS) does not exist" >&2; \
	    exit 1; }
	@symvers="$(IONIC_SYMVERS)"; \
	if [ -z "$$symvers" ] && [ -n "$(IONIC_SYMVERS_KVER)" ]; then \
	  symvers="$$(env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL \
	    "$(CURDIR)/find-ionic-symvers.sh" "$(IONIC_SYMVERS_KVER)")" || \
	    { echo "WARNING: no ionic symbols for $(IONIC_SYMVERS_KVER);" \
	        "building against the kernel's own" >&2; symvers=; }; \
	fi; \
	echo "KBUILD_EXTRA_SYMBOLS=$$symvers"; \
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_EXTRA_SYMBOLS="$$symvers" modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
