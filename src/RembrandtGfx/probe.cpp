//
//  VRAM probes. The accelerator's VRAM heap comes up empty on the 680M ("Failed to allocate ...
//  0 free memory remaining"), so log what the framebuffer and AMDHWMemory derive VRAM from.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>

namespace {

mach_vm_address_t orgPopulateMemorySize, orgHWMemInit, orgInitVRAMInfo, orgEnableAllocations,
	orgSetReservedVRAMOffset, orgUpdateHWInfoClocks, orgShouldAllocScanoutDcc, orgSdmaReset, orgResetHardware, orgPageOnOff, orgDoStretch, orgDoClear, orgDoExpand;

uint64_t q(uint64_t obj, uint32_t off) { return *reinterpret_cast<uint64_t *>(obj + off); }

// AmdAsicInfoNavi2: +0x120 RCC_CONFIG_MEMSIZE << 20, +0x128 CONFIG_APER_SIZE << 20,
// +0x12c CONFIG_REG_APER_SIZE.
uint64_t wrapPopulateMemorySize(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgPopulateMemorySize)(self, b, c, d, e, f);
	LOG("populateMemorySize: memsize %llx aper %x reg aper %x; fb loc base %llx top %llx offset %llx",
		q(self, 0x120), *reinterpret_cast<uint32_t *>(self + 0x128), *reinterpret_cast<uint32_t *>(self + 0x12c),
		q(self, 0x110), q(self, 0x118), q(self, 0x120));
	return r;
}

void dumpHWMem(const char *when, uint64_t self) {
	LOG("hwmem %s: +28 %llx +38 %llx +40 %llx +48 %llx +50 %llx +58 %llx +60 %llx +68 %llx +70 %llx", when,
		q(self, 0x28), q(self, 0x38), q(self, 0x40), q(self, 0x48), q(self, 0x50), q(self, 0x58),
		q(self, 0x60), q(self, 0x68), q(self, 0x70));
}

uint64_t wrapHWMemInit(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgHWMemInit)(self, b, c, d, e, f);
	dumpHWMem(r & 0xFF ? "init ok" : "init FAILED", self);
	return r;
}

uint64_t wrapInitVRAMInfo(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgInitVRAMInfo)(self, b, c, d, e, f);
	dumpHWMem(r & 0xFF ? "initVRAMInfo ok" : "initVRAMInfo FAILED", self);
	return r;
}

uint64_t wrapEnableAllocations(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	dumpHWMem("enableAllocations", self);
	auto r = reinterpret_cast<Fn6>(orgEnableAllocations)(self, b, c, d, e, f);
	LOG("enableAllocations = %llx", r & 0xFF);
	vmWatchStart();
	return r;
}

uint64_t wrapSetReservedVRAMOffset(uint64_t self, uint64_t off, uint64_t size, uint64_t d, uint64_t e, uint64_t f) {
	LOG("setReservedVRAMOffset(%llx, %llx)", off, size);
	return reinterpret_cast<Fn6>(orgSetReservedVRAMOffset)(self, off, size, d, e, f);
}

// AMDHardware::updateHWInfoClocks asks PowerPlay (stubbed) for current clocks and stores
// sysClk/memClk (10 kHz units) at +0x198/+0x1a0 -> both 0. AMDAccelDevice::getHardwareInfo
// then rejects every userspace HW-info query (0xe00002bc), so the GL driver's compute device
// stays NULL and anything using CoreImage/CGL crashes (Terminal, iconservicesagent, Dock).
uint64_t wrapUpdateHWInfoClocks(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgUpdateHWInfoClocks)(self, b, c, d, e, f);
	auto sys = reinterpret_cast<uint64_t *>(self + 0x198), mem = reinterpret_cast<uint64_t *>(self + 0x1a0);
	if (!*sys || !*mem) {
		LOG("hwclk: PowerPlay gave sysClk %llu memClk %llu -> 2200/1600 MHz", *sys, *mem);
		if (!*sys) *sys = 220000;
		if (!*mem) *mem = 160000;
		r = 1;
	}
	return r;
}

// Channel restarts (e.g. a hung SDMA0_PAGE) reset the SDMA engine, which may put
// SDMA0_GB_ADDR_CONFIG back to its 0x444 reset value; re-sync it after every reset.
uint64_t wrapSdmaReset(uint64_t self, uint64_t ring, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG("sdma-reset: ring type %llu", ring);
	auto r = reinterpret_cast<Fn6>(orgSdmaReset)(self, ring, c, d, e, f);
	syncSdmaAddrConfig("after SDMA reset");
	return r;
}

uint64_t wrapResetHardware(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG("hw-reset: resetHardware(%llx)", b);
	auto r = reinterpret_cast<Fn6>(orgResetHardware)(self, b, c, d, e, f);
	syncSdmaAddrConfig("after resetHardware");
	return r;
}

// Managed-texture uploads (CPU copy -> GPU) go through pageOnOffBuffer as a UBM blit. Dump the
// src/dst _UBM_SURFINFO (0x2c8 bytes; +0x5c width, +0x60 height) of square 64/96 px surfaces to
// compare a working (no DCC) and a stale (DCC-able RT) upload.
using FnPageOnOff = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void dumpSurf(const char *tag, uint64_t si) {
	auto d = reinterpret_cast<const uint32_t *>(si);
	for (uint32_t i = 0; i < 0x2c8 / 4; i += 8)
		LOG("pageon: %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", tag, i * 4, d[i], d[i + 1], d[i + 2], d[i + 3],
			i + 4 < 0x2c8 / 4 ? d[i + 4] : 0, i + 5 < 0x2c8 / 4 ? d[i + 5] : 0, i + 6 < 0x2c8 / 4 ? d[i + 6] : 0,
			i + 7 < 0x2c8 / 4 ? d[i + 7] : 0);
}

static thread_t gPageOnThread;

// UBM ops issued from inside a dumped pageOnOffBuffer call (same thread).
template <uint64_t *Org, char Op>
uint64_t wrapUbmOp(uint64_t self, uint64_t info, uint64_t opts, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(*Org)(self, info, opts, d, e, f);
	if (gPageOnThread == current_thread()) {
		auto o = reinterpret_cast<const uint32_t *>(opts);
		LOG("pageon:  ubm %c -> %llx, opts %08x %08x %08x %08x", Op, r & 0xffffffff, o ? o[0] : 0, o ? o[1] : 0,
			o ? o[2] : 0, o ? o[3] : 0);
	}
	return r;
}

uint64_t wrapPageOnOff(uint64_t self, uint64_t src, uint64_t dst, uint64_t srcMap, uint64_t dstMap, uint64_t dir,
	uint64_t srcRect, uint64_t dstRect) {
	static uint32_t dumps;
	uint32_t w = *reinterpret_cast<uint32_t *>(dst + 0x5c), h = *reinterpret_cast<uint32_t *>(dst + 0x60);
	bool dump = w == h && (w == 64 || w == 72 || w == 96) && dumps < 12;
	if (dump)
		gPageOnThread = current_thread();
	auto r = reinterpret_cast<FnPageOnOff>(orgPageOnOff)(self, src, dst, srcMap, dstMap, dir, srcRect, dstRect);
	gPageOnThread = nullptr;
	if (dump) {
		dumps++;
		LOG("pageon: %ux%u dir %llu -> %llu, blt flags %x", w, h, dir, r & 0xff,
			*reinterpret_cast<uint32_t *>(q(self, 0x18) + 0xdd8));
		dumpSurf("src", src);
		dumpSurf("dst", dst);
	}
	return r;
}

// Scanout-capable surfaces (window layers, decoded images, WebGL canvases) get DCC when the
// accelerator's GPUDCCDisplayable flag is set. Our DCN path strips DCC from HUBP (its meta
// addressing is wrong on 3.1.2), and CPU/scanout readers then see raw compressed blocks:
// garbled About-This-Mac image / Launchpad icons, WebGL trails, white flashes.
// -rmbscandcc keeps Apple's behaviour.
bool gKeepScanoutDcc;
uint64_t wrapShouldAllocScanoutDcc(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	if (gKeepScanoutDcc) return reinterpret_cast<Fn6>(orgShouldAllocScanoutDcc)(self, b, c, d, e, f);
	static int n;
	if (n++ < 3) LOG("scandcc: shouldAllocScanoutDcc(%llx, %llx, %llx, %llx) -> 0", b, c, d, e);
	return 0;
}

#define PROBE_TRACE(name)                                                                     \
	mach_vm_address_t org_##name;                                                            \
	uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx, %llx)", b, c);                                                                    \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                       \
		LOG("<< " #name " = %llx", r);                                                       \
		return r;                                                                            \
	}

PROBE_TRACE(AccelPowerUpHW)
// AMDGFX10Hardware::initHWInfo (run before powerUp) builds the chip description user space gets:
// +0x40, +0x64 SE count, +0x68, +0x6c SH per SE, +0x70 u32 CU masks [se][2], +0xa8, +0xf8 u64
// masks, +0x158.. copies. Dump it to see what the Metal driver is told about Rembrandt.
mach_vm_address_t org_Navi23HwPowerUp;
uint64_t wrap_Navi23HwPowerUp(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	LOG(">> Navi23HwPowerUp");
	auto u = [&](uint32_t off) { return *reinterpret_cast<uint32_t *>(a + off); };
	for (uint32_t off = 0x40; rmbVerbose() && off < 0x1f0; off += 0x20)
		LOG("hwinfo +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", off, u(off), u(off + 4), u(off + 8), u(off + 12),
			u(off + 16), u(off + 20), u(off + 24), u(off + 28));
	auto r = reinterpret_cast<Fn6>(org_Navi23HwPowerUp)(a, b, c, d, e, f);
	LOG("<< Navi23HwPowerUp = %llx", r);
	return r;
}
PROBE_TRACE(HWMemSetVirtualSpace)
PROBE_TRACE(PM4DoStart)
PROBE_TRACE(PM4InitComputeMQD)
PROBE_TRACE(PM4InitGraphicsMQD)

const char *className(uint64_t obj) {
	return obj ? reinterpret_cast<OSObject *>(obj)->getMetaClass()->getClassName() : "(null)";
}

// GC register dump (gc_10_3_0 offsets; GC seg0 0x1260, seg1 0xA000 on both chips).
constexpr uint32_t GC0 = 0x1260, GC1 = 0xA000;
uint32_t gc(uint32_t off) { return mmioRead(GC0 + off); }

void dumpHQD(uint32_t me, uint32_t pipe, uint32_t queue) {
	auto old = gc(0x0dc2);
	mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);  // GRBM_GFX_CNTL
	LOG("hqd me%u pipe%u q%u: active %x vmid %x base %x_%08x rptr %x wptr %x_%x ctl %x db %x mqd %x deq %x",
		me, pipe, queue, gc(0x1fab), gc(0x1fac), gc(0x1fb2), gc(0x1fb1), gc(0x1fb3), gc(0x1fe0), gc(0x1fdf),
		gc(0x1fba), gc(0x1fb8), gc(0x1fa9), gc(0x1fc1));
	if (me == 2) {
		mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);
		LOG("hqd me%u pipe%u q%u: eop %x_%x ctl %x mqd hi %x ctl %x rptr_rep %x_%x wptr_poll %x_%x persist %x ib %x prio %x quantum %x hq_st0 %x hq_ctl0 %x err %x eop_wptr %x",
			me, pipe, queue, gc(0x1fcf), gc(0x1fce), gc(0x1fd0), gc(0x1faa), gc(0x1fcb), gc(0x1fb5), gc(0x1fb4),
			gc(0x1fb7), gc(0x1fb6), gc(0x1fad), gc(0x1fbe), gc(0x1fae), gc(0x1fb0), gc(0x1fc9), gc(0x1fca),
			gc(0x1fdc), gc(0x1fdd));
		mmioWrite(GC0 + 0x0dc2, old);
		LOG("cp: PQ_WPTR_POLL_CNTL %x PQ_STATUS %x ME2P1_INT %x INT_STATUS %x ECC %x CPC_STALL %x CPF_STALL %x CPF_BUSY %x hdr dump me1 %x me2 %x",
			gc(0x1e23), gc(0x1e58), gc(0x1e32), gc(0x1dea), gc(0x1e1a), gc(0x0e26), gc(0x0e29), gc(0x0e28), gc(0x0e2e), gc(0x0e2f));
	}
}

void dumpCP(const char *when) {
	if (!mmioAvailable())
		return;
	LOG("cp %s: GRBM_STATUS %x STATUS2 %x CP_STAT %x CPC_STATUS %x CPC_BUSY %x CPF_STATUS %x MEC_CNTL %x", when,
		gc(0x0da4), gc(0x0da2), gc(0x0f40), gc(0x0e24), gc(0x0e25), gc(0x0e27), gc(0x0f55));
	LOG("cp %s: MEC1 IP %x MEC2 IP %x DB range %x-%x RLC_CNTL %x RLC_STAT %x RLC_CP_SCHED %x CPC_IC_BASE_LO %x", when,
		gc(0x0f48), gc(0x0f49), gc(0x1dfc), gc(0x1dfd), mmioRead(GC1 + 0x4c00), mmioRead(GC1 + 0x4c04),
		mmioRead(GC1 + 0x4ca1), mmioRead(GC1 + 0x584c));
	LOG("gcvm %s: FB %x-%x off %x AGP %x %x-%x SYSAP %x-%x def %x_%x L1TLB %x L2 %x", when,
		gc(0x16fc), gc(0x16fd), gc(0x16e7), gc(0x1700), gc(0x16ff), gc(0x16fe), gc(0x1701), gc(0x1702),
		gc(0x16e9), gc(0x16e8), gc(0x1703), gc(0x15bc));
	LOG("gcvm %s: ctx0 cntl %x pt %x_%08x start %x_%08x end %x_%08x fault status %x addr %x_%08x cntl %x utcl1 cpc %x cpf %x",
		when, gc(0x15fc), gc(0x1668), gc(0x1667), gc(0x1688), gc(0x1687), gc(0x16a8), gc(0x16a7),
		gc(0x15c8), gc(0x15ca), gc(0x15c9), gc(0x15c4), gc(0x1f55), gc(0x1f56));
	// GART (VMID0, flat table): PTE for the faulting page. PT base is the physical address;
	// minus FB_OFFSET << 24 gives the VRAM offset.
	uint64_t ptBase = ((uint64_t)gc(0x1668) << 32 | gc(0x1667)) & ~0xFFFULL;
	uint64_t fbPhys = (uint64_t)(gc(0x16e7) & 0xFFFFFF) << 24;
	uint64_t start = (uint64_t)gc(0x1688) << 32 | gc(0x1687);
	uint64_t fault = (uint64_t)gc(0x15ca) << 32 | gc(0x15c9);
	if (ptBase >= fbPhys && fault >= start) {
		uint64_t pt = ptBase - fbPhys, idx = fault - start;
		LOG("gart %s: pt vram %llx, fault page idx %llx: pte %llx (prev %llx next %llx), pte[0] %llx", when, pt, idx,
			vramRead64(pt + idx * 8), vramRead64(pt + idx * 8 - 8), vramRead64(pt + idx * 8 + 8), vramRead64(pt));
	}
	dumpHQD(1, 0, 0);
	dumpHQD(2, 1, 0);
}

mach_vm_address_t orgSubmitKIQFrame;
uint64_t wrapSubmitKIQFrame(uint64_t self, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgSubmitKIQFrame)(self, b, c, d, e, f);
	if (!(r & 0xFF))
		dumpCP("KIQ submit failed");
	return r;
}

// The APU only gets a light reset between VM runs (and from the host's amdgpu before that), so HQDs
// mapped earlier are still active and the MEC firmware may be wedged on an old KIQ packet; mapping
// queues again then hangs the KIQ. Halt the CP (as Linux cp_compute_enable/cp_gfx_enable(false)),
// deactivate every HQD by register, and unhalt so the firmware restarts from a clean state.
void cpResetQueues() {
	constexpr uint32_t MecHalt = (1u << 30) | (1u << 28);              // MEC_ME1_HALT | MEC_ME2_HALT
	constexpr uint32_t MeHalt = (1u << 28) | (1u << 26) | (1u << 24);  // ME | PFP | CE
	auto old = gc(0x0dc2);
	LOG("cp reset: MEC_CNTL %x ME_CNTL %x CP_STAT %x", gc(0x0f55), gc(0x0f56), gc(0x0f40));
	mmioWrite(GC0 + 0x0f55, MecHalt);
	mmioWrite(GC0 + 0x0f56, gc(0x0f56) | MeHalt);
	IODelay(100);
	uint32_t n = 0;
	for (uint32_t me = 1; me <= 2; me++)
		for (uint32_t pipe = 0; pipe < 4; pipe++)
			for (uint32_t queue = 0; queue < 8; queue++) {
				mmioWrite(GC0 + 0x0dc2, (queue << 8) | (me << 2) | pipe);
				n += gc(0x1fab) & 1;
				mmioWrite(GC0 + 0x1fb8, 0);  // doorbell control
				mmioWrite(GC0 + 0x1fab, 0);  // CP_HQD_ACTIVE
				mmioWrite(GC0 + 0x1fc1, 0);  // DEQUEUE_REQUEST
				mmioWrite(GC0 + 0x1fb3, 0);  // RPTR
				mmioWrite(GC0 + 0x1fdf, 0);  // WPTR
				mmioWrite(GC0 + 0x1fe0, 0);
				mmioWrite(GC0 + 0x1fdc, 0);  // CP_HQD_ERROR
			}
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		mmioWrite(GC0 + 0x0dc2, pipe);
		n += gc(0x1e80) & 1;
		mmioWrite(GC0 + 0x1e80, 0);  // CP_GFX_HQD_ACTIVE
		mmioWrite(GC0 + 0x1e93, 0);
	}
	mmioWrite(GC0 + 0x0dc2, old);
	mmioWrite(GC0 + 0x0f56, gc(0x0f56) & ~MeHalt);
	mmioWrite(GC0 + 0x0f55, 0);
	IODelay(1000);
	LOG("cp reset: %u active queues deactivated; MEC_CNTL %x ME_CNTL %x CP_STAT %x MEC1 IP %x MEC2 IP %x",
		n, gc(0x0f55), gc(0x0f56), gc(0x0f40), gc(0x0f48), gc(0x0f49));
}

// Logging copy of AMDGFX10KIQHWChannel::startKIQ(gpuAddr, gpuAddr2, spec, out).
mach_vm_address_t gAccelBase;
uint64_t wrapStartKIQ(uint64_t self, uint64_t a1, uint64_t a2, uint64_t spec, uint64_t, uint64_t) {
	auto ring = q(self, 0x30);
	struct { uint32_t r0, r4; uint64_t r8, r10, r18, r20, r28, r30; } in {};
	in.r0 = *reinterpret_cast<uint32_t *>(ring + 0x84) & ~3u;
	in.r4 = 0x80;
	in.r8 = q(ring, 0x48), in.r10 = q(ring, 0xb8), in.r18 = q(ring, 0xd0);
	in.r20 = a1, in.r28 = a2, in.r30 = 0x200;
	uint32_t out[6] {};
	gartRestore("before startKIQ");  // S3 wake: Apple never re-maps the lost kernel GART PTEs
	cpResetQueues();  // every start: boot and S3 wake both find stale HQDs
	// Clear the sticky GCVM protection-fault status so later dumps show only new faults.
	mmioWrite(GC0 + 0x15c4, gc(0x15c4) | 1);
	IODelay(10);
	mmioWrite(GC0 + 0x15c4, gc(0x15c4) & ~1u);
	LOG("startKIQ: fault status cleared -> %x", gc(0x15c8));
	auto owner = q(self, 0x20);
	auto svc = reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(owner))[0x2c0 / 8])(owner);
	auto fn = (*reinterpret_cast<uint64_t **>(svc))[0x120 / 8];
	LOG("startKIQ: svc vtable %llx fn %llx (accel base %llx); in %x %x %llx %llx %llx %llx %llx", q(svc, 0), fn, gAccelBase,
		in.r0, in.r4, in.r8, in.r10, in.r18, in.r20, in.r28);
	auto r = static_cast<uint32_t>(reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, void *, void *)>(fn)(svc, 8, 0, &in, out));
	auto sp = reinterpret_cast<uint32_t *>(spec);
	LOG("startKIQ: r %x out %x %x %x (spec %x %x %x) %x%08x", r, out[0], out[1], out[2], sp[0], sp[1], sp[2], out[5], out[4]);
	if (r)
		return r;
	if (out[0] != sp[0] || out[1] != sp[1] || out[2] != sp[2])
		r = 0xe00002bc;
	*reinterpret_cast<uint64_t *>(ring + 0xc0) = *reinterpret_cast<uint64_t *>(&out[4]);
	// The host's amdgpu ran its KIQ on this same HQD; a latched CP_HQD_ERROR (PQ_UTCL1_ERROR) from
	// then survives the APU's light reset and keeps the MEC from servicing the queue. Clear it.
	{
		auto sp2 = reinterpret_cast<uint32_t *>(spec);
		auto old = gc(0x0dc2);
		mmioWrite(GC0 + 0x0dc2, (sp2[2] << 8) | (sp2[0] << 2) | sp2[1]);
		auto err = gc(0x1fdc);
		mmioWrite(GC0 + 0x1fdc, 0);
		LOG("startKIQ: HQD error %x -> %x", err, gc(0x1fdc));
		mmioWrite(GC0 + 0x0dc2, old);
	}
	dumpCP("after startKIQ");
	return r;
}
PROBE_TRACE(KIQMapQueues)
PROBE_TRACE(PM4WaitForActive)

// AMDHardware status word (this+0x18 -> vfunc 0x178, +0x8): bit 0x100 = engines powered,
// 0x200 = engines started. Kept as the originals set them.
void setStatusBit(uint64_t hw, uint16_t bit, bool on) {
	auto obj = *reinterpret_cast<uint64_t *>(hw + 0x18);
	auto fn = reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(obj))[0x178 / 8]);
	auto w = reinterpret_cast<uint16_t *>(fn(obj) + 0x8);
	*w = on ? (*w | bit) : (*w & ~bit);
}

uint64_t callSlot(uint64_t obj, uint32_t slot) {
	return reinterpret_cast<uint64_t (*)(uint64_t)>((*reinterpret_cast<uint64_t **>(obj))[slot / 8])(obj);
}

// AMDHardware::powerUpHWEngines: engines[0..10] at +0x3b0, each ->powerUp (slot 0x138).
uint64_t wrapPowerUpHWEngines(uint64_t hw, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	bool ok = true;
	for (uint32_t i = 0; i < 11 && ok; i++) {
		auto e = *reinterpret_cast<uint64_t *>(hw + 0x3b0 + i * 8);
		if (!e)
			continue;
		ok = callSlot(e, 0x138) & 0xFF;
		LOG("engine %u %s powerUp = %d", i, className(e), ok);
	}
	setStatusBit(hw, 0x100, ok);
	return ok;
}

// AMDHardware::startHWEngines: 2 + 2 * byte[+0xc2] engines from +0x3b8, each ->start (0x148).
uint64_t wrapStartHWEngines(uint64_t hw, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	uint32_t n = 2 + 2 * *reinterpret_cast<uint8_t *>(hw + 0xc2);
	bool ok = true;
	syncSdmaAddrConfig("before engine start");
	for (uint32_t i = 0; i < n && ok; i++) {
		auto e = *reinterpret_cast<uint64_t *>(hw + 0x3b8 + i * 8);
		ok = e && (callSlot(e, 0x148) & 0xFF);
		syncSdmaAddrConfig("after engine start");
		LOG("engine slot %u %s start = %d", i + 1, className(e), ok);
		// Rembrandt has a single SDMA instance; Navi 23's SDMA1 slot (start slot 2) has no hardware.
		if (!ok && i + 1 == 2) {
			LOG("engine slot 2: no SDMA1 on Rembrandt, ignoring");
			ok = true;
		}
	}
	setStatusBit(hw, 0x200, ok);
	return ok;
}

// AMDRTHardware::getHWChannel(channelType, priority, index): for SDMA (type 2) it picks engine
// ((caps+0x92 ? 3 : 1) & index) + 1 and ring caps+0x8f, then AMDHardware::getHWChannel(engine, ring).
// Low-priority work (e.g. the VM page-table channel) lands on engine 2 = SDMA1, which Rembrandt
// doesn't have, so its jobs never complete. Same selection, clamped to SDMA0.
mach_vm_address_t orgRTGetHWChannel, gGetHWChannelEngineRing;
uint64_t wrapRTGetHWChannel(uint64_t hw, uint64_t type, uint64_t prio, uint64_t index, uint64_t e, uint64_t f) {
	if ((type & 0xFFFFFFFF) != 2 || !gGetHWChannelEngineRing)
		return reinterpret_cast<Fn6>(orgRTGetHWChannel)(hw, type, prio, index, e, f);
	auto caps = reinterpret_cast<uint8_t *>(callSlot(hw, 0x158));
	uint32_t p = prio & 0xFFFFFFFF, idx = index & 0xFFFFFFFF, ring = 0;
	if (p == 1)
		ring = caps[0x8f];
	else if (p == 0)
		idx = 1, ring = caps[0x8f];
	uint32_t engine = ((caps[0x92] * 2 + 1) & idx) + 1;
	if (engine != 1) {
		static uint32_t logged;
		if (logged++ < 8)
			LOG("getHWChannel: SDMA prio %u index %u -> engine %u ring %u, using SDMA0", p, idx, engine, ring);
		engine = 1;
	}
	return reinterpret_cast<Fn6>(gGetHWChannelEngineRing)(hw, engine, ring, 0, 0, 0);
}

// AMDHWMemory::adjustVRAMAddress(mc): MC address -> GPU physical (Linux amdgpu_gmc_vram_mc2pa), used
// for page-table bases/PDEs. hwmem +0x50 = FB MC base, +0x58 = FB physical offset, +0x40 = VRAM size,
// +0x1b8 = segment count, +0x60 = base - offset. The original range-checks against +0x60 instead of
// the MC base, which only coincides on dGPUs (offset 0); with the UMA carve-out MC addresses fall
// outside and pass through unconverted (VMID page-table base 0xF4_xxxxxxxx).
uint64_t wrapAdjustVRAMAddress(uint64_t hwmem, uint64_t mc, uint64_t, uint64_t, uint64_t, uint64_t) {
	uint64_t fb = q(hwmem, 0x50), phys = q(hwmem, 0x58);
	uint64_t size = q(hwmem, 0x40) * *reinterpret_cast<uint32_t *>(hwmem + 0x1b8);
	uint64_t r = mc >= fb && mc < fb + size ? mc - fb + phys : mc;
	static uint32_t logged;
	if (logged < 6 && r != mc) {
		logged++;
		LOG("adjustVRAMAddress(%llx) -> %llx", mc, r);
	}
	return r;
}

void route(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size,
	KernelPatcher::RouteRequest *reqs, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (!patcher.routeMultiple(index, &reqs[i], 1, address, size))
			LOG("failed to route %s (err %d)", reqs[i].symbol, patcher.getError());
		patcher.clearError();
	}
}

}  // namespace

void probeRouteFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateMemorySizeEv", wrapPopulateMemorySize, orgPopulateMemorySize},
	};
	route(patcher, index, address, size, reqs, arrsize(reqs));
}

void probeRouteAccel(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	gAccelBase = address;
	gKeepScanoutDcc = checkKernelArgument("-rmbscandcc");
	gGetHWChannelEngineRing = patcher.solveSymbol(index,
		"__ZN26AMDRadeonX6000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", address, size);
	patcher.clearError();
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN26AMDRadeonX6000_AMDHWMemory4initEP30AMDRadeonX6000_IAMDHWInterface", wrapHWMemInit, orgHWMemInit},
		{"__ZN26AMDRadeonX6000_AMDHWMemory12initVRAMInfoEv", wrapInitVRAMInfo, orgInitVRAMInfo},
		{"__ZN26AMDRadeonX6000_AMDHWMemory17enableAllocationsEv", wrapEnableAllocations, orgEnableAllocations},
		{"__ZN26AMDRadeonX6000_AMDHardware21setReservedVRAMOffsetEyy", wrapSetReservedVRAMOffset, orgSetReservedVRAMOffset},
		{"__ZN26AMDRadeonX6000_AMDHardware18updateHWInfoClocksEv", wrapUpdateHWInfoClocks, orgUpdateHWInfoClocks},
		{"__ZNK36AMDRadeonX6000_AMDAccelResourceAddr221shouldAllocScanoutDccEjjjj", wrapShouldAllocScanoutDcc, orgShouldAllocScanoutDcc},
		{"__ZN37AMDRadeonX6000_AMDGraphicsAccelerator9powerUpHWEv", wrap_AccelPowerUpHW, org_AccelPowerUpHW},
		{"__ZN32AMDRadeonX6000_AMDNavi23Hardware7powerUpEv", wrap_Navi23HwPowerUp, org_Navi23HwPowerUp},
		{"__ZN26AMDRadeonX6000_AMDHWMemory15setVirtualSpaceEP18IOMemoryDescriptor", wrap_HWMemSetVirtualSpace, org_HWMemSetVirtualSpace},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine7doStartEb", wrap_PM4DoStart, org_PM4DoStart},
		{"__ZN30AMDRadeonX6000_AMDKIQHWChannel14submitKIQFrameEv", wrapSubmitKIQFrame, orgSubmitKIQFrame},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine14initComputeMQDE18_eAMD_HW_RING_TYPE", wrap_PM4InitComputeMQD, org_PM4InitComputeMQD},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine15initGraphicsMQDEv", wrap_PM4InitGraphicsMQD, org_PM4InitGraphicsMQD},
		{"__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel8startKIQEyyPK18AMD_PM4_QUEUE_SPECPj", wrapStartKIQ},
		{"__ZN35AMDRadeonX6000_AMDGFX10KIQHWChannel21submitMapQueuesPacketE18_eAMD_HW_RING_TYPEjyPK18AMD_PM4_QUEUE_SPECy", wrap_KIQMapQueues, org_KIQMapQueues},
		{"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine18waitForActiveValueE18_eAMD_HW_RING_TYPEbb", wrap_PM4WaitForActive, org_PM4WaitForActive},
		{"__ZN26AMDRadeonX6000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress},
		{"__ZN26AMDRadeonX6000_AMDHardware16powerUpHWEnginesEv", wrapPowerUpHWEngines},
		{"__ZN28AMDRadeonX6000_AMDRTHardware12getHWChannelE18_eAMD_CHANNEL_TYPE11SS_PRIORITYj", wrapRTGetHWChannel, orgRTGetHWChannel},
		{"__ZN26AMDRadeonX6000_AMDHardware14startHWEnginesEv", wrapStartHWEngines},
		{"__ZN33AMDRadeonX6000_AMDGFX10SDMAEngine5resetE18_eAMD_HW_RING_TYPEPj", wrapSdmaReset, orgSdmaReset},
		{"__ZN26AMDRadeonX6000_AMDHardware13resetHardwareEjPj", wrapResetHardware, orgResetHardware},
		{"__ZN31AMDRadeonX6000_AMDAccelResource15pageOnOffBufferEP13_UBM_SURFINFOS1_P16IOAccelMemoryMapS3_jP10_UBM_RECTLS5_", wrapPageOnOff, orgPageOnOff},
		{"__ZN35AMDRadeonX6000_AMDAtomicBlitManager9doStretchEP16_UBM_STRETCHINFOPK11ABM_OPTIONS", wrapUbmOp<&orgDoStretch, 'S'>, orgDoStretch},
		{"__ZN35AMDRadeonX6000_AMDAtomicBlitManager7doClearEP14_UBM_CLEARINFOPK11ABM_OPTIONS", wrapUbmOp<&orgDoClear, 'C'>, orgDoClear},
		{"__ZN35AMDRadeonX6000_AMDAtomicBlitManager8doExpandEP15_UBM_EXPANDINFOPK11ABM_OPTIONS", wrapUbmOp<&orgDoExpand, 'E'>, orgDoExpand},
	};
	route(patcher, index, address, size, reqs, arrsize(reqs));
	LOG("accel probes installed");
}
