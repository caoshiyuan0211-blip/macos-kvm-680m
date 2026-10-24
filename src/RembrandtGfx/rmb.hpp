//
//  RembrandtGfx shared declarations.
//

#pragma once

#include <Headers/kern_patcher.hpp>

// Kernel printf reaches the serial port (boot-arg serial=3) for the whole boot; IOLog stops being
// mirrored there once os_log takes over, and kprintf only works early.
extern "C" int printf(const char *fmt, ...) __printflike(1, 2);
#define LOG(fmt, ...) printf("RembrandtGfx: " fmt "\n", ##__VA_ARGS__)
// -rmbverbose: bulk bring-up tracing (DAL log, DAL write log, DMUB ring dumps, register
// snapshots). Off by default: ~1 MB of synchronous serial printf slows the whole boot.
bool rmbVerbose();
void safeModesLog();

// Generic six-argument shape: SysV passes the first six integer args in registers, so wrapping
// a function with fewer args this way is safe.
using Fn6 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// GPU register aperture (BAR5, 512 KB), mapped on first use. Offsets are dword indices.
uint32_t mmioRead(uint32_t reg);
void mmioWrite(uint32_t reg, uint32_t value);
void applyGcGolden();
void haltGfxEngines(const char *why);
void smuExitReset(const char *tag);
void startGoldenGuard();
void syncSdmaAddrConfig(const char *when);
void dmubPollerStart();
void dmubResetForLoad();
void inboxReset();
// VBIOSSMC (Rembrandt display SMU mailbox) message; returns the response, arg reg holds the granted value.
uint32_t vbiosSmcSend(uint32_t msg, uint32_t param);
void edpPanelPowerLost();
// System shutdown/sleep is coming: let DAL turn the eDP panel off for real (nullptr: stop).
void edpAllowPowerOff(const char *why);
// Send the panel power-off DAL sent earlier (and we NULLed) ourselves, before shutdown resets the GPU.
void edpPowerOffNow(const char *why);
// Post-S3: re-trigger a panel connect if WindowServer leaves the display on with no OTG running.
extern bool gWakeWatchArmed;
void panelWakeWatch();
void panelRetrain();
// eDP link watchdog (display.cpp): checks the sink's lane lock over AUX0 every 2 s, CPU retrain if lost.
void edpLinkWatchStart();
void auxDalTouch(uint32_t hw);
extern bool gEdpLinkPause;
extern volatile bool gEdpLinkKick;
// Ring of the last DAL register writes (reg = DAL index, hw = real index), for post-mortems.
void traceWrite(uint32_t reg, uint32_t hw, uint32_t val);
void traceDump(const char *why);
// eDP PHY A register block (RDPCSTX0_RDPCS_TX_CNTL, dword index); reads 0 while the PHY is gated.
constexpr uint32_t PhyA = 0x5df0;
// -rmbphypoll: CPU reads of RDPCSTX0 around every DAL access and every poller tick. Off by default:
// the RDPCS bus is shared with the DMUB firmware's PHY CR accesses (Linux never reads it from the CPU),
// and a cold PHY (S3 wake) died right after one of these reads.
bool phyPollOn();
// RDPCSTX0 for log lines: the real value with -rmbphypoll, else 0xeeeeeeee (not read).
uint32_t phyPeek();
// AppleGraphicsDeviceControl load address (its __TEXT), for AGDC statics.
void displaySetAgdcBase(mach_vm_address_t base);
void displayRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
// AppleBacklight (AppleIntelPanelA under PNLF): panel table so it publishes "brightness".
void displayRouteBacklight(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
bool mmioAvailable();
// Disable + drain the IH rings left running by a previous VM run (INTx stuck asserted otherwise).
void quiesceIh(const char *why);
void ihQuiesceAtBoot();
// 64-bit read from CPU-visible VRAM (BAR0) at a VRAM offset; ~0 if unavailable.
uint64_t vramRead64(uint64_t off);
// 32-bit write to CPU-visible VRAM (BAR0 window only).
bool vramWrite32(uint64_t off, uint32_t value);

// SMU 13.0.1 shim: replaces HWLibs' SMU SWIP entry points (sw_init/hw_init/hw_exit/sw_exit).
void smuRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// GC firmware substitution (Navi 23 microcode descriptors -> embedded Rembrandt images).
void firmwareRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// VRAM probes (framebuffer memory size, accelerator AMDHWMemory).
void probeRouteFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
void probeRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

// GCVM fault watchdog (logs page-table walks for new faults).
void vmWatchStart();
void gartCensus(const char *when);
// Save the GART at S3 entry; on wake put back PTEs that were valid then and are zero now.
void gartSave();
void gartRestore(const char *when);

// Drops the vmware-svga IONDRVFramebuffer's system-sleep veto (guest ACPI S3).
void sleepRoute(KernelPatcher &patcher);
void umdRoute(KernelPatcher &patcher);
