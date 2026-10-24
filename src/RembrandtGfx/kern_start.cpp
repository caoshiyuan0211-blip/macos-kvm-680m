//
//  RembrandtGfx: makes Apple's Navi 23 driver stack run on the Radeon 680M (Rembrandt APU).
//
//  Stage 1 (this file): tracing only. Hooks AMDRadeonX6000HWLibs' TTL init entry points and
//  its event logger and prints what they return, to find where TTL init fails on the APU.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>

#include "rmb.hpp"

static const char *pathHWLibs[] {"/System/Library/Extensions/AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs"};
static const char *pathFB[] {"/System/Library/Extensions/AMDRadeonX6000Framebuffer.kext/Contents/MacOS/AMDRadeonX6000Framebuffer"};
static const char *pathAccel[] {"/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000"};
static const char *pathHWServices[] {"/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/MacOS/AMDRadeonX6000HWServices"};
static const char *pathBacklight[] {"/System/Library/Extensions/AppleBacklight.kext/Contents/MacOS/AppleBacklight"};
static KernelPatcher::KextInfo kexts[] {
	{"com.apple.kext.AMDRadeonX6000HWLibs", pathHWLibs, arrsize(pathHWLibs), {true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000Framebuffer", pathFB, arrsize(pathFB), {true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000", pathAccel, arrsize(pathAccel), {true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.kext.AMDRadeonX6000HWServices", pathHWServices, arrsize(pathHWServices), {true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.driver.AppleBacklight", pathBacklight, arrsize(pathBacklight), {true}, {}, KernelPatcher::KextInfo::Unloaded},
};
constexpr size_t BacklightIdx = 4;
static auto &kextHWLibs = kexts[0];

// Only hook functions whose first instructions are position-independent: Lilu copies them into
// the trampoline, so a RIP-relative lea or short jump there breaks the original (seen with
// _DevIsSupportedDevice, whose prologue loads _DeviceCapabilityTbl RIP-relative).
// Check with scripts/hook-safe.py.
#define TRACE_HOOK(name)                                                                     \
	static mach_vm_address_t org_##name;                                                     \
	static uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx, %llx)", a, b);                                              \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                       \
		LOG("<< " #name " = %llx", r);                                                       \
		return r;                                                                            \
	}

TRACE_HOOK(ttl_sw_init)
TRACE_HOOK(ttl_hw_init)
TRACE_HOOK(IpiSwInit)
TRACE_HOOK(IpiHwInit)
TRACE_HOOK(TlsSwInit)
TRACE_HOOK(TlsHwInit)
TRACE_HOOK(psp_sw_init)
TRACE_HOOK(PP_Initialize_internal)
TRACE_HOOK(PHM_Initialize)
TRACE_HOOK(PEM_Initialize)
TRACE_HOOK(PSM_Initialize)
TRACE_HOOK(PECI_Initialize)
TRACE_HOOK(PhwNavi21_Initialize)
TRACE_HOOK(PP_Tables_Initialize)
TRACE_HOOK(PP_ThermalController_Initialize)
TRACE_HOOK(PHM_RegAccessInitialize)
TRACE_HOOK(PP_Navi21_Thermal_Initialize)
TRACE_HOOK(psp_allocate_private_mem)
TRACE_HOOK(psp_ring_init)
TRACE_HOOK(psp_cmd_km_init)
TRACE_HOOK(psp_dtm_init)
TRACE_HOOK(psp_np_fw_init)
TRACE_HOOK(psp_cmd_km_reset)
TRACE_HOOK(psp_tmr_load)
TRACE_HOOK(gc_sw_init)
TRACE_HOOK(gc_get_config)
TRACE_HOOK(gc_set_fw_entry_info)
TRACE_HOOK(gc_init_fcn_ptr_list)
TRACE_HOOK(gc_validate_hw_config_tbl_version)
TRACE_HOOK(psp_hw_init)
TRACE_HOOK(psp_init_pfn_ptr)
TRACE_HOOK(ttlDevCreateInstance)
TRACE_HOOK(ttl_cgs_initialize)
TRACE_HOOK(validateCosInfo)
TRACE_HOOK(ttl_initialize)
TRACE_HOOK(TtlInitialize)
TRACE_HOOK(TtlServicesInitialize)
TRACE_HOOK(TtlInitializeCos)
TRACE_HOOK(TtlInitializeGmm)
TRACE_HOOK(TtlInitializeIcm)
TRACE_HOOK(TtlInitializePrm)

// bcs_log(ctx, eventId, phase, a, b): AMD's SWIP event log. Bit 31 of eventId marks an error.
static mach_vm_address_t org_bcs_log;
static uint32_t bcsLogCount;
static uint64_t wrap_bcs_log(uint64_t ctx, uint64_t event, uint64_t phase, uint64_t a, uint64_t b, uint64_t f) {
	if (bcsLogCount < 400 || (event & 0x80000000)) {
		bcsLogCount++;
		LOG("bcs_log event=%08llx phase=%llu a=%llx b=%llx", event & 0xFFFFFFFF, phase & 0xFFFFFFFF, a, b);
	}
	return reinterpret_cast<Fn6>(org_bcs_log)(ctx, event, phase, a, b, f);
}

// --- Framebuffer ---
// Once the framebuffer initializes, WindowServer opens it right away, but the GPU wrangler only
// sets LoadAccelerator (which the accelerator personality matches on) later. powerUp() then
// sends a PPLIB message to the not-yet-present accelerator and panics. Wait for it instead.
static mach_vm_address_t orgMessageAccelerator, orgDoesAcceleratorExist;
static uint64_t wrapMessageAccelerator(void *ctrl, uint64_t type, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	auto exists = reinterpret_cast<bool (*)(void *)>(orgDoesAcceleratorExist);
	if (!exists(ctrl)) {
		LOG("messageAccelerator(%llu): accelerator not attached yet, waiting", type);
		for (int i = 0; i < 200 && !exists(ctrl); i++)
			IOSleep(100);
		LOG("messageAccelerator(%llu): accelerator %s", type, exists(ctrl) ? "attached" : "still missing after 20 s");
	}
	auto r = reinterpret_cast<Fn6>(orgMessageAccelerator)(reinterpret_cast<uint64_t>(ctrl), type, a, b, c, d);
	if (r)
		LOG("messageAccelerator(%llu) = %llx", type, r);
	return r;
}

TRACE_HOOK(CtrlStart)
TRACE_HOOK(CtrlInitInternals)
TRACE_HOOK(CtrlPowerUp)
TRACE_HOOK(FbEnableController)
TRACE_HOOK(CreateCoreServices)
TRACE_HOOK(WranglerStart)
static mach_vm_address_t orgVendorAttr;
static uint64_t wrapVendorAttr(uint64_t self, uint64_t attr, uint64_t in, uint64_t inSize, uint64_t out, uint64_t args) {
	LOG(">> wrangler vendor_doDeviceAttribute(attr %llu)", attr & 0xFFFFFFFF);
	auto r = reinterpret_cast<Fn6>(orgVendorAttr)(self, attr, in, inSize, out, args);
	LOG("<< wrangler vendor_doDeviceAttribute(attr %llu) = %llx", attr & 0xFFFFFFFF, r);
	return r;
}

// -rmbheadless: publish no AMD framebuffers. The display engine (DCN 3.1) isn't supported yet, so
// WindowServer stays on the emulated display and the 680M is used as a display-less (Metal) GPU.
static bool gHeadless;
static uint64_t wrapFbStart(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("headless: AmdRadeonFramebuffer start declined");
	return 0;
}

// AmdAsicInfoNavi2::populateDeviceInfo takes device id (bits 15:0) and revision (27:24) from
// mmRCC_DEV0_EPF0_STRAP0 = 0xd31, an NBIO 2.3 (Navi) offset; on Rembrandt (NBIO 7.2) that is a
// different register (reads "Device 0xff00, revNo 15"), so CAIL's caps lookup fails and the Metal
// driver sees an unknown prototype (it then encodes zero-sized dispatches). Report Navi 23 instead.
// Fields: +0x60 family, +0x64 device, +0x68 revNo, +0x6c emuRevNo (= revNo + per-ASIC base).
static mach_vm_address_t orgPopulateDeviceInfo;
static uint64_t wrapPopulateDeviceInfo(uint64_t that, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgPopulateDeviceInfo)(that, b, c, d, e, f);
	auto info = reinterpret_cast<uint8_t *>(that);
	auto &dev = *reinterpret_cast<uint32_t *>(info + 0x64), &rev = *reinterpret_cast<uint32_t *>(info + 0x68),
		 &emu = *reinterpret_cast<uint32_t *>(info + 0x6c);
	LOG("asicinfo: strap says device %x rev %u emu %u; reporting device 73ff rev 0 emu %u", dev, rev, emu, emu - rev);
	emu -= rev;
	dev = 0x73ff;
	rev = 0;
	return r;
}

// AmdAsicInfoNavi2::populateXGmiConfig reads MC_VM_XGMI_LFB_CNTL/SIZE, MC_VM_FB_LOCATION_BASE/TOP
// and MC_VM_FB_OFFSET at MMHUB 2.0 offsets (0x1a867.. ); Rembrandt's MMHUB 2.3 has other offsets, so
// DAL got FB base 0x100000000 and built bad DMUB data windows. Use GC's copies (GCMC_VM_FB_*,
// same values) and one XGMI node. Fields: +0x138 node id, +0x13c nodes, +0x140 base, +0x144 top,
// +0x148 node segment size, +0x14c FB offset (all in 16 MB units).
static mach_vm_address_t orgPopulateXGmiConfig;
static uint64_t wrapPopulateXGmiConfig(uint64_t that, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgPopulateXGmiConfig)(that, b, c, d, e, f);
	auto u = [&](uint32_t off) -> uint32_t & { return *reinterpret_cast<uint32_t *>(that + off); };
	constexpr uint32_t GC0 = 0x1260;
	uint32_t base = mmioRead(GC0 + 0x16fc) & 0xFFFFFF, top = mmioRead(GC0 + 0x16fd) & 0xFFFFFF,
			 off = mmioRead(GC0 + 0x16e7) & 0xFFFFFF;
	LOG("asicinfo: MMHUB2.0 FB regs gave base %x top %x offset %x nodes %u; using GCMC base %x top %x offset %x",
		u(0x140), u(0x144), u(0x14c), u(0x13c), base, top, off);
	u(0x138) = 0;
	u(0x13c) = 1;
	u(0x140) = base;
	u(0x144) = top;
	u(0x14c) = off;
	return r;
}

// HDMI 2.0 TMDS above 340 MHz (scrambling) gives the LG TV no signal (4K60 RGB, 594 MHz), while 4K60
// YCbCr 4:2:0 (297 MHz) works. Cap the link encoders' max_hdmi_pixel_clock (encoder_feature_support
// {flags 0xf, deep color 12 bpc, 600000 kHz, ...} in __TEXT,__const, one per DCN generation) at
// 340000 so DAL rejects >340 MHz timings and picks 4:2:0 / lower depth. `-rmbhdmi6g` = keep 600 MHz.
static void capHdmiPixelClock(mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbhdmi6g"))
		return;
	static const uint8_t find[] {0x0f, 0, 0, 0, 0x04, 0, 0, 0, 0xc0, 0x27, 0x09, 0x00, 0x01};
	constexpr uint32_t capKhz = 340000;
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t n = 0;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("hdmi: kernel writing failed");
		return;
	}
	for (size_t i = 0; i + sizeof(find) <= size; i++) {
		if (memcmp(p + i, find, sizeof(find)))
			continue;
		memcpy(p + i + 8, &capKhz, sizeof(capKhz));
		n++;
	}
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	LOG("hdmi: max_hdmi_pixel_clock 600000 -> %u in %zu encoder feature tables", capKhz, n);
}

// Navi23 DAL (res_cap_dcn302 in __TEXT,__const) has 5 pipes; Rembrandt DCN 3.1.2 has 4. A wide
// scaled mode (LG 4K "looks like" 2xxx: 6400 px source) made DAL pipe-split onto pipe 4 → MPCC1
// BOT_SEL=4 = nonexistent MPCC, both MPCCs stuck BUSY, panel black + LG green, WindowServer hung
// (auto-1020-1104; also the mpc2_assert_idle_mpcc reg 908d = MPCC4 timeouts). Patch
// num_timing_generator/num_opp/num_video_plane 5 -> 4 (pipe_count and MPC count follow). `-rmbpipes5` = keep.
static void capPipes(mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbpipes5"))
		return;
	static const uint32_t find[] {5, 5, 5, 5, 5, 0, 1, 5, 16, 5};
	static const uint32_t pipes = 4;
	auto p = reinterpret_cast<uint8_t *>(address);
	size_t n = 0;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("pipes: kernel writing failed");
		return;
	}
	for (size_t i = 0; i + sizeof(find) <= size; i += 4) {
		if (memcmp(p + i, find, sizeof(find)))
			continue;
		for (size_t f = 0; f < 3; f++)
			memcpy(p + i + f * 4, &pipes, sizeof(pipes));
		n++;
	}
	// dcn302_resource_construct hardcodes pipe_count (hubp/dpp loops) and mpcc_count:
	// movabs $0xffffffff00000005, %rcx; mov %rcx, 0x2a8(%r13); movl $5, 0x378(%r13).
	// Left at 5 they index timing_generators[4] (NULL) in init_pipes and MPCC4.
	static const uint8_t code[] {0x48, 0xb9, 0x05, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
		0x49, 0x89, 0x8d, 0xa8, 0x02, 0x00, 0x00, 0x41, 0xc7, 0x85, 0x78, 0x03, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00};
	size_t c = 0;
	for (size_t i = 0; i + sizeof(code) <= size; i++) {
		if (memcmp(p + i, code, sizeof(code)))
			continue;
		p[i + 2] = pipes;
		p[i + 24] = pipes;
		c++;
	}
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	LOG("pipes: res_cap_dcn302 timing generators/opp/planes 5 -> %u in %zu tables, pipe/mpcc count in %zu places", pipes, n, c);
}

// System Settings > Displays: every panel size but the native one did nothing when clicked
// (auto-1024-1257 DisplaysExt "Requested mode (13) is not safe. Displaying the sheet." and no sheet
// ever showed). Mode safety = IODisplayWrangler::getFlagsForDisplayMode -> fb connectFlags into
// flags = 0, and AmdRadeonFramebuffer::connectFlags is an empty stub (push rbp; mov rsp,rbp;
// xor eax,eax; pop rbp; ret) -> no Valid/Safe bit for any mode. Report Valid|Safe like IODisplay's
// default for drivers without connectFlags. With that the 16:10 sizes switch, but IOGraphicsLib
// still strips Safe from the AMD 4:3/16:9 list modes (1920x1080, 1600x1200, 1280x1024 ...) and
// one click on such a size leaves the pane waiting on a sheet that never shows (auto-1024-1355:
// every later click ignored). extGetInformationForDisplayMode ORs our flags in, so add NeverShow
// to every mode that isn't 16:10 like the panel: the pane offers only sizes that switch, as on a
// real built-in display. `-rmbnosafemodes` = keep the stub, `-rmbshowallmodes` = no NeverShow.
static uint8_t *gConnectFlags;
static mach_vm_address_t gFbModeInfo;
static bool gShowAllModes;

void safeModesLog() {
	if (gConnectFlags)
		LOG("safemodes: connectFlags now %02x %02x %02x %02x %02x %02x", gConnectFlags[0], gConnectFlags[1],
		    gConnectFlags[2], gConnectFlags[3], gConnectFlags[4], gConnectFlags[5]);
	else
		LOG("safemodes: connectFlags never resolved");
}

static IOReturn wrapConnectFlags(void *fb, IOIndex connectIndex, IODisplayModeID mode, UInt32 *flags) {
	*flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
	if (gShowAllModes || !gFbModeInfo)
		return kIOReturnSuccess;
	IODisplayModeInformation info {};
	auto r = reinterpret_cast<IOReturn (*)(void *, IODisplayModeID, IODisplayModeInformation *)>(gFbModeInfo)(fb, mode, &info);
	bool hide = r == kIOReturnSuccess && info.nominalWidth && info.nominalHeight &&
	            info.nominalWidth * 10 != info.nominalHeight * 16;
	if (hide)
		*flags |= kDisplayModeNeverShowFlag;
	static IODisplayModeID logged[64];
	static unsigned nlogged;
	bool seen = false;
	for (unsigned i = 0; i < nlogged && !seen; i++)
		seen = logged[i] == mode;
	if (!seen && nlogged < arrsize(logged)) {
		logged[nlogged++] = mode;
		LOG("safemodes: mode %x %ux%u (info %x) -> flags %x%s", mode, info.nominalWidth, info.nominalHeight, r,
		    *flags, hide ? " (hidden, not 16:10)" : "");
	}
	return kIOReturnSuccess;
}

static void safeModes(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnosafemodes"))
		return;
	gShowAllModes = checkKernelArgument("-rmbshowallmodes");
	auto fn = patcher.solveSymbol(index, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12connectFlagsEiiPj", address, size);
	patcher.clearError();
	gFbModeInfo = patcher.solveSymbol(index,
		"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer28getInformationForDisplayModeEiP24IODisplayModeInformation", address, size);
	patcher.clearError();
	static const uint8_t stub[] {0x55, 0x48, 0x89, 0xe5, 0x31, 0xc0, 0x5d, 0xc3};
	auto p = reinterpret_cast<uint8_t *>(fn);
	gConnectFlags = p;
	if (!p || memcmp(p, stub, sizeof(stub))) {
		LOG("safemodes: connectFlags %s, not patched", p ? "is not the empty stub" : "not found");
		return;
	}
	// The stub is 8 bytes: a short (5-byte) route fits, no original needed.
	KernelPatcher::RouteRequest req {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12connectFlagsEiiPj", wrapConnectFlags};
	if (!patcher.routeMultiple(index, &req, 1, address, size)) {
		LOG("safemodes: route failed (err %d)", patcher.getError());
		patcher.clearError();
		return;
	}
	LOG("safemodes: AmdRadeonFramebuffer::connectFlags -> Valid|Safe, non-16:10 modes %s (modeinfo %s)",
	    gShowAllModes ? "shown" : "NeverShow", gFbModeInfo ? "found" : "NOT found");
}

static void processFramebuffer(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	capHdmiPixelClock(address, size);
	capPipes(address, size);
	safeModes(patcher, index, address, size);
	gHeadless = checkKernelArgument("-rmbheadless");
	if (gHeadless) {
		KernelPatcher::RouteRequest req {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer5startEP9IOService", wrapFbStart};
		if (!patcher.routeMultiple(index, &req, 1, address, size))
			LOG("headless: failed to route framebuffer start (err %d)", patcher.getError());
		else
			LOG("headless: framebuffers disabled");
		patcher.clearError();
	}
	KernelPatcher::RouteRequest traces[] {
		{"__ZN34AMDRadeonX6000_AmdRadeonController5startEP9IOService", wrap_CtrlStart, org_CtrlStart},
		{"__ZN34AMDRadeonX6000_AmdRadeonController19initializeInternalsEP9IOService", wrap_CtrlInitInternals, org_CtrlInitInternals},
		{"__ZN34AMDRadeonX6000_AmdRadeonController7powerUpEv", wrap_CtrlPowerUp, org_CtrlPowerUp},
		{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer16enableControllerEv", wrap_FbEnableController, org_FbEnableController},
		{"__ZN40AMDRadeonX6000_AmdRadeonControllerNavi2328createControllerCoreServicesEv", wrap_CreateCoreServices, org_CreateCoreServices},
		{"__ZN29AMDRadeonX6000_AmdGpuWrangler5startEP9IOService", wrap_WranglerStart, org_WranglerStart},
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateDeviceInfoEv", wrapPopulateDeviceInfo, orgPopulateDeviceInfo},
		{"__ZN31AMDRadeonX6000_AmdAsicInfoNavi218populateXGmiConfigEv", wrapPopulateXGmiConfig, orgPopulateXGmiConfig},
		{"__ZN29AMDRadeonX6000_AmdGpuWrangler24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments", wrapVendorAttr, orgVendorAttr},
	};
	displayRoute(patcher, index, address, size);
	for (auto &r : traces) {
		if (!patcher.routeMultiple(index, &r, 1, address, size))
			LOG("failed to route %s (err %d)", r.symbol, patcher.getError());
		patcher.clearError();
	}
	orgDoesAcceleratorExist = patcher.solveSymbol(index, "__ZNK34AMDRadeonX6000_AmdRadeonController20doesAcceleratorExistEv", address, size);
	patcher.clearError();
	KernelPatcher::RouteRequest req {"__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_",
		wrapMessageAccelerator, orgMessageAccelerator};
	if (!orgDoesAcceleratorExist || !patcher.routeMultiple(index, &req, 1, address, size))
		LOG("failed to hook messageAccelerator (err %d)", patcher.getError());
	else
		LOG("framebuffer hooks installed");
	patcher.clearError();
}

// The 680M sits on the SoC's internal fabric, not behind a PCIe link of its own, so the
// dGPU-only link checks in pcie_ip_sw_init (check_pcie_link_status / check_root_pcie_link_speed)
// fail and abort bgm_create (event 0xc00c020b). There is no link to manage: report success.
static uint64_t wrap_pcie_ip_sw_init(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("pcie_ip_sw_init: skipped (APU has no discrete PCIe link)");
	return 0;
}

// DeviceCapabilityTbl is keyed by (device id, internal revision, external/PCI revision). All
// Navi 23 (0x73FF) rows have internal revision 0; Rembrandt reports its own, so no row matches.
// Look up the Navi 23 row with internal revision 0 and the real PCI revision.
static mach_vm_address_t org_ttlSetDeviceCapabilityEntry;
static uint64_t wrap_ttlSetDeviceCapabilityEntry(uint64_t ctx, uint64_t dev, uint64_t intRev, uint64_t extRev, uint64_t e, uint64_t f) {
	LOG("ttlSetDeviceCapabilityEntry(dev %llx, internal rev %llx, external rev %llx) -> using internal rev 0",
		dev & 0xFFFFFFFF, intRev & 0xFFFFFFFF, extRev & 0xFFFFFFFF);
	auto r = reinterpret_cast<Fn6>(org_ttlSetDeviceCapabilityEntry)(ctx, dev, 0, extRev, e, f);
	LOG("ttlSetDeviceCapabilityEntry = %llx", r & 0xFF);
	return r;
}

// IP discovery entry (0x260 bytes): +0x0 hw id, +0x8 major, +0xC minor, +0x10 revision.
static mach_vm_address_t org_get_hw_revision;
static uint64_t wrap_get_hw_revision(uint64_t maj, uint64_t min, uint64_t rev, uint64_t d, uint64_t e, uint64_t f) {
	LOG("get_hw_revision(%llu.%llu.%llu)", maj & 0xFFFF, min & 0xFFFF, rev & 0xFFFF);
	return reinterpret_cast<Fn6>(org_get_hw_revision)(maj, min, rev, d, e, f);
}

// HWLibs only implements PSP 3.1 and 11.0. Rembrandt's MP0 is 13.x; present it as Navi 23's
// 11.0.11 so the PSP 11 code paths get selected (the differing parts get routed separately).
static mach_vm_address_t org_psp_asic_type_init;
static uint64_t wrap_psp_asic_type_init(uint64_t psp, uint64_t info, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto ip = reinterpret_cast<uint32_t *>(info);
	LOG("psp_asic_type_init: MP0 %u.%u.%u", ip[2], ip[3], ip[4]);
	uint8_t copy[0x260];
	if (ip[2] == 13) {
		memcpy(copy, ip, sizeof(copy));
		auto cp = reinterpret_cast<uint32_t *>(copy);
		cp[2] = 11, cp[3] = 0, cp[4] = 11;
		info = reinterpret_cast<uint64_t>(copy);
		LOG("psp_asic_type_init: presenting MP0 as 11.0.11");
	}
	auto r = reinterpret_cast<Fn6>(org_psp_asic_type_init)(psp, info, c, d, e, f);
	LOG("psp_asic_type_init = %llx, psp type %u", r & 0xFFFFFFFF, *reinterpret_cast<uint32_t *>(psp + 0x7d94));
	return r;
}

// HWLibs has GC tables for 10.3.0 (Navi 21) and 10.3.4 (Navi 23) only. Rembrandt's GC is 10.3.3,
// the same gfx10.3 design (Linux drives both with gfx_v10_0); present it as 10.3.4. Firmware and
// golden settings that differ are substituted separately.
static mach_vm_address_t org_gc_get_hw_version;
static uint64_t wrap_gc_get_hw_version(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto v = reinterpret_cast<Fn6>(org_gc_get_hw_version)(a, b, c, d, e, f) & 0xFFFFFFFF;
	static bool logged;
	if (!logged) {
		logged = true;
		LOG("gc_get_hw_version: GC %llx%s", v, v == 0xa0303 ? ", presenting as 10.3.4" : "");
	}
	return v == 0xa0303 ? 0xa0304 : v;
}

// PSP mailbox dump around KM ring creation (MP0 seg0 0x16000; C2PMSG_n = 0x40 + n).
static void dumpPspMailbox(const char *when) {
	constexpr uint32_t mp0 = 0x16000;
	LOG("psp %s: C2PMSG_35 %08x 64 %08x 69 %08x 70 %08x 71 %08x 81 %08x", when,
		mmioRead(mp0 + 0x63), mmioRead(mp0 + 0x80), mmioRead(mp0 + 0x85), mmioRead(mp0 + 0x86),
		mmioRead(mp0 + 0x87), mmioRead(mp0 + 0x91));
}

// The host's amdgpu (or an earlier VM run) leaves a KM ring / error status behind in the PSP,
// which keeps running across VM restarts. Destroy rings first, as Linux psp_v13_0_ring_stop does.
static void pspDestroyRings() {
	constexpr uint32_t c2pmsg64 = 0x16000 + 0x80;
	mmioWrite(c2pmsg64, 0x00030000);  // GFX_CTRL_CMD_ID_DESTROY_RINGS
	IODelay(20000);
	for (int i = 0; i < 2000 && !(mmioRead(c2pmsg64) & 0x80000000); i++)
		IODelay(1000);
	LOG("psp: destroy rings -> C2PMSG_64 %08x", mmioRead(c2pmsg64));
}

static mach_vm_address_t org_psp_ring_create;
static uint64_t wrap_psp_ring_create(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	dumpPspMailbox("before ring create");
	if (b == 2)
		gartCensus("before KM ring create");
	// Only before the KM ring (type 2), which HWLibs creates first: DESTROY_RINGS tears down every
	// ring, including the KM ring HWLibs created moments ago when it later creates the UM ring (type 1).
	// Every time, not once per boot: the PSP keeps its rings across guest S3 + host S3, and the wake
	// powerUp's KM ring create fails against the stale one.
	if (b == 2)
		pspDestroyRings();
	auto r = reinterpret_cast<Fn6>(org_psp_ring_create)(a, b, c, d, e, f);
	dumpPspMailbox("after ring create");
	LOG("psp_ring_create(type %llu) = %llx", b, r & 0xFFFFFFFF);
	return r;
}

// PowerPlay event chains: same loop as PEM_ExcuteEventChain, but logging each task (as an offset
// from HWLibs' __TEXT; resolve offline with +0xc305000 against build/amd/AMDRadeonX6000HWLibs).
static mach_vm_address_t gHWLibsBase;
using PemTask = uint32_t (*)(void *, void *);
// Tasks that need driver-built power states from a dGPU PowerPlay table, which Rembrandt doesn't
// have (its PMFW manages DPM itself). Treated as succeeded.
static mach_vm_address_t gSkipTasks[1];
// Tasks whose failure is ignored. NotifyHWOfPowerSource (AC/DC to the SMU) fails on some guest S3
// wakes (TTL dispatcher not up yet, e.g. after WindowServer missed the sleep ack) and Apple then
// panics "Failed Power Play Resume"; Rembrandt's PMFW doesn't need it. `-rmbpwrsrc` = fatal again.
static mach_vm_address_t gTolerateTasks[1];
static uint64_t wrap_PEM_ExcuteEventChain(uint64_t ctx, uint64_t chain, uint64_t data, uint64_t, uint64_t, uint64_t) {
	if (!chain || !data)
		return 7;
	auto name = *reinterpret_cast<const char **>(chain + 0x8);
	auto lists = *reinterpret_cast<PemTask ***>(chain + 0x10);
	uint32_t res = 1;
	// Runtime chains (not boot/sleep) preceded both SDMA hangs that took the host down (1024-1238,
	// 1024-1332) and never returned: trace each task so the last line names the stuck one.
	bool trace = name && strcmp(name, "Initialize") && strcmp(name, "Complete Init") && strcmp(name, "Suspend") &&
	             strcmp(name, "Resume");
	if (trace)
		LOG("pem <%s>: start", name);
	for (; lists && *lists && res == 1; lists++) {
		for (auto task = *lists; *task; task++) {
			bool skip = false;
			for (auto t : gSkipTasks)
				skip |= t && t == reinterpret_cast<mach_vm_address_t>(*task);
			uint64_t t0 = 0;
			if (trace) {
				LOG("pem <%s>: task +%llx", name, reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase);
				t0 = mach_absolute_time();
			}
			auto r = skip ? 1 : (*task)(reinterpret_cast<void *>(ctx), reinterpret_cast<void *>(data));
			if (trace) {
				uint64_t ns;
				absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
				if (ns > 50000000)
					LOG("pem <%s>: task +%llx took %llu ms", name, reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase, ns / 1000000);
			}
			if (r != 1)
				LOG("pem <%s>: task +%llx = %u", name ? name : "?", reinterpret_cast<mach_vm_address_t>(*task) - gHWLibsBase, r);
			bool tolerate = false;
			for (auto t : gTolerateTasks)
				tolerate |= t && t == reinterpret_cast<mach_vm_address_t>(*task);
			if (r != 1 && tolerate) {
				LOG("pem <%s>: ignoring failed task", name ? name : "?");
				r = 1;
			}
			if (res == 1)
				res = r;
		}
	}
	LOG("pem <%s> = %u", name ? name : "?", res);
	return res;
}

// The GART/VMID0 page tables live in the UMA carve-out. Their PDEs/PTEs and the page-table base
// register need the carve-out's system physical address: VRAM offset + (GCMC_VM_FB_OFFSET << 24),
// as Linux's amdgpu_gmc_vram_mc2pa does. HWLibs' vm_10_1 does this only when the device caps mark
// the ASIC as UMA (vm ctx +0xc, bits 0x18), and the Navi 23 capability row we use says dGPU.
static mach_vm_address_t org_get_uma_physical_fb_offset;
static uint64_t wrap_get_uma_physical_fb_offset(uint64_t vm, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	*reinterpret_cast<uint8_t *>(vm + 0xc) |= 0x8;
	auto r = reinterpret_cast<Fn6>(org_get_uma_physical_fb_offset)(vm, b, c, d, e, f);
	LOG("vm: UMA physical FB offset %llx", *reinterpret_cast<uint64_t *>(vm + 0x210));
	return r;
}

// Clamps a power state's clocks to the PowerPlay table's limits and divides by them; with no
// pptable on Rembrandt they are all zero (divide error). PMFW owns DPM, so leave states as-is.
static uint64_t wrap_ApplyClocksAdjustRules(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	return 1;  // PP_Result_OK
}

// bgm hw init applies the device-capability golden table (Navi23's: GL2 match masks 0xffffffcf etc.)
// and vm_10_3_hw_init programs more GC registers; after each, put Rembrandt's golden values back.
static mach_vm_address_t org_execute_register_golden_settings, org_gc_10_3_initialize_registers;
static uint64_t wrap_execute_register_golden_settings(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_execute_register_golden_settings)(a, b, c, d, e, f);
	LOG("golden: after execute_register_golden_settings (%llx)", r);
	applyGcGolden();
	return r;
}
static uint64_t wrap_gc_10_3_initialize_registers(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_gc_10_3_initialize_registers)(a, b, c, d, e, f);
	LOG("golden: after gc_10_3_initialize_registers (%llx)", r);
	applyGcGolden();
	return r;
}

static void processHWLibs(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest requests[] {
		{"_ttl_sw_init", wrap_ttl_sw_init, org_ttl_sw_init},
		{"_ttl_hw_init", wrap_ttl_hw_init, org_ttl_hw_init},
		{"_IpiSwInit", wrap_IpiSwInit, org_IpiSwInit},
		{"_IpiHwInit", wrap_IpiHwInit, org_IpiHwInit},
		{"_TlsSwInit", wrap_TlsSwInit, org_TlsSwInit},
		{"_TlsHwInit", wrap_TlsHwInit, org_TlsHwInit},
		{"_psp_sw_init", wrap_psp_sw_init, org_psp_sw_init},
		{"_PP_Initialize_internal", wrap_PP_Initialize_internal, org_PP_Initialize_internal},
		{"_PHM_Initialize", wrap_PHM_Initialize, org_PHM_Initialize},
		{"_PEM_Initialize", wrap_PEM_Initialize, org_PEM_Initialize},
		{"_PSM_Initialize", wrap_PSM_Initialize, org_PSM_Initialize},
		{"_PECI_Initialize", wrap_PECI_Initialize, org_PECI_Initialize},
		{"_PhwNavi21_Initialize", wrap_PhwNavi21_Initialize, org_PhwNavi21_Initialize},
		{"_PP_Tables_Initialize", wrap_PP_Tables_Initialize, org_PP_Tables_Initialize},
		{"_PP_ThermalController_Initialize", wrap_PP_ThermalController_Initialize, org_PP_ThermalController_Initialize},
		{"_PHM_RegAccessInitialize", wrap_PHM_RegAccessInitialize, org_PHM_RegAccessInitialize},
		{"_PP_Navi21_Thermal_Initialize", wrap_PP_Navi21_Thermal_Initialize, org_PP_Navi21_Thermal_Initialize},
		{"_psp_allocate_private_mem", wrap_psp_allocate_private_mem, org_psp_allocate_private_mem},
		{"_psp_ring_init", wrap_psp_ring_init, org_psp_ring_init},
		{"_psp_cmd_km_init", wrap_psp_cmd_km_init, org_psp_cmd_km_init},
		{"_psp_dtm_init", wrap_psp_dtm_init, org_psp_dtm_init},
		{"_psp_np_fw_init", wrap_psp_np_fw_init, org_psp_np_fw_init},
		{"_psp_ring_create", wrap_psp_ring_create, org_psp_ring_create},
		{"_psp_cmd_km_reset", wrap_psp_cmd_km_reset, org_psp_cmd_km_reset},
		{"_psp_tmr_load", wrap_psp_tmr_load, org_psp_tmr_load},
		{"_gc_sw_init", wrap_gc_sw_init, org_gc_sw_init},
		{"_gc_get_hw_version", wrap_gc_get_hw_version, org_gc_get_hw_version},  // after TRACE: GC 10.3.3 -> 10.3.4
		{"_gc_get_config", wrap_gc_get_config, org_gc_get_config},
		{"_execute_register_golden_settings", wrap_execute_register_golden_settings, org_execute_register_golden_settings},
		{"_gc_10_3_initialize_registers", wrap_gc_10_3_initialize_registers, org_gc_10_3_initialize_registers},
		{"_gc_set_fw_entry_info", wrap_gc_set_fw_entry_info, org_gc_set_fw_entry_info},
		{"_gc_init_fcn_ptr_list", wrap_gc_init_fcn_ptr_list, org_gc_init_fcn_ptr_list},
		{"_gc_validate_hw_config_tbl_version", wrap_gc_validate_hw_config_tbl_version, org_gc_validate_hw_config_tbl_version},
		{"_psp_hw_init", wrap_psp_hw_init, org_psp_hw_init},
		{"_psp_init_pfn_ptr", wrap_psp_init_pfn_ptr, org_psp_init_pfn_ptr},
		{"_ttlDevCreateInstance", wrap_ttlDevCreateInstance, org_ttlDevCreateInstance},
		{"_ttl_cgs_initialize", wrap_ttl_cgs_initialize, org_ttl_cgs_initialize},
		{"_validateCosInfo", wrap_validateCosInfo, org_validateCosInfo},
		{"_bcs_log", wrap_bcs_log, org_bcs_log},
		{"_pcie_ip_sw_init", wrap_pcie_ip_sw_init},
		{"_PEM_ExcuteEventChain", wrap_PEM_ExcuteEventChain},
		{"_Phw_CommonDynamicState_ApplyClocksAdjustRules", wrap_ApplyClocksAdjustRules},
		{"_get_hw_revision", wrap_get_hw_revision, org_get_hw_revision},
		{"_psp_asic_type_init", wrap_psp_asic_type_init, org_psp_asic_type_init},
		{"_ttlSetDeviceCapabilityEntry", wrap_ttlSetDeviceCapabilityEntry, org_ttlSetDeviceCapabilityEntry},
		{"_vm_10_1_get_uma_physical_fb_offset", wrap_get_uma_physical_fb_offset, org_get_uma_physical_fb_offset},
		{"_ttl_initialize", wrap_ttl_initialize, org_ttl_initialize},
		{"__ZN14AmdTtlServices10initializeEP30_TtlLibraryInitializationInput", wrap_TtlInitialize, org_TtlInitialize},
		{"__ZN14AmdTtlServices10initializeEv", wrap_TtlServicesInitialize, org_TtlServicesInitialize},
		{"__ZN14AmdTtlServices13initializeCosEv", wrap_TtlInitializeCos, org_TtlInitializeCos},
		{"__ZN14AmdTtlServices13initializeGmmEv", wrap_TtlInitializeGmm, org_TtlInitializeGmm},
		{"__ZN14AmdTtlServices13initializeIcmEv", wrap_TtlInitializeIcm, org_TtlInitializeIcm},
		{"__ZN14AmdTtlServices13initializePrmEv", wrap_TtlInitializePrm, org_TtlInitializePrm},
	};
	for (auto &r : requests) {
		if (!patcher.routeMultiple(index, &r, 1, address, size))
			LOG("failed to route %s (err %d)", r.symbol, patcher.getError());
		patcher.clearError();
	}
	gSkipTasks[0] = patcher.solveSymbol(index, "_PEM_Task_EnableDynamicStateManagement");
	patcher.clearError();
	LOG("pem: skip list %s", gSkipTasks[0] ? "resolved" : "missing");
	if (!checkKernelArgument("-rmbpwrsrc"))
		gTolerateTasks[0] = patcher.solveSymbol(index, "_PEM_Task_NotifyHWOfPowerSource");
	patcher.clearError();
	smuRoute(patcher, index, address, size);
	firmwareRoute(patcher, index, address, size);
	LOG("HWLibs hooks installed");
}

// Lilu only inspects the head of the kmod list when a kext loads, and on this setup it never
// reports the AMD kexts mapped from the System KC. So we route OSKext::start ourselves and scan
// the kmod list for HWLibs before any kext starts; HWLibs starts before AMDRadeonX6000 (which
// runs TTL init), so the hooks are in place in time.
static KernelPatcher *gPatcher;
static kmod_info_t **gKmods;
static bool hooked[arrsize(kexts)];
static mach_vm_address_t orgOSKextStart;

static void scanKmods() {
	if (!gKmods)
		return;
	static bool agdcFound;
	for (auto kmod = *gKmods; kmod; kmod = kmod->next) {
		if (!agdcFound && !strncmp(kmod->name, "com.apple.AppleGraphicsDeviceControl", KMOD_MAX_NAME)) {
			agdcFound = true;
			displaySetAgdcBase(kmod->address);
		}
		for (size_t i = 0; i < 3; i++) {  // 0 = HWLibs, 1 = Framebuffer, 2 = Accelerator
			if (hooked[i] || strncmp(kmod->name, kexts[i].id, KMOD_MAX_NAME))
				continue;
			hooked[i] = true;
			auto size = gPatcher->updateRunningInfo(kexts[i].loadIndex, kmod->address, kmod->size);
			LOG("found %s, size %zx", kexts[i].id, size);
			if (i == 0)
				gHWLibsBase = kmod->address,
				processHWLibs(*gPatcher, kexts[i].loadIndex, kmod->address, size);
			else if (i == 1)
				processFramebuffer(*gPatcher, kexts[i].loadIndex, kmod->address, size);
			else
				probeRouteAccel(*gPatcher, kexts[i].loadIndex, kmod->address, size);
		}
		auto &bl = kexts[BacklightIdx];
		if (!hooked[BacklightIdx] && !strncmp(kmod->name, bl.id, KMOD_MAX_NAME)) {
			hooked[BacklightIdx] = true;
			auto size = gPatcher->updateRunningInfo(bl.loadIndex, kmod->address, kmod->size);
			LOG("found %s, size %zx", bl.id, size);
			displayRouteBacklight(*gPatcher, bl.loadIndex, kmod->address, size);
		}
	}
}

static OSReturn wrapOSKextStart(void *kext, bool startDependencies) {
	scanKmods();
	return FunctionCast(wrapOSKextStart, orgOSKextStart)(kext, startDependencies);
}

static void pluginStart() {
	LOG("starting");
	ihQuiesceAtBoot();
	// Registering the kexts makes Lilu build their symbol tables (kinfos) for us.
	lilu.onKextLoadForce(kexts, arrsize(kexts));
	lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
		gPatcher = &patcher;
		gKmods = reinterpret_cast<kmod_info_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_kmod"));
		LOG("patcher ready, kmod list %s, hwlibs kinfo %zu", gKmods ? "found" : "missing", kextHWLibs.loadIndex);
		KernelPatcher::RouteRequest req {"__ZN6OSKext5startEb", wrapOSKextStart, orgOSKextStart};
		if (!patcher.routeMultiple(KernelPatcher::KernelID, &req, 1))
			LOG("failed to route OSKext::start (err %d)", patcher.getError());
		patcher.clearError();
		sleepRoute(patcher);
		umdRoute(patcher);
	});
}

static const char *bootargOff[] {"-rmbgfxoff"};
static const char *bootargDebug[] {"-rmbgfxdbg"};
static const char *bootargBeta[] {"-rmbgfxbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowSafeMode | LiluAPI::AllowInstallerRecovery,
	bootargOff, arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta, arrsize(bootargBeta),
	KernelVersion::BigSur,
	KernelVersion::Sequoia,
	pluginStart
};
