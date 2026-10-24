//
//  GC firmware substitution. HWLibs embeds Navi 23's signed GC microcode and hands it to the PSP,
//  which rejects it on Rembrandt (status 0x8000030a). Point HWLibs' firmware descriptors at the
//  Rembrandt images instead. Descriptor layout (0x38 bytes): +0x0C u32 size, +0x20 data pointer.
//

#include "rmb.hpp"
#include "fw.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>

namespace {

struct Substitution {
	const char *symbol;
	const uint8_t *data;
	uint32_t size;
};

// psp_tmr_init sends the TOC (psp+0x3588 data, psp+0x3580 size) with LOAD_TOC; the PSP answers
// with the TMR size it needs. Rembrandt's PSP rejects Navi 23's TOC, so give it yellow_carp_toc.
mach_vm_address_t orgPspTmrInit;
mach_vm_address_t gPspTmrUnload;
mach_vm_address_t gPspRingStop;
uint64_t gPsp;
IONotifier *gPowerNotifier;

// A VM restart (or guest S3 resume) can't re-init a PSP that still has the old run's TAs, TMR and
// rings: every KM command then times out. Apple never tears these down at shutdown, and
// psp_hw_exit's TMR destroy fails while TAs are still loaded (pexit1: psp_tmr_destroy failed; the
// next VM boot froze the host). Use Apple's own S3 sequence (psp_power_down_event's full branch,
// same order as Linux psp_suspend): unload the TAs, then the TMR, then stop the UM and KM rings
// (-rmbnopspexit: don't).
const char *const pspTeardown[] {"_psp_hdcp_unload", "_psp_hc_unload", "_psp_ras_unload", "_psp_dtm_unload",
	"_psp_asd_unload", "_psp_tmr_unload"};
mach_vm_address_t gPspTeardown[arrsize(pspTeardown)];
void pspTeardownSeq(uint64_t p, const char *tag) {
	for (size_t i = 0; i < arrsize(pspTeardown); i++) {
		if (!gPspTeardown[i])
			continue;
		auto r = reinterpret_cast<uint64_t (*)(uint64_t)>(gPspTeardown[i])(p);
		LOG("%s: %s = %llx, C2PMSG_64 %08x", tag, pspTeardown[i] + 1, r & 0xFFFFFFFF, mmioRead(0x16080));
	}
	for (uint64_t ring = 1; ring <= 2; ring++) {
		auto r = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(gPspRingStop)(p, ring);
		LOG("%s: psp_ring_stop(%llu) = %llx, C2PMSG_64 %08x", tag, ring, r & 0xFFFFFFFF, mmioRead(0x16080));
	}
}
IOReturn powerHandler(void *, void *, UInt32 type, IOService *, void *, vm_size_t) {
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) {
		edpAllowPowerOff("shutdown");
		edpPowerOffNow("shutdown");
	}
	else if (type == kIOMessageSystemWillSleep) {
		edpAllowPowerOff("sleep");
		gWakeWatchArmed = true;
		gEdpLinkPause = true;
	}
	else if (type == kIOMessageSystemHasPoweredOn) {
		edpAllowPowerOff(nullptr);  // aborted or finished sleep: wake training OFFs must stay NULLed
		panelWakeWatch();
		gEdpLinkPause = false;
		gEdpLinkKick = true;
	}
	if ((type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) && gPsp && gPspRingStop) {
		auto p = gPsp;
		gPsp = 0;
		LOG("psp-exit: %s, C2PMSG_64 %08x", type == kIOMessageSystemWillPowerOff ? "power off" : "restart",
		    mmioRead(0x16080));
		if (!checkKernelArgument("-rmbnohalt"))
			haltGfxEngines("at shutdown");
		pspTeardownSeq(p, "psp-exit");
		smuExitReset("psp-exit");
		quiesceIh("at shutdown");
	}
	return kIOReturnSuccess;
}
uint64_t wrapPspTmrInit(uint64_t psp, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto p = reinterpret_cast<uint8_t *>(psp);
	LOG("fw: TOC %#x bytes -> %#x", *reinterpret_cast<uint32_t *>(p + 0x3580), FW_SIZE(toc));
	*reinterpret_cast<const uint8_t **>(p + 0x3588) = fw_yc_toc;
	*reinterpret_cast<uint32_t *>(p + 0x3580) = FW_SIZE(toc);
	auto r = reinterpret_cast<Fn6>(orgPspTmrInit)(psp, b, c, d, e, f);
	LOG("fw: psp_tmr_init = %llx, TMR size %#llx", r & 0xFFFFFFFF, *reinterpret_cast<uint64_t *>(p + 0xaf8));
	// A previous VM run (or a guest S3 resume) leaves the PSP's TMR set up, so LOAD_TOC fails.
	// Linux destroys the TMR on suspend; do it here and retry (-rmbnotmrfix: don't).
	if ((r & 0xFFFFFFFF) && gPspTmrUnload && !checkKernelArgument("-rmbnotmrfix")) {
		auto u = reinterpret_cast<uint64_t (*)(uint64_t)>(gPspTmrUnload)(psp);
		r = reinterpret_cast<Fn6>(orgPspTmrInit)(psp, b, c, d, e, f);
		LOG("fw: stale TMR: psp_tmr_unload = %llx, retry psp_tmr_init = %llx, TMR size %#llx", u & 0xFFFFFFFF,
		    r & 0xFFFFFFFF, *reinterpret_cast<uint64_t *>(p + 0xaf8));
	}
	if (!(r & 0xFFFFFFFF) && !checkKernelArgument("-rmbnopspexit")) {
		gPsp = psp;
		if (!gPowerNotifier)
			gPowerNotifier = registerPrioritySleepWakeInterest(powerHandler, nullptr, nullptr);
		LOG("psp-exit: armed (notifier %s)", gPowerNotifier ? "ok" : "MISSING");
	}
	return r;
}

// psp_power_down_event(psp, flags*): with psp+0x28 bit 5 clear (dGPU BACO style, our case) a real
// power-down (*flags & 1) only saves np fw state and keeps the TAs, TMR and rings, assuming the
// PSP keeps its state. The guest's S3 wake re-inits the PSP from scratch (power_up_event calls
// psp_hardware_initialization), but the old TMR still holds DMUB, so its LOAD_IP_FW fails with
// 0x80000029/0x80000007 (s3wake4). Do the full branch's teardown too (-rmbnos3psp: don't).
mach_vm_address_t orgPspPowerDown;
uint64_t wrapPspPowerDown(uint64_t psp, uint64_t flags, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	bool light = !(*reinterpret_cast<uint64_t *>(psp + 0x28) & 0x20) && (*reinterpret_cast<uint8_t *>(flags) & 1);
	gartCensus("sleep entry");
	auto r = reinterpret_cast<Fn6>(orgPspPowerDown)(psp, flags, c, d, e, f);
	LOG("psp-sleep: power_down_event (%s branch), C2PMSG_64 %08x", light ? "light" : "full/none", mmioRead(0x16080));
	edpPanelPowerLost();
	if (light && gPspRingStop && !checkKernelArgument("-rmbnos3psp")) {
		pspTeardownSeq(psp, "psp-sleep");
		// Save before MODE2: it resets the GCVM context regs, so a later save sees no table (s3wake13).
		gartCensus("after psp-sleep");
		gartSave();
		// Guest S3 keeps the APU powered: the MEC that hung on Apple's sleep-entry UNMAP_QUEUES
		// (s3wake11: KIQ rptr 86/a0, MEC2 IP 4a1) is still hung on wake and never fetches the KIQ
		// ring again. Reset GFX like at shutdown (-rmbnos3mode2: don't).
		if (!checkKernelArgument("-rmbnos3mode2")) {
			haltGfxEngines("at sleep");
			smuExitReset("psp-sleep");
		}
	} else {
		gartCensus("after psp-sleep");
		gartSave();
	}
	return r;
}

// psp_np_fw_load walks HWLibs' firmware list and asks this which types to load (HWLibs' own type
// numbering; the PSP failure event tag is this type). Skip Navi 23-only images Rembrandt lacks.
bool skipFwType(uint32_t type) {
	switch (type) {
		case 30:  // global tap delays (Navi 23 RLC v2.4 only)
		case 31:  // SE0 tap delays
		case 32:  // SE1 tap delays
			return true;
		default:
			return false;
	}
}

mach_vm_address_t orgCapCheck;
uint64_t wrapCapCheck(uint64_t psp, uint64_t type, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = skipFwType(type & 0xFFFFFFFF) ? 0 : reinterpret_cast<Fn6>(orgCapCheck)(psp, type, c, d, e, f) & 0xFF;
	if (r && (type & 0xFFFFFFFF) == 35)  // DMUB
		dmubResetForLoad();
	if (rmbVerbose())
		LOG("fw: np_fw_load type %llu -> %s", type & 0xFFFFFFFF, r ? "load" : "skip");
	return r;
}

// DMCUB: dmcub_set_fw_entry_info sends DCN 3.1.x to dmcub_dcn31_get_fw_constrant, which only
// records the version; HWLibs has no DCN 3.1 image, so the DMUB firmware is never handed to the
// PSP and DAL's DMUB never comes up. Queue Rembrandt's signed image the way the DCN 3.0.2 path
// queues atidmcub_0.dat: dmcub_load_fw(dmcub, name, data, size, fw entry list).
mach_vm_address_t orgDcn31FwConstraint, gDmcubLoadFw;
uint64_t wrapDcn31FwConstraint(uint64_t ver, uint64_t dmcub, uint64_t list, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgDcn31FwConstraint)(ver, dmcub, list, d, e, f);
	uint64_t ok = 0;
	if (gDmcubLoadFw)
		ok = reinterpret_cast<Fn6>(gDmcubLoadFw)(dmcub, reinterpret_cast<uint64_t>("atidmcub_0.dat"),
			reinterpret_cast<uint64_t>(fw_yc_dmcub), FW_SIZE(dmcub), list, 0) & 0xFF;
	LOG("fw: DCN %llx: queued yellow_carp DMCUB (%#x bytes) -> %llu", ver & 0xFFFFFFFF, FW_SIZE(dmcub), ok);
	return r;
}

}  // namespace


void firmwareRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	const Substitution subs[] {
		{"_gc_10_3_4_me_ucode", fw_yc_me, FW_SIZE(me)},
		{"_gc_10_3_4_pfp_ucode", fw_yc_pfp, FW_SIZE(pfp)},
		{"_gc_10_3_4_ce_ucode", fw_yc_ce, FW_SIZE(ce)},
		{"_gc_10_3_4_mec_ucode", fw_yc_mec, FW_SIZE(mec)},
		{"_gc_10_3_4_mec_jt_ucode", fw_yc_mec_jt, FW_SIZE(mec_jt)},
		{"_gc_10_3_4_rlc_ucode", fw_yc_rlc, FW_SIZE(rlc)},
		{"_gc_10_3_4_rlc_srlist_cntl", fw_yc_srlist_cntl, FW_SIZE(srlist_cntl)},
		{"_gc_10_3_4_rlc_srlist_gpm_mem", fw_yc_srlist_gpm, FW_SIZE(srlist_gpm)},
		{"_gc_10_3_4_rlc_srlist_srm_mem", fw_yc_srlist_srm, FW_SIZE(srlist_srm)},
		{"_gc_10_3_4_rlc_lx6_iram_ucode", fw_yc_rlc_iram, FW_SIZE(rlc_iram)},
		{"_gc_10_3_4_rlc_lx6_dram_ucode", fw_yc_rlc_dram, FW_SIZE(rlc_dram)},
	};
	KernelPatcher::RouteRequest reqs[] {
		{"_psp_tmr_init", wrapPspTmrInit, orgPspTmrInit},
		{"_psp_np_fw_load_capability_check", wrapCapCheck, orgCapCheck},
		{"_dmcub_dcn31_get_fw_constrant", wrapDcn31FwConstraint, orgDcn31FwConstraint},
		{"_psp_power_down_event", wrapPspPowerDown, orgPspPowerDown},
	};
	gDmcubLoadFw = patcher.solveSymbol(index, "_dmcub_load_fw", address, size);
	patcher.clearError();
	gPspTmrUnload = patcher.solveSymbol(index, "_psp_tmr_unload", address, size);
	patcher.clearError();
	gPspRingStop = patcher.solveSymbol(index, "_psp_ring_stop", address, size);
	patcher.clearError();
	for (size_t i = 0; i < arrsize(pspTeardown); i++) {
		gPspTeardown[i] = patcher.solveSymbol(index, pspTeardown[i], address, size);
		patcher.clearError();
		if (!gPspTeardown[i])
			LOG("fw: %s not found", pspTeardown[i]);
	}
	if (!patcher.routeMultiple(index, reqs, address, size))
		LOG("fw: failed to route PSP firmware hooks (err %d)", patcher.getError());
	patcher.clearError();

	for (auto &s : subs) {
		auto desc = patcher.solveSymbol<uint8_t *>(index, s.symbol, address, size);
		patcher.clearError();
		if (!desc) {
			LOG("fw: %s not found", s.symbol);
			continue;
		}
		auto oldSize = *reinterpret_cast<uint32_t *>(desc + 0x0C);
		*reinterpret_cast<uint32_t *>(desc + 0x0C) = s.size;
		*reinterpret_cast<const uint8_t **>(desc + 0x20) = s.data;
		LOG("fw: %s %#x -> %#x bytes", s.symbol + 11, oldSize, s.size);
	}
}
