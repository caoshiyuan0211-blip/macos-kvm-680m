//
//  SMU shim. HWLibs' SMU SWIP only knows SMU 9.0/11.0 (dGPU PowerPlay, VBIOS pptables). The 680M
//  has SMU 13.0.1, which already runs and manages clocks/power on its own. We replace the SWIP
//  entry points: init succeeds, the ops table is logging stubs that report success, and we send
//  the few PMFW messages that matter ourselves (message IDs: Linux smu_v13_0_1_ppsmc.h, MIT).
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>

namespace {

// MP1 mailbox (Linux smu_v13_0_set_smu_mailbox_registers): MP1 seg0 0x16000 + C2PMSG_66/82/90.
constexpr uint32_t MP1Base = 0x16000;
constexpr uint32_t RegMsg = MP1Base + 0x282, RegArg = MP1Base + 0x292, RegResp = MP1Base + 0x29a;

enum : uint32_t {
	PPSMC_MSG_TestMessage = 0x01,
	PPSMC_MSG_GetSmuVersion = 0x02,
	PPSMC_MSG_GfxDeviceDriverReset = 0x11,
	PPSMC_MSG_DisallowGfxOff = 0x1A,
};

bool waitResp(uint32_t &resp, int ms = 1000) {
	for (int i = 0; i < ms * 100; i++) {
		resp = mmioRead(RegResp);
		if (resp)
			return true;
		IODelay(10);
	}
	return false;
}

// Returns the PMFW response (1 = OK); the result argument is read back into arg.
uint32_t sendMsg(uint32_t msg, uint32_t &arg, int ms = 1000) {
	uint32_t resp;
	if (!waitResp(resp)) {
		LOG("smu: mailbox busy before msg %#x", msg);
		return 0;
	}
	mmioWrite(RegResp, 0);
	mmioWrite(RegArg, arg);
	mmioWrite(RegMsg, msg);
	if (!waitResp(resp, ms)) {
		LOG("smu: msg %#x timed out after %d ms", msg, ms);
		return 0;
	}
	arg = mmioRead(RegArg);
	return resp;
}

uint8_t gContext[0x100];  // opaque handle handed back to TTL

const char *const opNames[] {
	"event_notify", "full_asic_reset", "per_engine_reset", "pp_table_control", "overdrive_control",
	"fan_control", "thermal_control", "fullscreen_event", "dpm_control", "power_control",
	"ips_control", "azalia_control", "ulv_control", "gfx_off_control", "system_features_control",
	"i2c_control", "power_feature_caps", "pm_log", "query_ras_status", "notify_number_of_displays",
	"push_to_stb",
};
constexpr size_t OpCount = arrsize(opNames);

// Op argument shapes vary (some take an enum/int, not a struct pointer), so log raw values only.
template <size_t I>
uint64_t opStub(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t, uint64_t) {
	LOG("smu op %s(%llx, %llx, %llx, %llx)", opNames[I], a, b, c, d);
	return 0;
}

#define OPS4(n) opStub<n>, opStub<n + 1>, opStub<n + 2>, opStub<n + 3>
const Fn6 ops[] {OPS4(0), OPS4(4), OPS4(8), OPS4(12), OPS4(16), opStub<20>};
static_assert(arrsize(ops) == OpCount, "one stub per op");

uint64_t smuSwInit(uint64_t in, uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t) {
	auto i = reinterpret_cast<uint32_t *>(in);
	auto o = reinterpret_cast<uint8_t *>(out);
	if (!i || !o || i[0] != 0x2c0 || *reinterpret_cast<uint32_t *>(o) != 0x50) {
		LOG("smu_sw_init: unexpected structs");
		return 1;
	}
	*reinterpret_cast<void **>(o + 0x8) = gContext;

	uint32_t arg = 0;
	auto resp = sendMsg(PPSMC_MSG_GetSmuVersion, arg);
	LOG("smu_sw_init: PMFW version %u.%u.%u (resp %u)", (arg >> 16) & 0xFF, (arg >> 8) & 0xFF, arg & 0xFF, resp);

	// The APU keeps GC/SDMA/CP state across VM runs (and from the host driver): active HQDs, a wedged
	// KIQ, latched errors. Reset the GFX engines the way Linux does on Rembrandt (yellow_carp_mode2_reset)
	// before PSP/RLC bring-up reloads their firmware. PSP and PMFW keep running.
	constexpr uint32_t GrbmStatus = 0x1260 + 0x0da4, MecIp = 0x1260 + 0x0f48;
	LOG("smu_sw_init: before MODE2 reset: GRBM_STATUS %x MEC1 IP %x", mmioRead(GrbmStatus), mmioRead(MecIp));
	// A previous VM run leaves the CP/SDMA running (restart1: GRBM a0003028, MEC1 IP 453); PMFW then
	// never finishes MODE2 and the PSP ignores every ring command afterwards. Halt them first.
	if (!checkKernelArgument("-rmbnohalt"))
		haltGfxEngines("before MODE2");
	if (checkKernelArgument("-rmbnomode2")) {  // experiment: rely on the previous run's exit MODE2
		LOG("smu_sw_init: MODE2 skipped (-rmbnomode2)");
		return 0;
	}
	arg = 2;  // SMU_RESET_MODE_2
	resp = sendMsg(PPSMC_MSG_GfxDeviceDriverReset, arg, 5000);
	IOSleep(100);
	LOG("smu_sw_init: MODE2 reset resp %u; GRBM_STATUS %x MEC1 IP %x", resp, mmioRead(GrbmStatus), mmioRead(MecIp));
	return 0;
}

uint64_t smuHwInit(uint64_t handle, uint64_t in, uint64_t out, uint64_t, uint64_t, uint64_t) {
	if (!handle || !in || !out)
		return 1;
	// Keep GFX powered while the PSP/RLC/GC bring-up below us pokes its registers.
	uint32_t arg = 0;
	LOG("smu_hw_init: DisallowGfxOff resp %u", sendMsg(PPSMC_MSG_DisallowGfxOff, arg));
	applyGcGolden();
	startGoldenGuard();
	dmubPollerStart();

	auto table = reinterpret_cast<Fn6 *>(out + 0x8);  // smu_hw_init fills out+0x8..0xa8
	for (size_t i = 0; i < OpCount; i++)
		table[i] = ops[i];
	return 0;
}

uint64_t smuHwExit(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("smu_hw_exit");
	return 0;
}

uint64_t smuSwExit(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
	LOG("smu_sw_exit");
	return 0;
}

}  // namespace

// Stop the command processors and SDMA the way Linux gfx/sdma hw_fini do (cp_enable/sdma_enable
// false), then wait for the GUI to go idle.
void haltGfxEngines(const char *why) {
	constexpr uint32_t GC0 = 0x1260;
	constexpr uint32_t GrbmStatus = GC0 + 0x0da4, MecCntl = GC0 + 0x0f55, MeCntl = GC0 + 0x0f56,
	                   Sdma0F32Cntl = GC0 + 0x002a;
	auto before = mmioRead(GrbmStatus);
	mmioWrite(MecCntl, (1u << 30) | (1u << 28));                              // MEC_ME1/ME2_HALT
	mmioWrite(MeCntl, mmioRead(MeCntl) | (1u << 28) | (1u << 26) | (1u << 24));  // ME/PFP/CE_HALT
	mmioWrite(Sdma0F32Cntl, mmioRead(Sdma0F32Cntl) | 1);                         // HALT
	int ms = 0;
	while ((mmioRead(GrbmStatus) & (1u << 31)) && ms < 200) {
		IODelay(1000);
		ms++;
	}
	LOG("halt %s: GRBM_STATUS %x -> %x after %d ms, MEC_CNTL %x ME_CNTL %x SDMA0_F32_CNTL %x", why, before,
	    mmioRead(GrbmStatus), ms, mmioRead(MecCntl), mmioRead(MeCntl), mmioRead(Sdma0F32Cntl));
}

// Linux resets the APU's GFX (yellow_carp_mode2_reset) only after psp_suspend; doing it at the next
// VM start instead finds the CP running again (QEMU's device reset clears our halt) and PMFW never
// answers (restart2). Called at guest shutdown after the PSP teardown (-rmbnoexitreset: don't).
void smuExitReset(const char *tag) {
	if (checkKernelArgument("-rmbnoexitreset"))
		return;
	uint32_t arg = 2;  // SMU_RESET_MODE_2
	auto resp = sendMsg(PPSMC_MSG_GfxDeviceDriverReset, arg, 5000);
	IOSleep(100);
	LOG("%s: MODE2 reset resp %u; GRBM_STATUS %x MEC1 IP %x, SMU resp now %x", tag, resp,
	    mmioRead(0x1260 + 0x0da4), mmioRead(0x1260 + 0x0f48), mmioRead(RegResp));
}

void smuRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest requests[] {
		{"_smu_sw_init", smuSwInit},
		{"_smu_hw_init", smuHwInit},
		{"_smu_hw_exit", smuHwExit},
		{"_smu_sw_exit", smuSwExit},
	};
	if (!patcher.routeMultiple(index, requests, address, size))
		LOG("smu: failed to route SWIP entry points (err %d)", patcher.getError());
	else
		LOG("smu: SWIP entry points replaced");
	patcher.clearError();
}
