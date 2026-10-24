//
//  GC golden register settings for Rembrandt (GC 10.3.3), from Linux gfx_v10_0.c.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>

namespace {

// Linux's golden_settings_gc_10_3_3 (notably GL2C/GL2A_ADDR_MATCH_MASK: Rembrandt's GL2 channel
// hashing differs from Navi23). macOS never programs GC golden settings on this path (HWLibs'
// gc_hw_init doesn't run; the accel kext has no golden step), so the registers keep reset values.
// Called from smu_hw_init once GFXOFF is disallowed (GC writes are dropped while GFX is off).
// GL2C_CTRL3 and TA_CNTL_AUX are left out (offsets not in the gc_10_3_0 headers).
struct Golden { uint32_t reg, mask, value; };
constexpr uint32_t GcSeg0 = 0x1260, GcSeg1 = 0xA000;
const Golden kGoldenGc1033[] {
	{GcSeg1 + 0x507c, 0xff7f0fff, 0x78000100},  // CGTT_SPI_CS_CLK_CTRL
	{GcSeg1 + 0x2d90, 0x000000ff, 0x000000e4},  // CH_PIPE_STEER
	{GcSeg0 + 0x1f53, 0x0007ffff, 0x0000c200},  // CPF_GCR_CNTL
	{GcSeg0 + 0x13ae, 0xffffffff, 0x00000280},  // DB_DEBUG3
	{GcSeg0 + 0x13af, 0xffffffff, 0x00800000},  // DB_DEBUG4
	{GcSeg0 + 0x13de, 0x0c1807ff, 0x00000242},  // GB_ADDR_CONFIG
	{GcSeg0 + 0x1580, 0x1ff1ffff, 0x00000500},  // GCR_GENERAL_CNTL
	{GcSeg1 + 0x2d10, 0x000000ff, 0x000000e4},  // GL1_PIPE_STEER
	{GcSeg1 + 0x2e25, 0x77777777, 0x32103210},  // GL2_PIPE_STEER_0
	{GcSeg1 + 0x2e26, 0x77777777, 0x32103210},  // GL2_PIPE_STEER_1
	{GcSeg1 + 0x2e21, 0xffffffff, 0xfffffff3},  // GL2A_ADDR_MATCH_MASK
	{GcSeg1 + 0x2e03, 0xffffffff, 0xfffffff3},  // GL2C_ADDR_MATCH_MASK
	{GcSeg1 + 0x2e08, 0xff8fff0f, 0x580f1008},  // GL2C_CM_CTRL1
	{GcSeg0 + 0x10a2, 0x000001ff, 0x00000020},  // LDS_CONFIG
	{GcSeg0 + 0x1025, 0xf17fffff, 0x01200007},  // PA_CL_ENHANCE
	{GcSeg0 + 0x1070, 0xffffffff, 0x00000800},  // PA_SC_BINNER_TIMEOUT_COUNTER
	{GcSeg0 + 0x107c, 0xffffffbf, 0x00000820},  // PA_SC_ENHANCE_2
	{GcSeg0 + 0x1588, 0xffffffff, 0x00100000},  // UTCL1_CTRL
};

}  // namespace

void applyGcGolden() {
	LOG("golden: GL2C_ADDR_MATCH_MASK %x UTCL1_CTRL %x GRBM_STATUS %x", mmioRead(GcSeg1 + 0x2e03), mmioRead(GcSeg0 + 0x1588),
		mmioRead(GcSeg0 + 0xda4));
	for (auto &g : kGoldenGc1033) {
		uint32_t old = mmioRead(g.reg);
		uint32_t val = g.mask == 0xffffffff ? g.value : (old & ~g.mask) | (g.value & g.mask);
		if (val != old) {
			mmioWrite(g.reg, val);
			if (rmbVerbose())
				LOG("golden: %x %x -> %x (reads back %x)", g.reg, old, val, mmioRead(g.reg));
		}
	}
	syncSdmaAddrConfig("golden");
}

// SDMA has its own copies of GB_ADDR_CONFIG (SDMA0_GB_ADDR_CONFIG/_READ), which stay at the reset
// value 0x444 (16 pipes/16 packers) here; GC uses 0x242. SDMA tiled copies (Metal blits between
// buffers and textures, GL texture uploads) then swizzle differently from the shaders → scrambled
// icons/images. Both must be changed together (writing only one hung SDMA in a live test).
// -rmbsdmacfg keeps the reset value.
void syncSdmaAddrConfig(const char *when) {
	static int keep = -1;
	if (keep < 0)
		keep = checkKernelArgument("-rmbsdmacfg");
	if (keep)
		return;
	uint32_t gb = mmioRead(GcSeg0 + 0x13de), cfg = mmioRead(GcSeg0 + 0x1e), rd = mmioRead(GcSeg0 + 0x1f);
	if (cfg == gb && rd == gb)
		return;
	mmioWrite(GcSeg0 + 0x1e, gb);
	mmioWrite(GcSeg0 + 0x1f, gb);
	LOG("sdma-addr: %s: SDMA0_GB_ADDR_CONFIG %x/%x -> %x (reads back %x/%x)", when, cfg, rd, gb, mmioRead(GcSeg0 + 0x1e),
		mmioRead(GcSeg0 + 0x1f));
}

// Something after HWLibs init (accel start) rewrites GL2C/GL2A_ADDR_MATCH_MASK with Navi23's value.
// It happens ~8 ms after smu_hw_init; poll every 1 ms for 3 s, log it and put Rembrandt's values back.
static void guard(void *, wait_result_t) {
	uint64_t t0;
	clock_get_uptime(&t0);
	uint32_t hits = 0;
	for (uint32_t i = 0; i < 3000; i++) {
		IOSleep(1);
		uint32_t m = mmioRead(GcSeg1 + 0x2e03);
		if (m == 0xfffffff3)
			continue;
		uint64_t now, ns;
		clock_get_uptime(&now);
		absolutetime_to_nanoseconds(now - t0, &ns);
		if (hits++ < 10)
			LOG("golden guard: +%llu ms GL2C_ADDR_MATCH_MASK %x GL2A %x UTCL1_CTRL %x", ns / 1000000, m,
				mmioRead(GcSeg1 + 0x2e21), mmioRead(GcSeg0 + 0x1588));
		applyGcGolden();
	}
	LOG("golden guard: done, %u rewrites", hits);
}

void startGoldenGuard() {
	thread_t t;
	if (kernel_thread_start(guard, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
}
