//
//  DMUB on DCN 3.1: DAL only has DCN 3.0 code, whose reset_release clears DMCUB_CNTL.SOFT_RESET
//  (bit 17). On DCN 3.1 that bit is gone and the reset is DMCUB_CNTL2.SOFT_RESET (bit 0), so the
//  DMCUB never leaves reset. Poll (1 ms) from framebuffer start and, once DAL has set
//  DMCUB_CNTL.ENABLE with bit 17 clear, clear DMCUB_CNTL2.SOFT_RESET as dmub_dcn31_reset_release does.
//

#include "rmb.hpp"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <Headers/kern_api.hpp>

// VBIOSSMC mailbox (MP1_SMN_C2PMSG_67/83/91), as dcn31_smu_send_msg_with_param.
uint32_t vbiosSmcSend(uint32_t msg, uint32_t param) {
	constexpr uint32_t Msg = 0x16283, Arg = 0x16293, Resp = 0x1629b;
	for (uint32_t t = 0; t < 1000 && mmioRead(Resp) == 0; t++)
		IODelay(10);
	mmioWrite(Resp, 0);
	mmioWrite(Arg, param);
	mmioWrite(Msg, msg);
	uint32_t r = 0;
	for (uint32_t t = 0; t < 20000 && (r = mmioRead(Resp)) == 0; t++)
		IODelay(10);
	return r;
}

namespace {

constexpr uint32_t DcnSeg2 = 0x34C0;
constexpr uint32_t DmcubCntl = DcnSeg2 + 0x1f6, DmcubCntl2 = DcnSeg2 + 0x200, DmcubScratch0 = DcnSeg2 + 0x1e3,
				   DmcubTimer = DcnSeg2 + 0x1fd, DmcubGpintIn1 = DcnSeg2 + 0x1f8, DmcubScratch7 = DcnSeg2 + 0x1ea,
				   MmhubbubSoftReset = DcnSeg2 + 0x342, DmcubInbox1Wptr = DcnSeg2 + 0x1d6, DmcubInbox1Rptr = DcnSeg2 + 0x1d7,
				   DmcubOutbox0Wptr = DcnSeg2 + 0x1da, DmcubOutbox0Rptr = DcnSeg2 + 0x1db, DmcubOutbox1Wptr = DcnSeg2 + 0x1de,
				   DmcubOutbox1Rptr = DcnSeg2 + 0x1df;

// -rmbnodmub: never release the DMCUB (test whether the firmware boot gates the eDP PHY).
bool gNoDmub;
// -rmbzstate: keep PMFW's default Z-state / display idle optimizations (Navi23 DAL never controls them;
// Linux dcn31 clk_mgr disallows Z-states unless it explicitly allows them).
bool gKeepZstate;
// -rmbnoclkvote: don't send Linux dcn31 clk_mgr's display clock floors. Navi23 DAL sends its clock requests
// to the Navi MP1 mailbox (0x1628a, never answered on Rembrandt), so PMFW gets no display votes; GOP-posted
// boots keep the firmware's, but after S3 / amdgpu unload the PMFW starts clean and a cold eDP PHY dies ~10 s
// after DMCUB release.
bool gNoClkVote;


// RDPCSTX0 CLOCK_CNTL / TX_SRAM_CNTL / PHY_CNTL0 / PHY_CNTL1 (only read while the block answers).
constexpr uint32_t PhyRegs[] = {0x5df1, 0x5df6, 0x5e00, 0x5e01};
// TX_CLK / SRAMCLK / OCLACLK gate-disable: set in every GOP-lit state; a cold PHY (S3 wake) dies async.
constexpr uint32_t PhyClkGateDis = 0x101100;
// -rmbphycg: don't force the RDPCS clock-gate-disable bits.
bool gKeepPhyCg;

void watchPhyRegs(uint32_t ms) {
	static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
	uint32_t v[4];
	for (int k = 0; k < 4; k++)
		v[k] = mmioRead(PhyRegs[k]);
	if (!gKeepPhyCg && v[0] && (v[0] & PhyClkGateDis) != PhyClkGateDis) {
		mmioWrite(PhyRegs[0], v[0] | PhyClkGateDis);
		LOG("phyregs: +%u ms forced RDPCS clock-gate disable: CLOCK_CNTL %x -> %x", ms, v[0], mmioRead(PhyRegs[0]));
		v[0] = mmioRead(PhyRegs[0]);
	}
	if (memcmp(v, last, sizeof(v))) {
		LOG("phyregs: +%u ms CLOCK_CNTL %x SRAM_CNTL %x PHY_CNTL0 %x PHY_CNTL1 %x", ms, v[0], v[1], v[2], v[3]);
		memcpy(last, v, sizeof(v));
	}
}

void poll(void *, wait_result_t) {
	uint32_t released = 0, phy = ~0u;
	// 1 ms for the first minute (boot traces), then 10 ms forever: S3 wake resets the DMCUB again.
	for (uint32_t i = 0;; i += i < 60000 ? 1 : 10) {  // i = ms since start
		IOSleep(i < 60000 ? 1 : 10);
		if (!mmioAvailable())
			continue;
		// Without -rmbphypoll: sample PHY A only every 2 s, and only while the DMUB inbox is idle.
		if (phyPollOn() || (i % 2000 == 0 && mmioRead(DmcubInbox1Rptr) == mmioRead(DmcubInbox1Wptr))) {
			uint32_t p = mmioRead(PhyA);
			if (p != phy) {
				LOG("phy: +%u ms RDPCSTX0 %x -> %x", i, phy, p);
				if (p == 0 && phy != ~0u)
					traceDump("RDPCSTX0 died");
				phy = p;
			}
			if (p)
				watchPhyRegs(i);
		}
		uint32_t cntl = mmioRead(DmcubCntl), cntl2 = mmioRead(DmcubCntl2);
		bool want = (cntl & (1u << 16)) && !(cntl & (1u << 17));
		if (want && (cntl2 & 1) && gNoDmub) {
			static bool logged;
			if (!logged) {
				logged = true;
				LOG("dmub: +%u ms CNTL %x: DAL wants DMCUB running, held in reset (-rmbnodmub)", i, cntl);
			}
		} else if (want && (cntl2 & 1)) {
			mmioWrite(MmhubbubSoftReset, mmioRead(MmhubbubSoftReset) & ~0x100u);  // DMUIF_SOFT_RESET
			mmioWrite(DmcubCntl2, cntl2 & ~1u);
			released++;
			LOG("dmub: +%u ms CNTL %x: released DMCUB_CNTL2 soft reset (CNTL2 now %x), PHY %x", i, cntl, mmioRead(DmcubCntl2),
				phyPeek());
			for (uint32_t t = 0; t < 50 && phyPollOn(); t++) {  // fine-grained: does the firmware boot itself gate the PHY?
				IODelay(100);
				uint32_t q = mmioRead(PhyA);
				if (q != phy) {
					LOG("phy: +%u ms +%u us after release RDPCSTX0 %x -> %x (SCRATCH0 %x)", i, (t + 1) * 100, phy, q,
						mmioRead(DmcubScratch0));
					phy = q;
				}
			}
			IOSleep(5);
			LOG("dmub: after 5 ms SCRATCH0 %x TIMER %x", mmioRead(DmcubScratch0), mmioRead(DmcubTimer));
			if (!gKeepZstate) {
				uint32_t r1 = vbiosSmcSend(0x16, 0);  // DisallowZstatesEntry
				uint32_t r2 = vbiosSmcSend(0x12, 0);  // SetDisplayIdleOptimizations(0)
				LOG("dmub: VBIOSSMC DisallowZstatesEntry resp %x, SetDisplayIdleOptimizations(0) resp %x, PHY %x", r1, r2,
					phyPeek());
			}
			if (!gNoClkVote) {
				// VBIOSSMC SetHardMinDcfclkByFreq / SetMinDeepSleepDcfclk / SetDispclkFreq / SetDppclkFreq (MHz),
				// then SetPhyclkVoltageByFreq for HBR3 symbol clock; arg reads back the granted frequency.
				static constexpr struct { uint32_t msg, mhz; const char *name; } Votes[] = {
					{0x7, 600, "HardMinDcfclk"}, {0x8, 400, "MinDeepSleepDcfclk"}, {0x4, 600, "Dispclk"},
					{0x6, 600, "Dppclk"}, {0x9, 810, "PhyclkVoltage"}};
				for (auto &v : Votes) {
					uint32_t r = vbiosSmcSend(v.msg, v.mhz);
					LOG("dmub: VBIOSSMC Set%s(%u) resp %x arg %u, PHY %x", v.name, v.mhz, r, mmioRead(0x16293), phyPeek());
				}
			}
		} else if (!want && !(cntl2 & 1) && released) {
			// DAL reset the DMCUB (DCN 3.0 style): mirror it into CNTL2.
			mmioWrite(DmcubCntl2, cntl2 | 1u);
			LOG("dmub: +%u ms CNTL %x: DAL reset DMCUB, set CNTL2 soft reset", i, cntl);
		}
	}
}

}  // namespace

bool phyPollOn() {
	static int on = -1;
	if (on < 0)
		on = checkKernelArgument("-rmbphypoll");
	return on;
}

uint32_t phyPeek() { return phyPollOn() ? mmioRead(PhyA) : 0xeeeeeeee; }

// Before the PSP (re)loads the DMUB image: S3 wake leaves the DMCUB running from before sleep, and the
// PSP rejects LOAD_IP_FW (0x80000029) for a running DMCUB. Same sequence as Linux dmub_dcn31_reset.
// -rmbnodmubrst skips it.
void dmubResetForLoad() {
	if (!mmioAvailable() || checkKernelArgument("-rmbnodmubrst"))
		return;
	uint32_t cntl2 = mmioRead(DmcubCntl2);
	uint32_t i, ack = 0, resp = 0, cntl = mmioRead(DmcubCntl);
	// Already in soft reset (e.g. after sleep): no fw to stop, but ENABLE may still be set.
	if (!(cntl2 & 1)) {
		mmioWrite(DmcubGpintIn1, (1u << 28) | (2u << 16));  // status 1, DMUB_GPINT__STOP_FW
		for (i = 0; i < 100 && (mmioRead(DmcubGpintIn1) >> 28) & 1; i++)
			IODelay(1);
		ack = i;
		for (i = 0; i < 100 && (resp = mmioRead(DmcubScratch7)) != 0xDEADDEAD; i++)
			IODelay(1);
		for (i = 0; i < 100 && !((cntl = mmioRead(DmcubCntl)) & (1u << 20)); i++)
			IODelay(1);
	}
	mmioWrite(DmcubCntl2, cntl2 | 1u);
	mmioWrite(DmcubCntl, mmioRead(DmcubCntl) & ~(1u << 16));
	mmioWrite(MmhubbubSoftReset, mmioRead(MmhubbubSoftReset) | 0x100u);
	const uint32_t zero[] = {DmcubInbox1Rptr, DmcubInbox1Wptr, DmcubOutbox1Rptr, DmcubOutbox1Wptr,
							 DmcubOutbox0Rptr, DmcubOutbox0Wptr, DmcubScratch0};
	for (uint32_t r : zero)
		mmioWrite(r, 0);
	mmioWrite(DmcubGpintIn1, 0);
	inboxReset();
	LOG("dmub-reset: DMCUB stopped for fw load (CNTL2 was %x, ack %u us, resp %x, CNTL %x) -> CNTL %x CNTL2 %x", cntl2, ack, resp, cntl,
		mmioRead(DmcubCntl), mmioRead(DmcubCntl2));
}

void dmubPollerStart() {
	static bool started;
	if (started)
		return;
	started = true;
	gNoDmub = checkKernelArgument("-rmbnodmub");
	gKeepZstate = checkKernelArgument("-rmbzstate");
	gNoClkVote = checkKernelArgument("-rmbnoclkvote");
	gKeepPhyCg = checkKernelArgument("-rmbphycg");
	thread_t t;
	if (kernel_thread_start(poll, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
}
