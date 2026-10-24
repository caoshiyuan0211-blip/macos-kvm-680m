//
//  Direct access to the 680M's register aperture, for the parts we drive ourselves.
//

#include "rmb.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <IOKit/pci/IOPCIDevice.h>

static volatile uint32_t *gMmio;
static IOMemoryMap *gMmioMap;
static constexpr uint64_t RegApertureSize = 0x80000;  // "Reg Aper: 512 KB"

static uint32_t dataU32(IORegistryEntry *e, const char *key) {
	auto d = OSDynamicCast(OSData, e->getProperty(key));
	return d && d->getLength() >= 4 ? *static_cast<const uint32_t *>(d->getBytesNoCopy()) : 0;
}

static bool mapMmio() {
	if (gMmio)
		return true;
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!matching)
		return false;
	auto iter = IOService::getMatchingServices(matching);
	matching->release();
	if (!iter)
		return false;
	while (auto svc = OSDynamicCast(IOService, iter->getNextObject())) {
		if (dataU32(svc, "vendor-id") != 0x1002 || (dataU32(svc, "class-code") >> 16) != 0x03)
			continue;
		for (unsigned i = 0; i < svc->getDeviceMemoryCount(); i++) {
			auto mem = svc->getDeviceMemoryWithIndex(i);
			if (!mem || mem->getLength() != RegApertureSize)
				continue;
			gMmioMap = mem->map(kIOMapInhibitCache);
			if (gMmioMap) {
				gMmio = reinterpret_cast<volatile uint32_t *>(gMmioMap->getVirtualAddress());
				LOG("mapped register aperture (device memory %u)", i);
			}
			break;
		}
		break;
	}
	iter->release();
	return gMmio != nullptr;
}

bool mmioAvailable() { return mapMmio(); }

uint32_t mmioRead(uint32_t reg) {
	return mapMmio() && reg < RegApertureSize / 4 ? gMmio[reg] : 0xFFFFFFFF;
}

void mmioWrite(uint32_t reg, uint32_t value) {
	if (mapMmio() && reg < RegApertureSize / 4)
		gMmio[reg] = value;
}

// CPU-visible VRAM (BAR0, 256 MB window onto the carve-out), mapped one page at a time.
static IODeviceMemory *gVram;
static IOMemoryMap *gVramPage;
static uint64_t gVramPageOff = ~0ULL;

static bool findVram() {
	if (gVram)
		return true;
	auto matching = IOService::serviceMatching("IOPCIDevice");
	auto iter = matching ? IOService::getMatchingServices(matching) : nullptr;
	if (matching)
		matching->release();
	if (!iter)
		return false;
	while (auto svc = OSDynamicCast(IOService, iter->getNextObject())) {
		if (dataU32(svc, "vendor-id") != 0x1002 || (dataU32(svc, "class-code") >> 16) != 0x03)
			continue;
		for (unsigned i = 0; i < svc->getDeviceMemoryCount(); i++) {
			auto mem = svc->getDeviceMemoryWithIndex(i);
			if (mem && mem->getLength() >= 0x10000000) {
				gVram = mem;
				break;
			}
		}
		break;
	}
	iter->release();
	return gVram != nullptr;
}

// Indirect VRAM access through MM_INDEX (0x0) / MM_DATA (0x1) / MM_INDEX_HI (0x6), as Linux
// amdgpu_device_mm_access does, for offsets beyond the CPU-visible BAR window.
static uint32_t vramReadIndirect32(uint64_t off) {
	mmioWrite(0x0, static_cast<uint32_t>(off) | 0x80000000u);
	mmioWrite(0x6, static_cast<uint32_t>(off >> 31));
	return mmioRead(0x1);
}

// Kernel pointer to VRAM offset off (inside the BAR window), via a one-page mapping; null if unavailable.
static volatile uint8_t *vramPtr(uint64_t off) {
	uint64_t page = off & ~0xFFFULL;
	if (page != gVramPageOff) {
		if (gVramPage)
			gVramPage->release();
		gVramPage = gVram->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, page, 0x1000);
		gVramPageOff = gVramPage ? page : ~0ULL;
		if (!gVramPage)
			return nullptr;
	}
	return reinterpret_cast<volatile uint8_t *>(gVramPage->getVirtualAddress() + (off & 0xFFF));
}

uint64_t vramRead64(uint64_t off) {
	if (!findVram())
		return ~0ULL;
	if (off + 8 > gVram->getLength())
		return vramReadIndirect32(off) | (uint64_t)vramReadIndirect32(off + 4) << 32;
	auto p = vramPtr(off);
	return p ? *reinterpret_cast<volatile uint64_t *>(p) : ~0ULL;
}

bool vramWrite32(uint64_t off, uint32_t value) {
	if (!findVram() || off + 4 > gVram->getLength())
		return false;
	auto p = vramPtr(off);
	if (!p)
		return false;
	*reinterpret_cast<volatile uint32_t *>(p) = value;
	return true;
}

// OSSSYS (IH) rings 0-2: {cntl, rptr, wptr} offsets from OSSSYS seg0 base 0x10A0 (osssys 5.0 layout).
// A VM run leaves IH ring 0 enabled with unacknowledged entries: the 680M then holds INTx asserted
// (PCI STATUS bit 3, even with no VM running) and keeps DMA-writing ring entries / the wptr
// writeback into the IOVAs the old guest set up, which the next guest's RAM reuses. In the guest
// INTx pin A shares IRQ 10 with the ICH9 SATA controller, so AppleAHCI's handler spins and the
// whole boot crawls (restart3: AHCI command timeouts, vCPU in AppleAHCI::PrimaryInterruptHandler).
// Disable and drain the rings like Linux navi10_ih_disable_interrupts (-rmbnoihq: don't).
void quiesceIh(const char *why) {
	if (checkKernelArgument("-rmbnoihq"))
		return;
	static constexpr uint32_t Base = 0x10A0;
	static constexpr struct { uint32_t cntl, rptr, wptr; } rings[] {{0x80, 0x83, 0x84}, {0x8c, 0x8f, 0x90}, {0x98, 0x9b, 0x9c}};
	for (size_t i = 0; i < arrsize(rings); i++) {
		auto &r = rings[i];
		uint32_t cntl = mmioRead(Base + r.cntl), rptr = mmioRead(Base + r.rptr), wptr = mmioRead(Base + r.wptr);
		if (cntl == 0xFFFFFFFF) {
			LOG("ihq: %s: register aperture not readable", why);
			return;
		}
		if (i == 0)
			cntl &= ~(1U << 17);  // ENABLE_INTR (ring 0 only)
		mmioWrite(Base + r.cntl, cntl & ~1U);  // RB_ENABLE
		mmioWrite(Base + r.rptr, 0);
		mmioWrite(Base + r.wptr, 0);
		LOG("ihq: %s: ring %zu cntl %08x rptr %x wptr %x -> cntl %08x rptr %x wptr %x", why, i, cntl, rptr, wptr,
		    mmioRead(Base + r.cntl), mmioRead(Base + r.rptr), mmioRead(Base + r.wptr));
	}
}

static IONotifier *gPciNotifier;
// At publish macOS has not enabled memory decode yet: turn it on for the quiesce, then restore
// COMMAND. If INTx is still pending afterwards, set Interrupt Disable (Apple's driver uses MSI).
// Virtual calls only (no IOPCIFamily symbol imports); the nub was identified by its properties.
static bool pciPublished(void *, void *, IOService *svc, IONotifier *) {
	if (dataU32(svc, "vendor-id") != 0x1002 || (dataU32(svc, "class-code") >> 16) != 0x03)
		return true;
	auto pci = static_cast<IOPCIDevice *>(svc);
	IOPCIAddressSpace sp;
	sp.bits = dataU32(svc, "reg") & 0x00FFFF00;  // phys.hi: bus/device/function, config space
	uint16_t cmd = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigCommand)), st = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus));
	pci->configWrite16(sp, static_cast<UInt8>(kIOPCIConfigCommand), cmd | kIOPCICommandMemorySpace);
	quiesceIh("at PCI publish");
	uint16_t st2 = pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus));
	uint16_t newCmd = cmd;
	if ((st2 & 0x8) && !checkKernelArgument("-rmbkeepintx"))
		newCmd |= kIOPCICommandInterruptDisable;
	pci->configWrite16(sp, static_cast<UInt8>(kIOPCIConfigCommand), newCmd);
	LOG("ihq: PCI command %04x status %04x -> after quiesce status %04x, command %04x (status now %04x)", cmd, st, st2,
	    pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigCommand)), pci->configRead16(sp, static_cast<UInt8>(kIOPCIConfigStatus)));
	return true;
}

void ihQuiesceAtBoot() {
	auto matching = IOService::serviceMatching("IOPCIDevice");
	if (!matching)
		return;
	gPciNotifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, pciPublished, nullptr);
	matching->release();
	LOG("ihq: PCI publish notifier %s", gPciNotifier ? "armed" : "MISSING");
}
