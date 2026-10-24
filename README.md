# macOS on a Radeon 680M (KVM + VFIO)

macOS Sequoia 15.8.1 running in a QEMU/KVM virtual machine on a **GPD Win Max 2 (Ryzen 7 6800U)**, with the
**Radeon 680M iGPU passed through** and driving the laptop's own built-in panel. It has full Metal
acceleration from Apple's own AMD driver.

Apple never shipped a driver for Rembrandt (RDNA2 APU, GC 10.3.3 / DCN 3.1.2 / PSP 13 / SMU 13). This
project makes Apple's **Navi 23 driver stack** (`AMDRadeonX6000`, `AMDRadeonX6000Framebuffer`, HWLibs) run on it
through a Lilu plugin, **RembrandtGfx**, that patches the differences at run time.

It's half a hackintosh: Linux owns the machine, and macOS gets the GPU, panel, Wi-Fi, Bluetooth, sound,
touchscreen and battery.

## What works

| | |
|---|---|
| Metal / GPU acceleration | ✅ GFX, compute, SDMA (Apple's Navi 23 kexts + RembrandtGfx) |
| Internal eDP panel 2560x1600 @ 60 Hz | ✅ including the Displays pane sizes (16:10, HiDPI), and native brightness slider/keys |
| HDMI out | ✅ up to 4K120 on the GPD HDMI port, extended or mirrored |
| Sleep | ✅ guest ACPI S3 + host S3 round trip; panel, GPU, Wi-Fi and input come back |
| Shutdown | ✅ clean panel power-off |
| Wi-Fi | ✅ Intel AX210 passed through, native Wi-Fi menu (AirportItlwm) |
| Bluetooth | ✅ AX210 BT (IntelBluetoothFirmware + BlueToolFixup) |
| Sound | ✅ Conexant SN6140 speakers/headphones via VoodooHDA |
| Battery | ✅ real host battery, via a small QEMU device + SSDT + SMCBatteryManager |
| Touchscreen | ✅ Goodix touchscreen, through a host forwarder and a guest event injector |
| App Store, iCloud, macOS updates | ✅ |

## What doesn't

- **Hardware video decode/encode (VCN):** hidden on purpose, so VideoToolbox uses software codecs. Apple's
  Navi MMHUB 2.0 page-table invalidation hangs the GPU on Rembrandt's MMHUB 2.3.
- **AirPlay screen mirroring:** disabled, because it needs VCN. **AirDrop** is impossible on the AX210, which has no AWDL.
- **Restarting the VM** needs a host reboot. The PSP can't be re-initialised twice in one host boot.
- While the VM runs, the 680M belongs to macOS. Linux has no display unless you add a second GPU
  (an eGPU here) or run it headless.

## How it works

`src/RembrandtGfx` is a [Lilu](https://github.com/acidanthera/Lilu) plugin. Lilu's `onKextLoad` never fires for
the AMD kexts in the System KC, so the plugin routes `OSKext::start` and scans `_kmod` itself. The main pieces:

- **Accel:** PSP 13.0.x reported as 11.0.x, with destroy-rings before every ring create. An SMU 13 shim talks
  to the real mailbox. GC 10.3.3 is reported as Navi, with Linux gc_10_3_3 golden registers. GC/SDMA/RLC/MEC
  firmware is swapped for the yellow_carp blobs (`scripts/fw-extract.py`). Other fixes: one SDMA engine
  instead of two, UMA/GART page-table fixes, `SDMA0_GB_ADDR_CONFIG` sync, and PowerPlay chain fixes.
- **Display (DCN 3.0.2 → 3.1.2):** register and field remap tables (`dcnremap.hpp`) plus a PHY hole filter;
  a DAL read of an undefined PHY register wedges the bus. DMUB command filters cover eDP power, PLL
  power-down, FAMS and ABM. The pipe count is capped 5 → 4. DET and system-aperture setup for the second
  pipe make HDMI work. There is also a DP DTO fix for the 600 MHz dprefclk and dispclk votes for 4K120.
- **Panel:** an eDP link watchdog retrains the panel itself over AUX after wake. PWRSEQ backlight revive.
  An ApplePanels entry gives macOS native brightness. Safe/NeverShow mode flags make the Displays pane work.
- **Textures/icons:** the Metal UMD's `HwlComputePipeBankXor` is patched to 0 in code pages via
  `cs_validate_page`. The GPU's pipe/bank-XOR swizzle didn't match the Navi 23 assumption, which scrambled icons.
- **Sleep:** GART save/restore, engine halt + MODE2 reset at sleep, PSP teardown, and a post-wake display
  watchdog.

Every fix has a `-rmb*` boot-arg that turns it off, documented in the source.

`src/RmbGop` is a small UEFI driver. It takes over the scanout the host firmware left on the 680M, so the
OpenCore/Apple boot logo shows on the internal panel. The logo still dies a few seconds into kernel start, and the panel comes back when the driver loads.

`qemu/qemu-11.1.2-hkvm.patch` changes QEMU 11.1.2:
- vmxnet3 survives guest S3
- evdev input gets an escape hotkey, a touchpad mode and a sleep-time input mute
- brightness keys map to F14/F15
- adds the `host-battery` ISA device

## Layout

```
src/RembrandtGfx   the kext (Lilu plugin)
src/RmbGop         UEFI GOP driver for the 680M
src/acpi           SSDT-HOSTBAT (battery), SSDT-PNLF (backlight)
src/USBMapQemu.kext  codeless USB map for qemu-xhci
src/{touchd,sleepmute,setext,setres,setmain,brtest,metaltest,...}  small guest tools
qemu/              QEMU patch
scripts/           host side: boot.sh, pt-reboot.sh, sleepsync.py, touch-host.py, register/DMUB/DPCD tools
guest/             guest helper scripts + LaunchDaemons (sleepmute, wakefix, touchd, agent)
host/              udev rules, vfio bind service, sudoers, systemd drop-ins
efi/config.plist.example  OpenCore 1.0.5 config (SMBIOS blanked)
```

## Building

**The kext** needs clang, an ld64 (built from [cctools-port](https://github.com/tpoechtrager/cctools-port), expected at
`build/cctools`), [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) at `src/MacKernelSDK`, and Lilu 1.7.2 at
`src/Lilu-1.7.2`:

```sh
cd src/RembrandtGfx
make            # also splits /usr/lib/firmware/amdgpu/yellow_carp_*.bin into fw/ (FWSRC=... to override)
make install    # copies RembrandtGfx.kext into efi/EFI/OC/Kexts
```

`make check` needs symbol lists of the guest kernel and Lilu in `build/kpi/` and is optional.

**QEMU:** check out v11.1.2, `git apply qemu/qemu-11.1.2-hkvm.patch`, and build into `build/qemu-wake/build-dir`.

**RmbGop:** `make` in `src/RmbGop` (clang + lld-link).

## Running (short version)

None of the following is included in this repo. You need:

- An OpenCore 1.0.5 EFI with Lilu, VirtualSMC, SMCBatteryManager, AirportItlwm (no WhateverGreen), the BT kexts,
  RembrandtGfx and RmbGop. Start from `efi/config.plist.example`, generate your own SMBIOS
  (`macserial -m MacPro7,1`), and put the EFI in `disks/opencore.img` (`scripts/sync-efi.sh`).
- A macOS install in `disks/macos.qcow2`.
- A 680M VBIOS image made from your own machine's ACPI VFCT table (`scripts/vfct-rom.py`,
  `scripts/augment-rom.py`) in `roms/`.
- Host setup: the host must not load amdgpu, and the 680M must be bound to vfio-pci with `disable_idle_d3=1`
  (`host/bind-680m.service`). Install the udev rules for evdev/touch, set `kvm.ignore_msrs=1`, and raise
  memlock.

Then `scripts/pt-reboot.sh TAG` (or `scripts/autostart-vm.sh` from a desktop autostart entry) does the
passthrough boot. `scripts/boot.sh` documents its environment variables at the top.

This was built for one machine, and the scripts carry its PCI IDs, input devices and quirks. Treat it as a reference
and a starting point, not a turnkey installer.

## Legal

No Apple software, Apple-derived binaries, AMD firmware or VBIOS images are in this repository. Firmware is
taken from your own `linux-firmware` at build time. You need your own copy of macOS, and you are responsible
for following Apple's license. Not affiliated with Apple or AMD.

Thanks to the acidanthera projects (Lilu, OpenCore, VirtualSMC, MacKernelSDK, BlueToolFixup), OpenIntelWireless
(itlwm/AirportItlwm, IntelBluetoothFirmware), VoodooHDA, QEMU, and the Linux amdgpu/DC driver, whose source
was the reference for the Rembrandt hardware.

## License

GPL-2.0-only, see [LICENSE](LICENSE).
