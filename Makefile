BPF_CC ?= clang
BPFTOOL ?= /usr/sbin/bpftool

# -g is required (by Clang) to generate BTF
# for bpf-gcc, use -mcpu=v3 -gbtf -mco-re -O2
BPF_CFLAGS += --target=bpf -mcpu=v3 -g -O2
MODE ?= debug

ifeq ($(MODE), debug)
CFLAGS += -O0 -g
else ifeq ($(MODE), release)
CFLAGS += -O2
endif

BPF_CFLAGS += -iquote. -Wall -std=gnu99
CFLAGS += -iquote. -Wall -std=gnu99

ifneq ($(BPF_USE_SYSTEM_VMLINUX),)
BPF_CFLAGS += -D_MIMIC_BPF_USE_SYSTEM_VMLINUX
use_system_vmlinux_req := bpf/vmlinux/system.h

ifeq ($(KERNEL_UNAME),)
KERNEL_VMLINUX := /sys/kernel/btf/vmlinux
else ifeq ($(KERNEL_UNAME),$(shell uname -r))
KERNEL_VMLINUX := /sys/kernel/btf/vmlinux
else ifneq ($(wildcard /usr/lib/debug/lib/modules/$(KERNEL_UNAME)/vmlinux),)
KERNEL_VMLINUX := /usr/lib/debug/lib/modules/$(KERNEL_UNAME)/vmlinux
else ifneq ($(wildcard /lib/modules/$(KERNEL_UNAME)/build/vmlinux),)
KERNEL_VMLINUX := /lib/modules/$(KERNEL_UNAME)/build/vmlinux
else
$(error vmlinux file not found)
endif

else
BPF_CFLAGS += -D_MIMIC_BPF_TARGET_ARCH_$(shell uname -m)
endif

mimic_common_headers := $(wildcard common/*.h)

mimic_bpf_src := $(wildcard bpf/*.c)
mimic_bpf_obj := $(mimic_bpf_src:.c=.o)
mimic_bpf_headers := bpf/vmlinux.h $(wildcard bpf/*.h) $(mimic_common_headers)

mimic_src := $(wildcard src/*.c)
mimic_obj := $(mimic_src:.c=.o)
mimic_headers := src/bpf_skel.h $(wildcard src/*.h) $(mimic_common_headers)
mimic_link_libs := -lbpf -lffi

ifeq ($(filter "gnu libc" "glibc" "free software foundation",$(shell ldd --version 2>&1 | tr '[A-Z]' '[a-z]')),)
mimic_link_libs += -largp
endif

ifneq ($(STATIC),)
mimic_link_libs += -lelf -lzstd -lz
LDFLAGS += -static
endif

mimic_tools := $(patsubst tools/%.c,%,$(wildcard tools/*.c))

RUNTIME_DIR ?=
ifneq ($(RUNTIME_DIR),)
CFLAGS += -DMIMIC_RUNTIME_DIR="\"$(RUNTIME_DIR)\""
endif

mkdir_p = mkdir -p $(@D)
check_options := out/.options.$(shell echo $(BPF_CC) $(CC) $(BPFTOOL) $(BPF_CFLAGS) $(CFLAGS) | sha256sum | awk '{ print $$1 }')

.PHONY: .FORCE
.FORCE:

all: build

.PHONY: build build-cli build-kmod build-tools
build: build-cli build-kmod build-tools
build-cli: out/mimic
build-kmod: out/mimic.ko
build-tools: $(patsubst %,out/%,$(mimic_tools))

.PHONY: generate generate-skel generate-manpage generate-pot generate-compile-commands
generate: generate-skel generate-vmlinux
generate-skel: src/bpf_skel.h
generate-manpage: out/mimic.1.gz
generate-pot: out/mimic.pot
generate-compile-commands: compile_commands.json

.PHONY: test
test: build-cli
	bats tests

.PHONY: bench
bench: build-cli
	tests/bench.bash

.PHONY: clean
clean:
	$(MAKE) -C kmod $@
	rm -rf out/
	find . -type f -name *.o -delete
	rm -f src/bpf_skel.h
	rm -f bpf/vmlinux/system.h

out/.options.%:
	$(mkdir_p)
	rm -f out/.options.*
	touch $@

bpf/vmlinux/system.h:
	$(BPFTOOL) btf dump file $(KERNEL_VMLINUX) format c > $@

$(filter bpf/%.o, $(mimic_bpf_obj)): bpf/%.o: bpf/%.c $(mimic_bpf_headers) $(use_system_vmlinux_req) $(check_options)
	$(BPF_CC) $(BPF_CFLAGS) -D_MIMIC_BPF -c -o $@ $<

out/mimic.bpf.o: $(mimic_bpf_obj)
	$(mkdir_p)
	$(BPFTOOL) gen object $@ $(mimic_bpf_obj)

src/bpf_skel.h: out/mimic.bpf.o
	$(BPFTOOL) gen skeleton out/mimic.bpf.o > $@

$(filter src/%.o, $(mimic_obj)): src/%.o: $(mimic_headers) $(check_options)

out/mimic: $(mimic_obj)
	$(mkdir_p)
	$(CC) $(CFLAGS) $(mimic_obj) -o $@ $(LDFLAGS) $(mimic_link_libs)

out/mimic.ko: .FORCE build-tools
	$(mkdir_p)
	$(MAKE) -C kmod
	cp kmod/mimic.ko $@

define generate_tool_rule
out/$(1): tools/$(1).c $$(mimic_common_headers)
	$$(mkdir_p)
	$$(CC) $$(CFLAGS) $$< -o $$@ $$(LDFLAGS)
endef
$(foreach _tool,$(mimic_tools),$(eval $(call generate_tool_rule,$(_tool))))

out/mimic.1.gz: docs/mimic.1.md
	$(mkdir_p)
	ronn -r --pipe $< | gzip -c > $@

out/mimic.pot:
	$(mkdir_p)
	find src -type f -regex '.*\.[ch]' | xargs xgettext -k_ -kN_ -o $@ --

compile_commands.json: clean
	bear -- $(MAKE)
