# Freestanding UEFI boot-service driver (PE32+, subsystem 11). `make install` copies into the OC tree.
EFI = RmbGop.efi
CFLAGS = -target x86_64-unknown-windows -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
         -fno-builtin -O2 -Wall -Wextra
$(EFI): RmbGop.c
	clang $(CFLAGS) -c RmbGop.c -o RmbGop.o
	lld-link /nologo /subsystem:efi_boot_service_driver /entry:efi_main /nodefaultlib /dll /out:$(EFI) RmbGop.o
install: $(EFI)
	cp $(EFI) ../../efi/EFI/OC/Drivers/
clean:
	rm -f RmbGop.o $(EFI) RmbGop.lib
