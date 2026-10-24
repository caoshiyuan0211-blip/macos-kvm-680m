# RembrandtGfx: Lilu plugin for the Radeon 680M. Same toolchain as src/hellokext (see its Makefile).
ROOT   := ../..
SDK    := ../MacKernelSDK
LILU   := ../Lilu-1.7.2/Lilu
LD     := $(ROOT)/build/cctools/bin/x86_64-apple-darwin19-ld
NAME   := RembrandtGfx
VER    := 0.1.0
KFLAGS := -target x86_64-apple-macos10.15 -mkernel -nostdinc -fno-builtin -fno-common -mno-red-zone -O2 \
          -fno-asynchronous-unwind-tables -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -I$(SDK)/Headers -I$(LILU)
CXXFLAGS := $(KFLAGS) -std=c++17 -fapple-kext -nostdlib -fno-rtti -fno-exceptions \
            -fno-use-cxa-atexit -mllvm -disable-atexit-based-global-dtor-lowering \
            -DPRODUCT_NAME=$(NAME) -DMODULE_VERSION=$(VER) -Wno-deprecated-declarations
OBJS   := kern_start.o mmio.o smu.o golden.o dmub.o display.o firmware.o probe.o vmwatch.o sleep.o umdpatch.o fw.o plugin_start.o info.o
KEXT   := $(NAME).kext/Contents

all: $(KEXT)/MacOS/$(NAME)

%.o: %.cpp rmb.hpp
	clang++ $(CXXFLAGS) -c $< -o $@
plugin_start.o: $(LILU)/Library/plugin_start.cpp
	clang++ $(CXXFLAGS) -c $< -o $@
# GC/SDMA/DMUB firmware pieces, split from linux-firmware's yellow_carp_*.bin (not shipped in this repo).
FWSRC  ?= /usr/lib/firmware/amdgpu
fw/toc.bin:
	rm -rf fw.tmp && mkdir -p fw.tmp
	for f in $(FWSRC)/yellow_carp_*.bin*; do b=$$(basename $$f); case $$f in \
	  *.zst) zstd -qdcf $$f > fw.tmp/$${b%.zst} ;; *.xz) xz -dc $$f > fw.tmp/$${b%.xz} ;; *) cp $$f fw.tmp/ ;; esac; done
	python3 $(ROOT)/scripts/fw-extract.py fw.tmp fw && rm -rf fw.tmp
fw.o: fw.s fw/toc.bin
	clang -target x86_64-apple-macos10.15 -c $< -o $@
info.o: info.c
	clang $(KFLAGS) -c $< -o $@

$(NAME): $(OBJS)
	$(LD) -arch x86_64 -kext -platform_version macos 10.15 10.15 -no_data_const -o $@ $^ $(SDK)/Library/x86_64/libkmod.a 2>&1 | grep -v 'directory not found' || true
	@test -f $@

$(KEXT)/MacOS/$(NAME): $(NAME) Info.plist
	mkdir -p $(KEXT)/MacOS && cp $(NAME) $@ && cp Info.plist $(KEXT)/Info.plist

# Every undefined symbol must come from the guest kernel or Lilu 1.7.2.
check: $(NAME)
	@m=$$(llvm-nm -u $< | sort -u | comm -23 - $(ROOT)/build/kpi/kernel.syms | comm -23 - $(ROOT)/build/kpi/lilu.syms); \
	 if [ -n "$$m" ]; then echo "unresolved: $$m"; exit 1; else echo "all symbols resolve"; fi

install: all check
	rm -rf $(ROOT)/efi/EFI/OC/Kexts/$(NAME).kext && cp -r $(NAME).kext $(ROOT)/efi/EFI/OC/Kexts/

clean:
	rm -rf *.o $(NAME) $(NAME).kext fw fw.tmp

.PHONY: all check install clean
