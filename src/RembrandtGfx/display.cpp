//
//  DCN 3.1.2 under DAL's DCN 3.0.2 code: DAL reaches registers only through AmdDalHelper's
//  dalAtomRead/WriteRegister32 callbacks. Remap every 3.0.2 address whose register moved in 3.1.2
//  (dcnremap.hpp, generated from the Linux headers by scripts/dcn-remap.py).
//

#include "rmb.hpp"
#include "dcnremap.hpp"
#include "phyvalid.hpp"
#include <string.h>
#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>

namespace {

const DcnRemap *find(uint32_t reg) {
	size_t lo = 0, hi = sizeof(kDcnRemap) / sizeof(kDcnRemap[0]);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (kDcnRemap[mid].from == reg)
			return &kDcnRemap[mid];
		if (kDcnRemap[mid].from < reg)
			lo = mid + 1;
		else
			hi = mid;
	}
	return nullptr;
}

// Backlight PWM: DCN 3.0.2 BL_PWM_CNTL/CNTL2/PERIOD_CNTL/GRP1_REG_LOCK (seg2 0x2888-0x288b) are holes on
// 3.1.2, where they live in PWRSEQ0 (seg2 0x2f19-0x2f1c). Not in dcnremap.hpp: the names differ, so
// scripts/dcn-remap.py can't pair them. LVTMA_PWRSEQ_CNTL/STATE stay unmapped on purpose (eDP power
// sequencing works as is). -rmbabm: no remap.
bool gKeepAbm;

uint32_t remap(uint32_t reg, bool write) {
	if (!gKeepAbm && reg >= 0x34C0 + 0x2888 && reg <= 0x34C0 + 0x288b) {
		static uint32_t loggedBl;
		if (loggedBl++ < 20)
			LOG("bl: DAL %s BL_PWM reg %x -> %x", write ? "write" : "read", reg, reg + 0x2f19 - 0x2888);
		return reg + 0x2f19 - 0x2888;
	}
	auto m = find(reg);
	if (!m)
		return reg;
	static uint32_t logged;
	if (logged < 60) {
		logged++;
		LOG("dcn: %s %s %x -> %x", write ? "write" : "read", m->name, reg, m->to);
	}
	return m->to;
}

// Registers at the same address whose fields moved (scripts/dcn-fielddiff.py). ODMn
// OPTC_DATA_SOURCE_SELECT: 3.0.2 NUM_OF_OUTPUT_SEGMENT 3:2, SEG0-3_SRC_SEL 23:8; 3.1.2 9:8, 31:16 (left
// as reset 0xf, the OTG gets no OPP and scans out black). HUBPRETn_CONTROL: PACK_3TO2_ELEMENT_DISABLE
// bit 12 -> 15; DET_BUF_PLANE1_BASE_ADDRESS has other units on 3.1.2 (Linux never sets it), kept as is.
enum class Fields { None, OdmSrc, HubpRet };

Fields fieldKind(uint32_t hw) {
	if (hw >= 0x4f8b && hw <= 0x4fbb && (hw & 0xf) == 0xb)
		return Fields::OdmSrc;
	if (hw >= 0x3b2c && hw <= 0x3b2c + 3 * 0xdc && (hw - 0x3b2c) % 0xdc == 0)
		return Fields::HubpRet;
	return Fields::None;
}

uint32_t fieldsToDal(Fields k, uint32_t h) {
	if (k == Fields::OdmSrc)
		return (h & 3) | ((h >> 8 & 3) << 2) | ((h >> 16) << 8);
	return (h & 0xffff0000) | ((h >> 15 & 1) << 12);
}

uint32_t fieldsToHw(Fields k, uint32_t hw, uint32_t v, uint32_t old) {
	uint32_t h = k == Fields::OdmSrc ? (v & 3) | ((v >> 2 & 3) << 8) | ((v >> 8 & 0xffff) << 16)
									 : (v & 0xffff0000) | (old & 0x1ff0) | ((v >> 12 & 1) << 15);
	static uint32_t logged;
	if (logged++ < 40)
		LOG("dcn: field remap write %x: DAL %x -> hw %x", hw, v, h);
	return h;
}

// Power gating. DCN 3.0.2: HUBP pipe n = DOMAIN(2n), DPP pipe n = DOMAIN(2n+1) (0..9), DSC =
// DOMAIN16..20; DOMAINx_PG_STATUS follows its PG_CONFIG. DCN 3.1.2 has one domain per pipe
// (DOMAIN0..3) and DSC DOMAIN16..18 at other offsets, so DAL's gating lands on the wrong domains
// and its status waits time out. DAL gets a shadow PG block (status mirrors the requested state)
// and the real domains are held on with DOMAIN_POWER_FORCEON.
constexpr uint32_t Seg2 = 0x34C0, PgFirst = Seg2 + 0x80, PgLast = Seg2 + 0xaa;
uint32_t gPgShadow[PgLast - PgFirst + 1];
bool gPgForced;

bool isPgStatus(uint32_t off) {  // seg2 offset, DCN 3.0.2 numbering
	return (off >= 0x81 && off <= 0x93 && (off & 1)) || (off >= 0xa2 && off <= 0xaa && !(off & 1));
}

void forceDomainsOn() {
	if (gPgForced)
		return;
	gPgForced = true;
	static const uint32_t domains[] {0x80, 0x82, 0x84, 0x86, 0x89, 0x8b, 0x8d};  // DCN 3.1.2 DOMAIN0-3, 16-18 PG_CONFIG
	for (uint32_t d : domains) {
		uint32_t v = mmioRead(Seg2 + d);
		mmioWrite(Seg2 + d, (v | 1u) & ~(1u << 8));  // FORCEON, not POWER_GATE
		LOG("dcn: PG domain cfg %x: %x -> %x, status %x", Seg2 + d, v, mmioRead(Seg2 + d), mmioRead(Seg2 + d + 1));
	}
}

constexpr uint32_t CcDcPipeDis = Seg2 + 0xca;

constexpr uint32_t TraceLen = 256;
struct TraceEnt {
	uint64_t t;
	uint32_t reg, hw, val;
	bool rd;
};
TraceEnt gTrace[TraceLen];
uint32_t gTraceN;

uint64_t nowUs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

void traceAccess(bool rd, uint32_t reg, uint32_t hw, uint32_t val) {
	auto &e = gTrace[gTraceN++ % TraceLen];
	e = {nowUs(), reg, hw, val, rd};
}

// RDPCSTX0 liveness around every DAL register access: shows whether PHY A dies on a DAL access
// or between two of them (another agent: DMUB firmware, PMFW).
bool gPhyAlive;
uint64_t gLastAccessUs;
uint32_t gLastReg;
bool gLastRd;

void phyEnter(bool rd, uint32_t r) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died between DAL accesses: after %s %x (%llu us ago), before %s %x", gLastRd ? "read" : "write",
			gLastReg, nowUs() - gLastAccessUs, rd ? "read" : "write", r);
		traceDump("died between DAL accesses");
	}
	gPhyAlive = alive;
}

void phyExit(bool rd, uint32_t r, uint32_t hw, uint32_t val) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died right after DAL %s reg %x (hw %x) val %x", rd ? "read" : "write", r, hw, val);
		traceDump("died on a DAL access");
	}
	gPhyAlive = alive;
	gLastAccessUs = nowUs();
	gLastReg = r;
	gLastRd = rd;
}

extern bool gKeepPhyWrites;

// HUBPREQn_DCSURF_SURFACE_CONTROL (pipe stride 0xdc): with DCC decode on, the scanout is scrambled
// (Sequoia wallpaper recognizable only by color); with it cleared the image is clean, so the
// framebuffer content is not actually DCC-compressed the way HUBP expects. Clear all DCC enables /
// independent-block bits. -rmbdcc keeps them.
bool gKeepDcc;
constexpr uint32_t SurfCtl0 = 0x3ada, SurfCtlDcc = 0x2626;
bool isSurfCtl(uint32_t hw) { return hw >= SurfCtl0 && hw <= SurfCtl0 + 3 * 0xdc && (hw - SurfCtl0) % 0xdc == 0; }
bool isPhyHole(uint32_t hw);

// HUBPREQn_DCN_VM_SYSTEM_APERTURE_LOW/HIGH_ADDR: on the APU the framebuffer (MC 0xf4xxxxxxxx) is only
// reachable through the system aperture. The GOP sets it for HUBP0 only and Navi23 DAL (dGPU, no
// vm_pa_config) never writes it, so any other pipe fetches nothing: HUBP underflow, the screen shows
// the MPCC background (0,0,0 = pure green on a YCbCr HDMI stream). Program it like Linux
// hubp21_set_vm_system_aperture_settings from DCN_VM_FB_LOCATION before each surface address write.
// -rmbnoaperture leaves it.
bool gNoAperture;
constexpr uint32_t SurfAddr0 = 0x3aca, AptLow0 = 0x3af5, AptHigh0 = 0x3af6, FbLocBase = 0x3935, FbLocTop = 0x3936;
bool isSurfAddr(uint32_t hw) { return hw >= SurfAddr0 && hw <= SurfAddr0 + 3 * 0xdc && (hw - SurfAddr0) % 0xdc == 0; }
void fixAperture(uint32_t pipe) {
	uint32_t base = mmioRead(FbLocBase) & 0xFFFFFF, top = mmioRead(FbLocTop) & 0xFFFFFF;
	if (!base || top < base)
		return;
	uint32_t lo = base << 6, hi = ((top + 1) << 6) - 1;  // 16 MB units -> 256 KB units
	uint32_t off = pipe * 0xdc;
	if (mmioRead(AptLow0 + off) == lo && mmioRead(AptHigh0 + off) == hi)
		return;
	static uint32_t logged;
	if (logged++ < 8)
		LOG("dcn: HUBP%u system aperture %x-%x -> %x-%x", pipe, mmioRead(AptLow0 + off), mmioRead(AptHigh0 + off), lo, hi);
	mmioWrite(AptLow0 + off, lo);
	mmioWrite(AptHigh0 + off, hi);
}

// DCHUBBUB_DETn_CTRL: DCN 3.1 splits the 1792 KB return buffer (28 x 64 KB segments) between per-pipe
// detile buffers and the compressed buffer; Navi23 (DCN 3.0, fixed DET) DAL never sizes it. The GOP
// gives DET0 3 segments and every other pipe 0 -> that HUBP can't fetch at all (HUBP_UNDERFLOW, same
// green screen). Give an unsized pipe Linux's default 384 KB (6 segments) if the buffer has room.
// Verified live on HDMI pipe 1 (underflow gone). -rmbnodet leaves it.
bool gNoDet;
constexpr uint32_t CompbufCtl = 0x398b, Det0Ctl = 0x398c, CrbSegs = 28, DefaultDetSegs = 6;
void fixDet(uint32_t pipe) {
	if (mmioRead(Det0Ctl + pipe) & 7)
		return;
	uint32_t used = mmioRead(CompbufCtl) & 0x1f;
	for (uint32_t i = 0; i < 4; i++)
		used += mmioRead(Det0Ctl + i) & 7;
	uint32_t segs = used >= CrbSegs ? 0 : CrbSegs - used < DefaultDetSegs ? CrbSegs - used : DefaultDetSegs;
	LOG("dcn: HUBP%u DET size 0 -> %u segments (%u in use)", pipe, segs, used);
	if (segs)
		mmioWrite(Det0Ctl + pipe, segs);
}

// DP_DTOn_MODULO (DCCG, seg1 0xC0 + 0x82 + 4n): pixel clock = DTO ref * PHASE / MODULO, and DAL
// writes MODULO = its idea of the DTO ref (Navi23: 730 MHz). Rembrandt's DP DTO ref is dprefclk =
// 600 MHz (Linux dcn31_clk_mgr), so the panel ran at 49.3 Hz instead of 60 and flashed white on
// static screens. Rescale MODULO to the real ref. -rmbdto keeps DAL's value.
bool gKeepDto;
constexpr uint32_t DpDto0Modulo = 0xC0 + 0x82, NaviDtoRefKhz = 730000, RmbDtoRefKhz = 600000;
bool isDtoModulo(uint32_t hw) { return hw >= DpDto0Modulo && hw <= DpDto0Modulo + 3 * 4 && (hw - DpDto0Modulo) % 4 == 0; }
// DCHUBBUB_ARB_DRAM_STATE_CNTL: with self-refresh (stutter) allowed, DCN runs on Navi23's SR
// watermarks and the panel shows random-height grey/white/yellow/blue bands below the menu bar,
// much more often with 16 vCPUs (DRAM contention). Force SR disallowed (FORCE_ENABLE=1, VALUE=0),
// as Linux does with disable_stutter. -rmbstutter keeps DAL's value.
bool gKeepStutter;
constexpr uint32_t DramStateCntl = 0x39bc, SrForceEnable = 2, SrForceValue = 1;
constexpr uint32_t DmcubInbox1Wptr = Seg2 + 0x1d6, DmcubInbox1Rptr = Seg2 + 0x1d7, DmcubCntl = Seg2 + 0x1f6,
				   DmcubCntl2 = Seg2 + 0x200, DmcubScratch0 = Seg2 + 0x1e3, DmcubCw4OffLo = Seg2 + 0x1bd,
				   DmcubCw4OffHi = Seg2 + 0x1be;
bool inboxHeld();
extern uint32_t gDalWptr;

mach_vm_address_t orgRead, orgWrite;
thread_t gDalThread;

uint64_t wrapRead(uint64_t helper, uint64_t reg, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (r >= PgFirst && r <= PgLast) {
		uint32_t off = r - Seg2;
		if (isPgStatus(off))
			return (gPgShadow[r - 1 - PgFirst] & (1u << 8)) ? (2u << 30 | 2u << 28) : 0;
		return gPgShadow[r - PgFirst];
	}
	if ((r == DmcubInbox1Wptr || r == DmcubInbox1Rptr) && inboxHeld()) {
		uint32_t hwW = mmioRead(DmcubInbox1Wptr);
		if (r == DmcubInbox1Wptr || mmioRead(DmcubInbox1Rptr) == hwW)
			return gDalWptr;
	}
	uint32_t hw = remap(r, false);
	auxDalTouch(hw);
	if (!gKeepPhyWrites && isPhyHole(hw)) {
		static uint32_t logged;
		if (logged++ < 60 && rmbVerbose())
			LOG("phy: skipped DAL read of undefined reg %x (hw %x)", r, hw);
		return 0;
	}
	phyEnter(true, r);
	gDalThread = current_thread();
	auto v = reinterpret_cast<Fn6>(orgRead)(helper, hw, c, d, e, f);
	gDalThread = nullptr;
	if (auto k = fieldKind(hw); k != Fields::None)
		v = fieldsToDal(k, v & 0xFFFFFFFF);
	traceAccess(true, r, hw, v & 0xFFFFFFFF);
	phyExit(true, r, hw, v & 0xFFFFFFFF);
	if (r == CcDcPipeDis) {
		static bool logged;
		if (!logged) {
			logged = true;
			LOG("dcn: CC_DC_PIPE_DIS %llx -> %llx (pipe 4 absent on DCN 3.1.2)", v & 0xFFFFFFFF, (v | 0x10) & 0xFFFFFFFF);
		}
		v |= 0x10;
	}
	return v;
}

// DMCUB inbox 1: log every command DAL submits (slots between the old and new WPTR) with the
// DMCUB run state, to see what reaches the firmware and when. The ring lives at code window 4
// (VRAM offset = CW4 offset - GCMC FB base).

// Linux DCN3.x hooks enable_display_power_gating to a dummy and never sends
// DMUB_CMD__VBIOS_ENABLE_DISP_POWER_GATING; Navi23 DAL does, and RDPCSTX0 dies right after.
// Turn those commands into DMUB_CMD__NULL before the firmware sees them (-rmbpwrgate: keep them).
bool gKeepPwrGate;

// Linux dcn31_init_hw does send them via bios_golden_init (global PIPE_INIT + per-pipe DISABLE)
// when the VBIOS isn't in accelerated mode, i.e. a cold DCN after S3. GOP-trained boots don't need
// them, but after an S3 sleep the PHY died ~10 ms into wake training with them NULLed (s3wake6).
// Pass them through after a power loss until the first transmitter cmd (-rmbnocoldpg: never).
bool gDcnCold, gNoColdPg;

// Navi23 DAL's dce110_edp_power_control decides on/off from PWRSEQ_STATE, which on DCN3.1 stays "on"
// after the DMUB firmware cuts panel VDD: every link-training attempt sends LVTMA POWER_OFF (VBIOS
// sub 0xf, action 0x0d) + transmitter POWER_OFF (sub 1, action 0x0d), then skips POWER_ON, so the
// panel trains unpowered (CR 0 on all lanes). Turn the power-offs into NULL cmds (-rmbedpoff: keep).
bool gKeepEdpOff;

// With the power-offs gone the panel keeps VDD, so every later POWER_ON (sub 0xf / sub 1, action
// 0x0c) is redundant, and the firmware sometimes kills RDPCSTX0 processing one (s3wake5: display went
// offline right after the wake modeset, DISABLE then POWER_ON 530 ms later, PHY 0, DMUB wedged).
// Let one through after boot and after each S3 sleep (VDD really off then), NULL the rest
// (-rmbedpon: keep them all).
bool gKeepEdpOn;
bool gEdpPowered;
// A POWER_OFF really reached DMUB since the last passed POWER_ON. gEdpPowered alone can't say "panel
// is off": a GOP-lit boot never sends a POWER_ON (auto-1024-0745: replay skipped, shutdown lines).
bool gEdpOffDone;

// With VDD kept on, shutdown and sleep left the panel lit with no stream: vertical lines crept in
// instead of black (auto-1017-1943/2000). Between a system shutdown/sleep notice and the next eDP
// power-up, let DAL's POWER_OFF through (-rmbedpoffkeep: keep NULLing them); the following POWER_ON
// then goes through too, since the panel really is off.
bool gPanelOffOk, gKeepEdpOffAlways;

// But kIOMessageSystemWillPowerOff only arrives after "CPU halted", long after DAL's last POWER_OFF
// (sent at display sleep / WindowServer exit) was NULLed, so nothing turned the panel off and the
// lines stayed (auto-1017-2023). Keep a copy of the last dropped pair and replay it at shutdown.
uint64_t gEdpOffCmd[2][8];  // [0] = sub 0xf, [1] = sub 1
uint32_t gEdpOffSaved;
// With no display sleep before shutdown nothing was dropped, so there's no pair to replay and
// DAL's own POWER_OFF only comes after our MODE2 (DMUB stuck, lines again: auto-1024-0631).
// The passed POWER_ON pair is the same command with action 0x0c, so keep it and flip it to 0x0d.
uint64_t gEdpOnCmd[2][8];
uint32_t gEdpOnSaved;

// RDPCSTX1 (PHY B, HDMI) CNTL / PHY_CNTL0 / PHY_CNTL6 and RDPCSTX0_CNTL, for the HDMI logs.
// PHY B is never read from the CPU (see isPhyHole): logs 0xdead for it unless -rmbphybcpu.
uint32_t phyRead(uint32_t hw);
#define PHYB_REGS phyRead(Seg2 + 0x2a08), phyRead(Seg2 + 0x2a18), phyRead(Seg2 + 0x2a1e), mmioRead(Seg2 + 0x2930)

// When the stream goes away Navi23 DAL powers down every COMBO-PHY PLL (VBIOS SET_PIXEL_CLOCK, sub 2,
// pixclk 0, pll_id ATOM_COMBOPHY_PLL0-5 = 20-25). Linux DCN3.1 drives DP from the DP DTO and never
// does that; afterwards RDPCSTX0 reads 0 and the firmware hangs in the next transmitter ENABLE.
// NULL those power-downs (-rmbpllpd: keep).
// Only PLL 20 (PHY A, eDP) though: Linux does power the other PHY PLLs down (cs_power_down, FORCE
// program), and with PHY B's dropped the first HDMI SET_PIXEL_CLOCK (PLL 21) hung DMUB for good with
// the whole RDPCSTX1 block reading 0 (auto-1019-1248). -rmbpllpdall: NULL all of 20-25 again.
bool gKeepPllPd, gDropAllPllPd;
// Bare-metal Linux (logs/linux-hdmi-dmub.txt) powers PLL 20-24 down 1.5 ms before the HDMI SET_PIXEL_CLOCK
// on PLL 21; ours never reach DMUB (sent at init with no OTG on). Keep the PLL 21-25 ones and send them
// later in a slot we NULL anyway (dropped eDP POWER_ON etc.) once an OTG runs. -rmbnopllpdlate: don't.
bool gNoPllPdLate;
uint64_t gPllPdCmd[5][8];
uint32_t gPllPdPend;
struct StreamInfo {
	uint32_t pixKhz, enc, depth;
};
StreamInfo gPendStream, gExtStream;

// Navi23 DAL sends a type 0x4c cmd after the watermarks in every commit (payload 0, pixclk 100 Hz, 1).
// In the yellow_carp firmware 0x4c is DMUB_CMD__FW_ASSISTED_MCLK_SWITCH, which Linux DCN3.1 never sends.
// DMUB hung on one in auto-1017-1852, and the HDMI SET_PIXEL_CLOCK right after one hangs every time.
// NULL them (-rmbfams: keep).
bool gKeepFams;

// DMUB ABM SET_BACKLIGHT (type 0x42 sub 2): Navi23 DAL's ABM was never set up for this panel in the
// Rembrandt firmware (no ABM SET_PIPE), so program the PWM like Linux dce_driver_set_backlight and
// NULL the cmd (-rmbabm: pass it). level = u16.16 duty, 0x10000 = full; kept >= 2% so the panel
// never goes dark.
constexpr uint32_t BlPwmCntl = 0x34C0 + 0x2f19, BlPwmPeriod = BlPwmCntl + 2, BlPwmLock = BlPwmCntl + 3;

// After S3 the DMUB firmware owns PWRSEQ0 (auto-1022-1102: the whole block reads 0 and ignores CPU and
// host writes; PWRSEQ1 stays writable), the DCN3.1 way: Linux always drives the panel PWM through DMUB
// PANEL_CNTL HW_INIT (hands the saved PWM regs to the fw, which restores them) + ABM SET_BACKLIGHT.
// The regs are saved while the CPU still owned the PWM. -rmbnofwbl: just skip like before.
struct BlRegs {
	uint32_t cntl, period, refDiv1, refDiv2;
};
BlRegs gBlSaved;
bool gNoFwBl, gPanelCntlInit;
bool cpuOwnsPwm() { return mmioRead(BlPwmPeriod) & 0xffff; }
void dmubBacklight(uint32_t level);

// Correction (auto-1022-1212): PWRSEQ0 is not fw-owned, it is DEAD. It died between display sleep
// (+139 s) and the next modeset (+142 s), long before S3, with no ABM SET_PIPE passed. Then the whole
// block times out on the register bus (RBBMIF_INT_STATUS timeout addr 0x63e1), reads 0 and drops
// writes, the fw too (PANEL_CNTL replies current_backlight 0), and the PWM keeps its last duty (the 2%
// display-sleep dim -> "super dark"). Host test, VM off: one DCIO_SOFT_RESET.PWRSEQ0_SOFT_RESET pulse
// brings it back at reset defaults, then the CPU lock/write/unlock works; hole reads/writes at the Navi
// LVTMA_PWRSEQ addresses (0x5d43-0x5d47), zero REF_DIVs, CNTL2/LOCK writes do NOT kill it. So revive it
// and restore everything saved while it lived, REF_DIV1 + PANEL_PWRSEQ_CNTL (DIGON override = panel
// VDD) first. -rmbnorevive: the DMUB PANEL_CNTL path above.
constexpr uint32_t DcioSoftReset = Seg2 + 0x289e, Pwrseq0SoftReset = 1u << 16, PsCntl = Seg2 + 0x2f14,
				   PsDelay1 = PsCntl + 2, PsDelay2 = PsCntl + 3, PsRefDiv1 = PsCntl + 4, BlPwmCntl2 = BlPwmCntl + 1,
				   PsRefDiv2 = PsCntl + 9;
struct PwrseqRegs {
	uint32_t cntl, delay1, delay2, refDiv1, blCntl, blCntl2, period, lock, refDiv2;
};
PwrseqRegs gPs;
bool gNoRevive;
uint32_t gRevives;

// Alive but at reset defaults (PERIOD 1, BL_PWM_EN off) is not a state to save or to write a duty into.
bool pwrseqProgrammed(uint32_t period, uint32_t blCntl) { return (period & 0xffff) > 1 && (blCntl & 0x80000000); }

// Who kills PWRSEQ0? Every death so far came at a display off whose DMUB cmds were all NULLed, so check
// it around every CPU write (DAL, raw AmdRegisterAccess, VBIOS tables) while it lives. Only while alive:
// a dead block costs a 33 us bus timeout per read.
bool gPsAlive;
void pwrseqWatch(bool before, const char *what, uint32_t reg, uint32_t val) {
	if (!gPsAlive || (mmioRead(BlPwmPeriod) & 0xffff))
		return;
	gPsAlive = false;
	static uint32_t logged;
	if (logged++ < 6) {
		LOG("bl: PWRSEQ0 died %s %s %x = %x", before ? "BEFORE (async, not this write)" : "right after", what, reg, val);
		// 256 trace lines hold the display-off path ~6 s at the serial cap (slows every sleep); -rmbpsdump.
		static int dump = -1;
		if (dump < 0)
			dump = checkKernelArgument("-rmbpsdump");
		if (dump && logged <= 2)
			traceDump("PWRSEQ0 died");
	}
}

void savePwrseq0() {
	PwrseqRegs p = {mmioRead(PsCntl),	 mmioRead(PsDelay1),   mmioRead(PsDelay2),
					mmioRead(PsRefDiv1), mmioRead(BlPwmCntl),  mmioRead(BlPwmCntl2),
					mmioRead(BlPwmPeriod), mmioRead(BlPwmLock), mmioRead(PsRefDiv2)};
	// auto-1022-1743: after S3 the block came back by itself at reset defaults (PERIOD 1, BL_PWM_CNTL 0);
	// saving that made the next revive "restore" duty 0 -> panel lit but backlight off after wake.
	if (!pwrseqProgrammed(p.period, p.blCntl))
		return;
	gPsAlive = true;
	if (gPs.period && !(p.cntl & 0x10))  // PANEL_PWRSEQ_TARGET_STATE off: a revive must not power the panel down
		p.cntl = gPs.cntl;
	if (!gPs.period)
		LOG("bl: PWRSEQ0 saved: CNTL %x DELAY1 %x DELAY2 %x REF_DIV1 %x BL_PWM_CNTL %x CNTL2 %x PERIOD %x LOCK %x REF_DIV2 %x",
			p.cntl, p.delay1, p.delay2, p.refDiv1, p.blCntl, p.blCntl2, p.period, p.lock, p.refDiv2);
	gPs = p;
}

// auto-1022-1243: revive #10 came back at reset defaults (CNTL 0 STATE 14 PERIOD 1) with every restore
// write lost (written 2 us after the reset) -> PANEL_PWRSEQ off -> eDP retrain failed after wake, panel
// dark for good. So hold the reset longer, wait until the block answers, restore, read back and retry.
void restorePwrseq0() {
	mmioWrite(PsRefDiv1, gPs.refDiv1);
	mmioWrite(PsCntl, gPs.cntl);
	mmioWrite(PsDelay1, gPs.delay1);
	mmioWrite(PsDelay2, gPs.delay2);
	mmioWrite(PsRefDiv2, gPs.refDiv2);
	mmioWrite(BlPwmPeriod, gPs.period);
	mmioWrite(BlPwmCntl2, gPs.blCntl2);
	mmioWrite(BlPwmCntl, gPs.blCntl);
	mmioWrite(BlPwmLock, gPs.lock & ~0x101u);
}

bool revivePwrseq0(uint32_t level) {
	if (gNoRevive || !gPs.period)
		return false;
	uint32_t sr = mmioRead(DcioSoftReset);
	mmioWrite(DcioSoftReset, sr | Pwrseq0SoftReset);
	IODelay(50);
	mmioWrite(DcioSoftReset, sr & ~Pwrseq0SoftReset);
	uint32_t waitUs = 0;
	while (!(mmioRead(BlPwmPeriod) & 0xffff) && waitUs < 1000) {
		IODelay(10);
		waitUs += 10;
	}
	uint32_t tries = 0, cntl = 0, per = 0;
	if (mmioRead(BlPwmPeriod) & 0xffff) {
		for (; tries < 5; tries++) {
			if (tries)
				IODelay(100);
			restorePwrseq0();
			cntl = mmioRead(PsCntl);
			per = mmioRead(BlPwmPeriod);
			if (cntl == gPs.cntl && (per & 0xffff) == (gPs.period & 0xffff))
				break;
		}
	}
	bool ok = cntl == gPs.cntl && pwrseqProgrammed(per, mmioRead(BlPwmCntl));
	// auto-1022-1755: after revive #8 every reg was right (STATE 41f, PWM toggling on the pin, OTG + PHY
	// lit) yet the panel was black with no glow: the death dropped DIGON (panel VDD), the panel reset and
	// lost the eDP link trained before. Retrain it.
	if (ok) {
		gPsAlive = true;
		panelRetrain();
	}
	if (gRevives++ < 40)
		LOG("bl: PWRSEQ0 revived #%u for level %x (DCIO_SOFT_RESET %x, alive after %u us, %u restores): CNTL %x STATE %x "
			"BL_PWM_CNTL %x PERIOD %x LOCK %x%s",
			gRevives, level, sr, waitUs, tries + 1, mmioRead(PsCntl), mmioRead(PsCntl + 1), mmioRead(BlPwmCntl),
			mmioRead(BlPwmPeriod), mmioRead(BlPwmLock),
			ok ? "" : (mmioRead(BlPwmPeriod) & 0xffff) ? " - RESTORE FAILED" : " - STILL DEAD");
	return ok;
}

void setBacklight(uint32_t level) {
	if (level > 0x10000)
		level = 0x10000;
	if (level < 0x10000 / 50)
		level = 0x10000 / 50;
	uint32_t per = mmioRead(BlPwmPeriod), bits = per >> 16 & 0x1f, cntl = mmioRead(BlPwmCntl);
	if (!(per & 0xffff) && revivePwrseq0(level)) {
		per = mmioRead(BlPwmPeriod);
		bits = per >> 16 & 0x1f;
		cntl = mmioRead(BlPwmCntl);
	}
	if ((per & 0xffff) && !pwrseqProgrammed(per, cntl) && gPs.period && !gNoRevive) {
		restorePwrseq0();
		static uint32_t restores;
		if (restores++ < 20)
			LOG("bl: PWRSEQ0 at reset defaults (BL_PWM_CNTL %x PERIOD %x) -> restored saved regs: CNTL %x BL_PWM_CNTL %x PERIOD %x",
				cntl, per, mmioRead(PsCntl), mmioRead(BlPwmCntl), mmioRead(BlPwmPeriod));
		per = mmioRead(BlPwmPeriod);
		bits = per >> 16 & 0x1f;
		cntl = mmioRead(BlPwmCntl);
	}
	// auto-1021-1528: during a modeset both read 0 (block not readable); writing would set duty 0.
	if (!(per & 0xffff) || !(cntl & 0x80000000)) {
		// only when PERIOD is unreadable: the ring handler (inbox lock held) calls us only when it reads
		if (!gNoFwBl && gBlSaved.period && !(per & 0xffff)) {
			dmubBacklight(level);
			return;
		}
		LOG("bl: level %x skipped: BL_PWM_CNTL %x PERIOD %x unreadable (PWRSEQ0 CNTL %x REF_DIV1 %x, PWRSEQ1 BL %x, "
			"RBBMIF_SEC %x)", level, cntl, per, mmioRead(Seg2 + 0x2f14), mmioRead(Seg2 + 0x2f18), mmioRead(0x6445),
			mmioRead(Seg2 + 0x17a));
		return;
	}
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = per & ((1u << bits) - 1);
	uint32_t active = ((uint64_t)period * level >> 16) << (16 - bits);
	if (active > 0xffff)
		active = 0xffff;
	mmioWrite(BlPwmLock, mmioRead(BlPwmLock) | 1);
	mmioWrite(BlPwmCntl, (cntl & ~0xffffu) | active);
	mmioWrite(BlPwmLock, mmioRead(BlPwmLock) & ~1u);
	uint32_t n = 0;
	while ((mmioRead(BlPwmLock) & 0x100) && n++ < 1000)  // BL_PWM_GRP1_REG_UPDATE_PENDING
		IODelay(10);
	gBlSaved = {mmioRead(BlPwmCntl), per, mmioRead(Seg2 + 0x2f18), mmioRead(Seg2 + 0x2f1d)};
	savePwrseq0();
	static uint32_t logged;
	if (logged++ < 40)
		LOG("bl: level %x: BL_PWM_CNTL %x -> %x (period %x bits %u, pending polls %u)", level, cntl, mmioRead(BlPwmCntl), period,
			bits, n);
}

static bool anyOtgOn() {
	bool on = false;
	for (uint32_t i = 0; i < 4; i++)
		on |= mmioRead(Seg2 + 0x1b41 + i * 0x80) & 1;  // OTGn_OTG_CONTROL.OTG_MASTER_EN
	return on;
}

// Linux sends DMUB_CMD__CLK_MGR NOTIFY_CLOCKS (type 0x49: dispclk, dppclk kHz) before every HDMI
// SET_PIXEL_CLOCK, in both its working and its hanging enable; Navi23 DAL never sends one. auto-1019-1557:
// with the PLL 21 power-down sent in the 0-stream commit before, DMUB still hung on the pixclk. So wait
// for the commit that carries the ext stream, then use its NULLed slots for CLK_MGR first and the PLL
// power-downs after, as close to the pixclk as we have slots. -rmbnoclkmgr: no CLK_MGR.
bool gNoClkMgr;
bool gClkMgrSent;

static void enableFwTrace();

// Put CLK_MGR or one saved PLL power-down into slot s (being NULLed). True if it did.
static bool sendLatePllPd(uint64_t ring, uint32_t s) {
	if (!gExtStream.pixKhz || !anyOtgOn())
		return false;
	enableFwTrace();  // before the PLL 21 power-down: does it run the PHY B reset path (trace d0/e2) like Linux?
	if (!gNoClkMgr && !gClkMgrSent) {
		gClkMgrSent = true;
		const uint32_t cmd[4] = {0x49, 600000, 100000, 0};  // header type 0x49 sub 0, dispclk, dppclk (Linux: 640000/100000)
		for (uint32_t k = 15; k >= 1; k--)
			vramWrite32(ring + s + k * 4, k < 4 ? cmd[k] : 0);
		vramWrite32(ring + s, cmd[0]);
		LOG("dmub-cmd: CLK_MGR notify clocks (dispclk %u dppclk %u) at [%x]", cmd[1], cmd[2], s);
		return true;
	}
	if (!gPllPdPend)
		return false;
	uint32_t i = __builtin_ctz(gPllPdPend);
	gPllPdPend &= ~(1u << i);
	for (uint32_t k = 1; k < 16; k++)
		vramWrite32(ring + s + k * 4, (uint32_t)(gPllPdCmd[i][k / 2] >> (k % 2 * 32)));
	vramWrite32(ring + s, (uint32_t)gPllPdCmd[i][0]);  // header last
	LOG("dmub-cmd: late PHY PLL %u power-down at [%x] (%08llx %08llx); PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x",
		21 + i, s, gPllPdCmd[i][1] & 0xFFFFFFFF, gPllPdCmd[i][1] >> 32, PHYB_REGS);
	return true;
}


// DPCS/RDPCS PHY + DCIO UNIPHY macro registers (dpcs_4_2_0, seg2 0x2928-0x2d8e). On DCN3.1 the
// DMUB firmware owns them; Linux only reads them. Navi23 DAL writes RDPCSTX_CNTL = 0x400000 and
// DPCSTX_DEBUG_CONFIG during link-encoder init, which resets PHY A. Drop those (-rmbphywr: keep).
constexpr uint32_t PhyFirst = Seg2 + 0x2928, PhyLast = Seg2 + 0x2d8e;
bool gKeepPhyWrites;

// Navi23 DAL also READS dpcs_3_0_0 offsets that don't exist on DCN 3.1.2 (e.g. DPCSTX_DEBUG_CONFIG
// 0x292e); RDPCSTX0 dies on that read and its block then reads 0 / drops writes. Never touch them.
// UNIPHY1-4 (PHY B-E, seg2 0x2a00-0x2d8e) don't answer the CPU at all, even on bare-metal Linux while
// HDMI is lit: every read takes a 33 us bus timeout and returns 0. Our CPU reads there (DAL init,
// "now" of dropped writes, incl. UNIPHY1 RESERVED6 = PHY B's DEBUG_CONFIG hole) left PHY B dead for
// DMUB too: its PHY B steps ran 10-40x slower than Linux and SRAM_INIT_DONE never came
// (auto-1019-1647). Treat the whole block as a hole (-rmbphybcpu: old behaviour).
constexpr uint32_t PhyBFirst = Seg2 + 0x2a00;
bool gPhyBCpu;
bool isPhyHole(uint32_t hw) {
	if (hw < PhyFirst || hw > PhyLast)
		return false;
	if (hw >= PhyBFirst && !gPhyBCpu)
		return true;
	uint32_t i = hw - PhyFirst;
	return !(gPhyValid[i / 32] & (1u << (i % 32)));
}

uint32_t phyRead(uint32_t hw) { return isPhyHole(hw) ? 0xdeadu : mmioRead(hw); }

// HDMI SET_PIXEL_CLOCK (VBIOS sub 2, encoder_mode 3): Navi23 DAL (dce112_program_pix_clk) sends the
// deep-color-adjusted clock with deep_color_ratio 0 and no YUV420 misc bit; Linux dcn31_program_pix_clk
// sends the ratio and YUV420 flag too ("Make sure we send the correct color depth to DMUB for HDMI").
// DMUB hung for good on the first HDMI one (auto-1019-1231: 371.25 MHz, PLL 21, 4K60 4:2:0 10 bpc).
// Fill both in from the stream DAL committed for the non-eDP link (-rmbpixclk: leave as DAL sent it).
bool gKeepPixClk;

uint32_t hdmiClk100Hz(const StreamInfo &st) {
	uint64_t c = (uint64_t)st.pixKhz * 10;
	if (st.enc == 4)  // PIXEL_ENCODING_YCBCR420
		c /= 2;
	switch (st.depth) {  // COLOR_DEPTH_101010 / 121212 / 161616
	case 3: return (uint32_t)(c * 5 / 4);
	case 4: return (uint32_t)(c * 6 / 4);
	case 6: return (uint32_t)(c * 2);
	default: return (uint32_t)c;
	}
}

// DMUB also hangs on an HDMI SET_PIXEL_CLOCK when the target OTG's clock isn't running. Bare-metal Linux
// (2026-10-19): first HDMI enable on OTG0 still clocked from the GOP's eDP DP DTO worked (OTG_CLOCK_ON
// set, DMUB done in 20 ms); a re-enable right after KWin stopped OTG0's clock hung DMUB the same way
// as ours (OTG1 never clocked, OTG1_PIXEL_RATE_CNTL 0). So start the OTG on its DP DTO first and let
// the firmware switch it to the PHY PLL (-rmbotgclk: don't).
bool gNoOtgClk;

void startOtgClock(uint32_t crtc, uint32_t clk100Hz) {
	constexpr uint32_t Seg1 = 0xC0;
	if (gNoOtgClk || crtc > 3)
		return;
	uint32_t otgClk = Seg2 + 0x1b86 + crtc * 0x80, odmClk = Seg2 + 0x1acf + crtc * 0x10;
	uint32_t rate = Seg1 + 0x80 + crtc * 4;  // OTGn_PIXEL_RATE_CNTL, DP_DTOn_PHASE +1, _MODULO +2
	uint32_t before = mmioRead(otgClk), rateBefore = mmioRead(rate);
	// auto-1019-1415: OTG1 clock "on" (103) but PIXEL_RATE_CNTL 8000c000 = no DP/DTB DTO, i.e. fed by the
	// unstarted PHY PLL B, and DMUB still hung. Only a DTO-fed OTG clock counts as running.
	if ((before & (1u << 8)) && (rateBefore & 0x18)) {  // OTG_CLOCK_ON, DP_DTO/DTBCLK_DTO_ENABLE
		LOG("dmub-cmd: OTG%u clock already on (%x), PIXEL_RATE_CNTL %x", crtc, before, rateBefore);
		return;
	}
	uint32_t i = 0;
	if (before & 1) {  // gate the dead-sourced clock off before switching the source
		mmioWrite(otgClk, before & ~1u);
		while ((mmioRead(otgClk) & (1u << 8)) && i++ < 1000)
			IODelay(10);
	}
	uint32_t hz = clk100Hz * 100 < 600000000u ? clk100Hz * 100 : 600000000u;  // dprefclk 600 MHz
	mmioWrite(rate + 1, hz);
	mmioWrite(rate + 2, 600000000u);
	mmioWrite(rate, mmioRead(rate) | 0x10);  // DP_DTOn_ENABLE
	uint32_t j = 0;
	while (!(mmioRead(rate) & 0x80) && j++ < 1000)  // DPDTOn_ENABLE_STATUS
		IODelay(10);
	mmioWrite(odmClk, mmioRead(odmClk) | 3);  // OPTC_INPUT_CLK_GATE_DIS | _EN
	mmioWrite(otgClk, mmioRead(otgClk) | 3);  // OTG_CLOCK_EN | _GATE_DIS
	uint32_t k = 0;
	while (!(mmioRead(otgClk) & (1u << 8)) && k++ < 1000)
		IODelay(10);
	LOG("dmub-cmd: OTG%u clock started on DP DTO %u Hz: OTG_CLOCK_CONTROL %x -> %x, ODM %x, PIXEL_RATE_CNTL %x -> %x (polls off %u dto %u on %u)",
		crtc, hz, before, mmioRead(otgClk), mmioRead(odmClk), rateBefore, mmioRead(rate), i, j, k);
}

// DMUB firmware trace buffer (CW5, read by checkDmubStall): turn every trace class on before the first
// HDMI pixclk so the stall dump shows what the firmware did last. GPINT = DMCUB_GPINT_DATAIN1
// (status 1 << 28 | code << 16 | param), acked when the status bits clear. -rmbnofwtrace: don't.
bool gNoFwTrace;
constexpr uint32_t DmcubGpintDatain1 = Seg2 + 0x1f8;

static bool dmubGpint(uint32_t code, uint32_t param) {
	uint32_t v = code << 16 | param;
	mmioWrite(DmcubGpintDatain1, 1u << 28 | v);
	for (uint32_t i = 0; i < 1000; i++) {
		if (mmioRead(DmcubGpintDatain1) == v)
			return true;
		IODelay(10);
	}
	return false;
}

// yellow_carp fw 0x400004c keeps one 0x3c-byte state record per PHY at CW6 + 0x2130 (fw pointer 0x60008024 ->
// 0x66000300, + 0x1e30). The SET_PIXEL_CLOCK handler skips the RDPCS SRAM_INIT_DONE wait (0x6000d244, spins
// forever on timeout) when bit 16 of the dword at +8 is set; dump them all to compare with Linux fw_state.
static void dumpFwPhyState(const char *why) {
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t cw6 = ((uint64_t)mmioRead(Seg2 + 0x1c2) << 32 | mmioRead(Seg2 + 0x1c1)) - fbBase;
	LOG("fwstate: %s: CW6 off %llx base %x", why, cw6, mmioRead(Seg2 + 0x1ab));
	for (uint32_t phy = 0; phy < 5; phy++) {
		uint64_t r = cw6 + 0x2130 + phy * 0x3c;
		uint32_t w[15];
		for (uint32_t i = 0; i < 15; i++)
			w[i] = vramRead64(r + i * 4) & 0xFFFFFFFF;
		LOG("fwstate:   phy%u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", phy, w[0],
			w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14]);
	}
}

static uint32_t gFwTraceStartW = ~0u;  // OUTBOX0 WPTR when the mask went on (stall dump starts there)

static void enableFwTrace() {
	static bool done;
	if (done || gNoFwTrace)
		return;
	done = true;
	dumpFwPhyState("before late slots / first HDMI pixclk");
	gFwTraceStartW = mmioRead(Seg2 + 0x1da);
	bool a = dmubGpint(102, 0xFFFF), b = dmubGpint(103, 0xFFFF), c = dmubGpint(101, 0);  // MASK_WORD0/1, UPDATE
	LOG("dmub-cmd: fw trace mask all on: acks %d %d %d, SCRATCH7 %x, OUTBOX0 WPTR %x", a, b, c, mmioRead(Seg2 + 0x1ea),
		mmioRead(Seg2 + 0x1da));
}

// Navi23 DAL's clock requests never reach the Rembrandt PMFW, so dispclk/dppclk stay at the boot vote
// (600 MHz, DPPCLK DTOs 1:1). 4K120 4:2:0 (1188 MHz pixel rate) starved the pipe = green flashes on the LG
// (auto-1020-1138). Vote the ext stream's pixel rate + 1% (PMFW max 1200 MHz), never below 600
// (-rmbnodispclk: keep 600).
bool gNoDispClk;

static void voteDispClk(uint32_t pixKhz) {
	static uint32_t cur = 600;
	uint32_t mhz = (uint32_t)(((uint64_t)pixKhz * 101 + 99999) / 100000);
	mhz = mhz < 600 ? 600 : mhz > 1200 ? 1200 : mhz;
	if (gNoDispClk || mhz == cur)
		return;
	uint32_t r1 = vbiosSmcSend(0x4, mhz), g1 = mmioRead(0x16293);  // SetDispclkFreq
	uint32_t r2 = vbiosSmcSend(0x6, mhz), g2 = mmioRead(0x16293);  // SetDppclkFreq
	LOG("dmub-cmd: ext stream %u kHz: dispclk %u -> %u MHz (resp %x granted %u), dppclk (resp %x granted %u)", pixKhz,
		cur, mhz, r1, g1, r2, g2);
	cur = mhz;
}

void fixHdmiPixClk(uint64_t ring, uint32_t s, uint32_t clk) {
	uint32_t w2 = vramRead64(ring + s + 8) & 0xFFFFFFFF, w3 = vramRead64(ring + s + 8) >> 32;
	if (((w2 >> 16) & 0xFF) != 3)  // ATOM_ENCODER_MODE_HDMI
		return;
	enableFwTrace();
	startOtgClock(w3 & 0xFF, clk);
	auto &st = gExtStream;
	if (!st.pixKhz || hdmiClk100Hz(st) != clk) {
		LOG("dmub-cmd: HDMI pixclk %u at [%x] matches no ext stream (last %u kHz enc %u depth %u), left alone", clk, s,
			st.pixKhz, st.enc, st.depth);
		return;
	}
	voteDispClk(st.pixKhz);
	uint32_t ratio = st.depth == 3 ? 1 : st.depth == 4 ? 2 : st.depth == 6 ? 3 : 0;
	uint32_t n2 = w2 | (st.enc == 4 ? 0x04u << 24 : 0), n3 = (w3 & ~0xFF00u) | ratio << 8;
	if (gKeepPixClk) {
		LOG("dmub-cmd: HDMI pixclk %u at [%x] enc %u depth %u (kept: %08x %08x)", clk, s, st.enc, st.depth, w2, w3);
		return;
	}
	vramWrite32(ring + s + 8, n2);
	vramWrite32(ring + s + 12, n3);
	LOG("dmub-cmd: HDMI pixclk %u at [%x] enc %u depth %u: %08x %08x -> %08x %08x", clk, s, st.enc, st.depth, w2, w3, n2, n3);
	LOG("dmub-cmd:   before: PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x", PHYB_REGS);
	if (gPllPdPend)
		LOG("dmub-cmd:   late PLL power-downs never sent (mask %x)", gPllPdPend);
}

// Returns the number of non-NULL commands in DAL's batch [old, newWptr).
uint32_t dropPowerGating(uint32_t old, uint32_t newWptr) {
	if (gKeepPwrGate && gKeepEdpOff && gKeepPllPd && gKeepPixClk && gKeepFams && gKeepAbm)
		return (newWptr - old) % 0x2000 / 64;
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	// Who kills PWRSEQ0 (see revivePwrseq0)? Check it before every DAL batch and name the previous one.
	static uint64_t prevCmds[8];
	static uint32_t prevN, prevOld, prevNew, deaths;
	static bool wasAlive;
	bool alive = cpuOwnsPwm();
	if (wasAlive && !alive && deaths++ < 20) {
		LOG("bl: PWRSEQ0 died before DAL batch [%x, %x); previous batch [%x, %x) had %u cmds:", old, newWptr, prevOld,
			prevNew, prevN);
		for (uint32_t i = 0; i < prevN && i < 8; i++)
			LOG("bl:   cmd %u: %016llx", i, prevCmds[i]);
	}
	if (alive)
		savePwrseq0();
	wasAlive = alive;
	prevOld = old;
	prevNew = newWptr;
	prevN = 0;
	uint32_t real = 0;
	for (uint32_t s = old; s != newWptr; s = (s + 64) % 0x2000) {
		uint32_t hdr = vramRead64(ring + s) & 0xFFFFFFFF;
		uint64_t q = vramRead64(ring + s);
		if (prevN < 8)
			prevCmds[prevN] = q;
		prevN++;
		bool edpOff = !gKeepEdpOff && (((hdr & 0xFFFF) == 0x0f80 && ((q >> 32) & 0xFF) == 0x0d) ||
									   ((hdr & 0xFFFF) == 0x0180 && ((q >> 40) & 0xFF) == 0x0d));
		bool edpOn = ((hdr & 0xFFFF) == 0x0f80 && ((q >> 32) & 0xFF) == 0x0c) ||
					 ((hdr & 0xFFFF) == 0x0180 && ((q >> 40) & 0xFF) == 0x0c);
		if (edpOn && !gKeepEdpOn && !gKeepEdpOff) {
			if (gEdpPowered) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedOn;
				if (loggedOn++ < 20)
					LOG("dmub-cmd: dropped eDP POWER_ON (sub %x) at [%x] payload %08llx, panel kept powered", (hdr >> 8) & 0xFF, s,
						q >> 32);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
			uint32_t k = (hdr & 0xFFFF) == 0x0f80 ? 0 : 1;
			for (uint32_t i = 0; i < 8; i++)
				gEdpOnCmd[k][i] = vramRead64(ring + s + i * 8);
			gEdpOnSaved |= 1u << k;
			if ((hdr & 0xFFFF) == 0x0180)  // the LVTMA one (sub 0xf) comes first; both make up one power-up
				gEdpPowered = true;
			gEdpOffDone = false;
			gPanelOffOk = false;
		}
		if (!gKeepFams && (hdr & 0xFF) == 0x4c) {
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t loggedFams;
			if (loggedFams++ < 20)
				LOG("dmub-cmd: dropped type 4c (FAMS) at [%x] payload %08llx %08llx", s, q >> 32, vramRead64(ring + s + 8));
			if (sendLatePllPd(ring, s))
				real++;
			continue;
		}
		if ((hdr & 0xFF) == 0x42) {
			static uint32_t loggedAbm;
			if (loggedAbm++ < 40)
				LOG("bl: DMUB ABM sub %x at [%x] payload %08llx %08llx%s", (hdr >> 8) & 0xFF, s, q >> 32, vramRead64(ring + s + 8),
					gKeepAbm ? " (passed)" : "");
			// No revive here: this runs inside DAL's display power-down/up sequence (auto-1022-1243 #10).
			bool fwPwm = !gNoFwBl && !cpuOwnsPwm();
			if (fwPwm && ((hdr & 0xFFFF) == 0x0242 || (hdr & 0xFFFF) == 0x0142)) {
				static uint32_t loggedFw;
				if (loggedFw++ < 10)
					LOG("bl: passed ABM sub %x at [%x] (firmware owns the PWM)", (hdr >> 8) & 0xFF, s);
			} else if (!gKeepAbm && (hdr & 0xFFFF) == 0x0242) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				setBacklight(vramRead64(ring + s + 8) & 0xFFFFFFFF);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
			// ABM SET_PIPE (sub 1) hands the panel to the firmware: from then on the whole PWRSEQ0 block
			// reads 0 and setBacklight can't work (auto-1021-1540: OK at +100 s, SET_PIPE at +101 s;
			// auto-1021-1621: the 0xff disable alone passed and still 0 ever after). Drop every option.
			uint32_t pipeOpt = (q >> 48) & 0xFF;
			if (!fwPwm && !gKeepAbm && (hdr & 0xFFFF) == 0x0142) {
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedPipe;
				if (loggedPipe++ < 10)
					LOG("bl: dropped ABM SET_PIPE option %x at [%x], PWM stays with the CPU", pipeOpt, s);
				if (sendLatePllPd(ring, s))
					real++;
				continue;
			}
		}
		if ((hdr & 0xFFFF) == 0x0280 && (q >> 32) != 0)
			fixHdmiPixClk(ring, s, q >> 32);
		if (!gKeepPllPd && (hdr & 0xFFFF) == 0x0280 && (q >> 32) == 0) {
			uint32_t pll = vramRead64(ring + s + 8) & 0xFF;
			// Waking DMCUB with every OTG off kills PHY A (see the hold below), so only while one runs.
			bool otgOn = anyOtgOn();
			if (pll == 20 || !otgOn || (gDropAllPllPd && pll >= 20 && pll <= 25)) {
				if (!gNoPllPdLate && !gDropAllPllPd && pll > 20 && pll <= 25) {
					for (uint32_t k = 0; k < 8; k++)
						gPllPdCmd[pll - 21][k] = vramRead64(ring + s + k * 8);
					gPllPdPend |= 1u << (pll - 21);
				}
				vramWrite32(ring + s, hdr & ~0xFFFFu);
				static uint32_t loggedPll;
				if (loggedPll++ < 20)
					LOG("dmub-cmd: dropped PHY PLL %u power-down at [%x]%s", pll, s, otgOn ? "" : " (no OTG on)");
				continue;
			}
			if (pll > 20 && pll <= 25) {
				static uint32_t loggedPass;
				if (loggedPass++ < 20)
					LOG("dmub-cmd: passed PHY PLL %u power-down at [%x] (%08llx %08llx); PHY B CNTL %x PHY_CNTL0 %x PHY_CNTL6 %x, PHY A CNTL %x",
						pll, s, vramRead64(ring + s + 8) & 0xFFFFFFFF, vramRead64(ring + s + 8) >> 32, PHYB_REGS);
			}
		}
		if (edpOff && gPanelOffOk) {
			gEdpPowered = false;
			gEdpOffDone = true;
			LOG("dmub-cmd: passed eDP POWER_OFF (sub %x) at [%x] payload %08llx (shutdown/sleep)", (hdr >> 8) & 0xFF, s, q >> 32);
			real++;
			continue;
		}
		if (edpOff) {
			uint32_t k = (hdr & 0xFFFF) == 0x0f80 ? 0 : 1;
			for (uint32_t i = 0; i < 8; i++)
				gEdpOffCmd[k][i] = vramRead64(ring + s + i * 8);
			gEdpOffSaved |= 1u << k;
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t loggedOff;
			if (loggedOff++ < 20)
				LOG("dmub-cmd: dropped eDP POWER_OFF (sub %x) at [%x] payload %08llx", (hdr >> 8) & 0xFF, s, q >> 32);
			continue;
		}
		if ((hdr & 0xFFFF) == 0x0180 && gDcnCold) {
			gDcnCold = false;
			LOG("dmub-cmd: first transmitter cmd after power loss at [%x], PG filter back on", s);
		}
		if (!gKeepPwrGate && (hdr & 0xFFFF) == 0x0380 && gDcnCold) {
			LOG("dmub-cmd: passed ENABLE_DISP_POWER_GATING at [%x] payload %08llx (cold DCN)", s, q >> 32);
		} else if (!gKeepPwrGate && (hdr & 0xFFFF) == 0x0380) {
			vramWrite32(ring + s, hdr & ~0xFFFFu);
			static uint32_t logged;
			if (logged++ < 20)
				LOG("dmub-cmd: dropped ENABLE_DISP_POWER_GATING at [%x] payload %08llx", s, vramRead64(ring + s) >> 32);
			continue;
		}
		if (hdr & 0xFF)
			real++;
	}
	return real;
}

// The PHY still dies while the firmware is processing one of the NULLed PLL power-downs in
// commit_streams (odmsrc2 at [940], raw1/dcc1 at [980]): with every OTG off, waking DMCUB at all
// lets it power PHY A down. So don't ring the doorbell for NULL-only batches: keep the hardware
// WPTR where it is, remember DAL's, and report DAL's WPTR as RPTR once the firmware has caught up.
// The held NULLs go out with the next real command. -rmbnohold: always submit.
bool gNoHold;
uint32_t gDalWptr = ~0u;

bool inboxHeld() { return gDalWptr != ~0u && gDalWptr != mmioRead(DmcubInbox1Wptr); }

// Our own DMUB commands (dmubInject). DAL's dmub_srv keeps its WPTR in software, so it can't learn
// about them: held (hardware WPTR behind DAL's, the slots between are all NULL) → the cmd takes the
// first held slot; else it goes at the hardware WPTR and the hardware runs gAhead slots ahead of
// DAL. DAL's next batch then starts on top of those consumed slots: realignInbox moves its real cmds
// up behind them (wrapRead keeps reporting DAL's own WPTR). gInboxLock serializes with DAL's WPTR
// writes; DMCUB reload resets the state (inboxReset).
IOLock *gInboxLock;
uint32_t gAhead;
uint64_t gRealign[128][8];

uint64_t inboxRing() {
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	return ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
}

void inboxResetState() {
	gAhead = 0;
	gPanelCntlInit = false;
}

bool dmubInject(const uint32_t *cmd, uint32_t *reply) {
	if (!gInboxLock || !mmioAvailable())
		return false;
	IOLockLock(gInboxLock);
	uint64_t ring = inboxRing();
	uint32_t w = mmioRead(DmcubInbox1Wptr), nw = (w + 64) % 0x2000;
	bool held = !gAhead && inboxHeld();
	for (uint32_t i = 0; i < 16; i++)
		vramWrite32(ring + w + i * 4, cmd[i]);
	if (!held) {
		if (gDalWptr == ~0u)
			gDalWptr = w;
		gAhead++;
	}
	mmioWrite(DmcubInbox1Wptr, nw);
	uint32_t ms = 0;
	while (mmioRead(DmcubInbox1Rptr) != nw && ms < 100) {
		IODelay(1000);
		ms++;
	}
	bool ok = mmioRead(DmcubInbox1Rptr) == nw;
	if (reply)
		for (uint32_t i = 0; i < 16; i += 2) {
			uint64_t q = vramRead64(ring + w + i * 4);
			reply[i] = q & 0xFFFFFFFF;
			reply[i + 1] = q >> 32;
		}
	static uint32_t logged;
	if (logged++ < 20 || !ok)
		LOG("dmub-inject: type %x sub %x at [%x]%s, rptr %x after %u ms, ahead %u", cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF, w,
			held ? " (held slot)" : "", mmioRead(DmcubInbox1Rptr), ms, gAhead);
	IOLockUnlock(gInboxLock);
	return ok;
}

// Panel PWM owned by the fw (see gBlSaved): PANEL_CNTL HW_INIT with the saved PERIOD/REF_DIVs and a
// BL_PWM_CNTL duty computed for this level, on every set (the fw writes the regs it is given). auto-1022-1140:
// one HW_INIT with the saved (display-sleep dimmed, 2%) duty + ABM SET_BACKLIGHT 0x75ae left the panel at 2%
// = SET_BACKLIGHT does nothing without ABM INIT_CONFIG (Linux sends one, Navi DAL doesn't).
// -rmbfwabmset: also send ABM SET_BACKLIGHT (Linux dmub_abm_set_backlight: ramp 0, level u16.16, version 1,
// panel mask 1).
bool gFwAbmSet;
void dmubBacklight(uint32_t level) {
	uint32_t c[16] = {}, rep[16];
	uint32_t bits = gBlSaved.period >> 16 & 0x1f;
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = gBlSaved.period & ((1u << bits) - 1);
	uint32_t active = ((uint64_t)period * level >> 16) << (16 - bits);
	if (active > 0xffff)
		active = 0xffff;
	uint32_t cntl = (gBlSaved.cntl & ~0xffffu) | active | 0x80000000;
	c[0] = 0x2000004a;  // DMUB_CMD__PANEL_CNTL, HW_INIT, 32 payload bytes
	c[3] = cntl;
	c[4] = gBlSaved.period;
	c[5] = gBlSaved.refDiv1;
	c[6] = 3;  // is_backlight_on | is_powered_on
	c[7] = gBlSaved.refDiv2;
	bool ok = dmubInject(c, rep);
	if (ok)
		gBlSaved.cntl = cntl;
	static uint32_t logged;
	if (logged++ < 40 || !ok || !gPanelCntlInit)
		LOG("bl: level %x via panel-cntl HW_INIT %s: sent cntl %x period %x ref_div1 %x ref_div2 %x; reply cur %x cntl %x period %x "
			"ref_div1 %x flags %x",
			level, ok ? "ok" : "FAILED", c[3], c[4], c[5], c[7], rep[2], rep[3], rep[4], rep[5], rep[6] & 0xFF);
	gPanelCntlInit = true;
	if (!gFwAbmSet)
		return;
	for (uint32_t &x : c)
		x = 0;
	c[0] = 0x20000242;  // DMUB_CMD__ABM, SET_BACKLIGHT
	c[2] = level;
	c[3] = 0x101;
	ok = dmubInject(c, nullptr);
	if (logged < 40 || !ok)
		LOG("bl: level %x via DMUB ABM SET_BACKLIGHT%s", level, ok ? "" : " FAILED");
}

// DAL wrote WPTR = v for its batch [gDalWptr, v) (already filtered in place), while the hardware is
// gAhead slots further. Copy the batch's real cmds to the hardware WPTR; pad with NULLs up to v if
// that ends behind it. Returns the hardware WPTR to write, or ~0u for "nothing to submit".
uint32_t realignInbox(uint32_t v) {
	uint64_t ring = inboxRing();
	uint32_t p = mmioRead(DmcubInbox1Wptr), n = 0, ahead0 = gAhead;
	for (uint32_t s = gDalWptr; s != v && n < 128; s = (s + 64) % 0x2000) {
		if (!(vramRead64(ring + s) & 0xFF))
			continue;
		for (uint32_t k = 0; k < 8; k++)
			gRealign[n][k] = vramRead64(ring + s + k * 8);
		n++;
	}
	uint32_t q = p;
	for (uint32_t i = 0; i < n; i++, q = (q + 64) % 0x2000)
		for (uint32_t k = 0; k < 16; k++)
			vramWrite32(ring + q + k * 4, (gRealign[i][k / 2] >> (k % 2 * 32)) & 0xFFFFFFFF);
	uint32_t ahead = (q - v + 0x2000) % 0x2000;
	if (ahead < 0x1000) {  // still at or past DAL's WPTR
		gAhead = ahead / 64;
	} else if (!n) {  // behind: only NULLs, which [q, v) already are → plain held state
		gAhead = 0;
	} else {
		for (; q != v; q = (q + 64) % 0x2000)
			for (uint32_t k = 0; k < 16; k++)
				vramWrite32(ring + q + k * 4, 0);
		gAhead = 0;
	}
	static uint32_t logged;
	if (logged++ < 20)
		LOG("dmub-inject: realigned DAL batch [%x, %x) (%u real) to hw [%x, %x), ahead %u -> %u", gDalWptr, v, n, p, q, ahead0,
			gAhead);
	return n ? q : ~0u;
}

// After the first DMUB transmitter ENABLE (VBIOS type 0x80 sub 1, action byte 1), log the next DAL
// register writes: shows how Navi23 DAL programs DIG/DP/DCCG around link training.
uint32_t gWriteLogLeft;

void logInboxWrite(uint32_t newWptr) {
	static uint32_t lines;
	static bool armed;
	uint32_t old = mmioRead(DmcubInbox1Wptr);
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	if (lines >= 400)
		return;
	LOG("dmub-cmd: wptr %x -> %x (rptr %x) CNTL %x CNTL2 %x SCRATCH0 %x", old, newWptr, mmioRead(DmcubInbox1Rptr),
		mmioRead(DmcubCntl), mmioRead(DmcubCntl2), mmioRead(DmcubScratch0));
	lines++;
	for (uint32_t s = old; s != newWptr && lines < 400; s = (s + 64) % 0x2000, lines++) {
		uint64_t a = vramRead64(ring + s), b = vramRead64(ring + s + 8);
		if (!armed && mmioRead(DmcubCntl2) == 0 && (a & 0xFFFF) == 0x0180 && ((a >> 40) & 0xFF) == 1) {
			armed = true;
			gWriteLogLeft = 800;
			LOG("wlog: transmitter ENABLE at [%x], logging next %u DAL writes", s, gWriteLogLeft);
		}
		LOG("dmub-cmd:   [%x] type %llx sub %llx payload %08llx %08llx %08llx", s, a & 0xFF, (a >> 8) & 0xFF, a >> 32,
			b & 0xFFFFFFFF, b >> 32);
	}
}


// DAL waits for DMUB idle after each command, so a new submission while RPTR still trails the
// hardware WPTR means the firmware is stuck on an earlier one. Once per boot: dump the last DAL
// register accesses and the PHY A/B, DCCG, DIG1 and DMCUB state, to see what it is waiting for.
void checkDmubStall() {
	static bool dumped;
	static uint32_t lastRp = ~0u;
	static uint64_t sinceUs;
	uint32_t rp = mmioRead(DmcubInbox1Rptr), wp = mmioRead(DmcubInbox1Wptr);
	if (dumped || rp == wp)
		return;
	if (rp != lastRp) {  // only a RPTR that hasn't moved for 200 ms counts (NO_WAIT cmds can trail)
		lastRp = rp;
		sinceUs = nowUs();
		return;
	}
	if (nowUs() - sinceUs < 200000)
		return;
	dumped = true;
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	uint64_t a = vramRead64(ring + rp), b = vramRead64(ring + rp + 8);
	LOG("stall: DMUB stuck at [%x] (wptr %x) type %llx sub %llx payload %08llx %08llx %08llx", rp, wp, a & 0xFF, (a >> 8) & 0xFF,
		a >> 32, b & 0xFFFFFFFF, b >> 32);
	traceDump("DMUB stalled");
	constexpr uint32_t Seg1 = 0xC0;
	static const struct {
		const char *name;
		uint32_t reg;
	} regs[] = {
		{"DMCUB_CNTL", Seg2 + 0x1f6}, {"DMCUB_SCRATCH1", Seg2 + 0x1e4}, {"DMCUB_SCRATCH4", Seg2 + 0x1e7},
		{"DMCUB_TIMER_CURRENT", Seg2 + 0x1fd}, {"DMCUB_INST_FETCH_FAULT_ADDR", Seg2 + 0x1cc},
		{"DMCUB_DATA_WRITE_FAULT_ADDR", Seg2 + 0x1cd}, {"PHYASYMCLK_CLOCK_CNTL", Seg2 + 0x52},
		{"PHYBSYMCLK_CLOCK_CNTL", Seg2 + 0x53}, {"DCCG_GATE_DISABLE_CNTL", Seg1 + 0x74}, {"DCCG_GATE_DISABLE_CNTL2", Seg1 + 0x7c},
		{"OTG1_PIXEL_RATE_CNTL", Seg1 + 0x84}, {"SYMCLKB_CLOCK_ENABLE", Seg1 + 0xa1}, {"DIO_LINKB_CNTL", Seg2 + 0x1f05},
		{"DIG1_DIG_FE_CNTL", Seg2 + 0x218b}, {"DIG1_HDMI_CONTROL", Seg2 + 0x2193}, {"DIG1_DIG_BE_CNTL", Seg2 + 0x21b0},
		{"DOMAIN0_PG_STATUS", Seg2 + 0x81}, {"DOMAIN1_PG_STATUS", Seg2 + 0x83}, {"DOMAIN2_PG_STATUS", Seg2 + 0x85},
		{"DOMAIN3_PG_STATUS", Seg2 + 0x87},
	};
	for (auto &e : regs)
		LOG("stall:   %s (%x) = %x", e.name, e.reg, mmioRead(e.reg));
	for (uint32_t i = 0; i < 16; i += 4)
		LOG("stall:   SCRATCH%u-%u = %x %x %x %x", i, i + 3, mmioRead(Seg2 + 0x1e3 + i), mmioRead(Seg2 + 0x1e4 + i),
			mmioRead(Seg2 + 0x1e5 + i), mmioRead(Seg2 + 0x1e6 + i));
	// Trace buffer: CW5 = 16-byte header (entry_count) + ring of {code, tick, param0, param1}; OUTBOX0 = that ring.
	uint64_t tb = ((uint64_t)mmioRead(Seg2 + 0x1c0) << 32 | mmioRead(Seg2 + 0x1bf)) - fbBase;
	uint32_t cw5Size = (mmioRead(Seg2 + 0x1b2) & 0x1FFFFFFF) - mmioRead(Seg2 + 0x1aa), obSize = mmioRead(Seg2 + 0x1d9),
			 obW = mmioRead(Seg2 + 0x1da), count = vramRead64(tb) & 0xFFFFFFFF;
	LOG("stall:   CW5 off %llx size %x (TOP raw %x) OUTBOX0 base %x size %x wptr %x rptr %x, header %llx %llx", tb,
		cw5Size, mmioRead(Seg2 + 0x1b2), mmioRead(Seg2 + 0x1d8), obSize, obW, mmioRead(Seg2 + 0x1db), vramRead64(tb),
		vramRead64(tb + 8));
	uint32_t maxEnt = obSize >= 16 && obSize < 0x1000000 ? obSize / 16 : 0;
	if (maxEnt) {
		uint32_t last = obW / 16;  // next entry to be written
		uint32_t n = gFwTraceStartW == ~0u ? 48 : (obW + obSize - gFwTraceStartW) % obSize / 16;
		if (n > 256 || n == 0)
			n = n ? 256 : 48;
		for (uint32_t k = n; k >= 1; k--) {
			uint32_t idx = (last + maxEnt - k) % maxEnt;
			uint64_t lo = vramRead64(tb + 16 + idx * 16), hi = vramRead64(tb + 16 + idx * 16 + 8);
			if (lo || hi)
				LOG("stall:   trace[%u] code %08llx tick %08llx p0 %08llx p1 %08llx", idx, lo & 0xFFFFFFFF, lo >> 32,
					hi & 0xFFFFFFFF, hi >> 32);
		}
		LOG("stall:   (header entry_count %u)", count);
	}
	dumpFwPhyState("stall");
	static const uint32_t phyBlocks[] = {Seg2 + 0x2928, Seg2 + 0x2a00};  // RDPCSTX0 / RDPCSTX1
	for (uint32_t base : phyBlocks)
		for (uint32_t r = base; r < base + 0x48; r++)
			if (!isPhyHole(r))
				LOG("stall:   phy %x = %x", r, mmioRead(r));
}

uint64_t wrapWrite(uint64_t helper, uint64_t reg, uint64_t val, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (r >= PgFirst && r <= PgLast) {
		gPgShadow[r - PgFirst] = val & 0xFFFFFFFF;
		forceDomainsOn();
		return 0;
	}
	bool inboxLocked = false;
	if (r == DmcubInbox1Wptr) {
		if (gInboxLock) {
			IOLockLock(gInboxLock);
			inboxLocked = true;
		}
		checkDmubStall();
		uint32_t v = val & 0xFFFFFFFF;
		uint32_t real = dropPowerGating(gAhead ? gDalWptr : mmioRead(DmcubInbox1Wptr), v);
		if (gAhead) {
			uint32_t hwW = realignInbox(v);
			gDalWptr = v;
			if (hwW != ~0u)
				mmioWrite(DmcubInbox1Wptr, hwW);
			IOLockUnlock(gInboxLock);
			return 0;
		}
		gDalWptr = v;
		if (!gNoHold && real == 0) {
			static uint32_t logged;
			if (logged++ < 20)
				LOG("dmub-cmd: held NULL-only batch, DAL wptr %x, hw wptr %x", gDalWptr, mmioRead(DmcubInbox1Wptr));
			if (inboxLocked)
				IOLockUnlock(gInboxLock);
			return 0;
		}
		if (rmbVerbose())
			logInboxWrite(v);
	}
	uint32_t hw = remap(r, true);
	auxDalTouch(hw);
	if (hw >= BlPwmCntl && hw <= BlPwmLock) {
		static uint32_t loggedBlWr;
		if (loggedBlWr++ < 60)
			LOG("bl: DAL write %x = %llx (PWRSEQ0 %s)", hw, val & 0xFFFFFFFF, cpuOwnsPwm() ? "alive" : "dead");
	}
	if (!gKeepPhyWrites && hw >= PhyFirst && hw <= PhyLast) {
		static uint32_t logged;
		if (logged++ < 60 && rmbVerbose())
			LOG("phy: dropped DAL write reg %x (hw %x) = %llx, now %x", r, hw, val & 0xFFFFFFFF,
				isPhyHole(hw) ? 0xdeadu : mmioRead(hw));
		return 0;
	}
	if (gWriteLogLeft) {
		gWriteLogLeft--;
		auto m = find(r);
		LOG("wlog: %x (hw %x)%s%s = %llx (was %x)", r, hw, m ? " " : "", m ? m->name : "", val & 0xFFFFFFFF, mmioRead(hw));
	}
	if (auto k = fieldKind(hw); k != Fields::None)
		val = fieldsToHw(k, hw, val & 0xFFFFFFFF, mmioRead(hw));
	if (!gKeepDto && isDtoModulo(hw) && (val & 0xFFFFFFFF)) {
		uint32_t m = static_cast<uint32_t>((val & 0xFFFFFFFF) * RmbDtoRefKhz / NaviDtoRefKhz);
		static uint32_t logged;
		if (logged++ < 20)
			LOG("dcn: DP DTO modulo %x: %llu -> %u", hw, val & 0xFFFFFFFF, m);
		val = m;
	}
	if (!gKeepStutter && hw == DramStateCntl) {
		static uint32_t logged;
		if (logged++ < 5)
			LOG("dcn: self-refresh disallowed (DAL wrote %llx)", val & 0xFFFFFFFF);
		val = (val & ~static_cast<uint64_t>(SrForceValue)) | SrForceEnable;
	}
	if (!gNoAperture && isSurfAddr(hw))
		fixAperture((hw - SurfAddr0) / 0xdc);
	if (!gNoDet && isSurfAddr(hw))
		fixDet((hw - SurfAddr0) / 0xdc);
	if (!gKeepDcc && isSurfCtl(hw) && (val & SurfCtlDcc)) {
		static uint32_t logged;
		if (logged++ < 20)
			LOG("dcn: cleared DCC in surface control %x: %llx", hw, val & 0xFFFFFFFF);
		val &= ~static_cast<uint64_t>(SurfCtlDcc);
	}
	phyEnter(false, r);
	traceAccess(false, r, hw, val & 0xFFFFFFFF);
	gDalThread = current_thread();
	pwrseqWatch(true, "DAL write", hw, val & 0xFFFFFFFF);
	auto ret = reinterpret_cast<Fn6>(orgWrite)(helper, hw, val, d, e, f);
	pwrseqWatch(false, "DAL write", hw, val & 0xFFFFFFFF);
	gDalThread = nullptr;
	if (inboxLocked)
		IOLockUnlock(gInboxLock);
	phyExit(false, r, hw, val & 0xFFFFFFFF);
	return ret;
}

// Everything else in the framebuffer (IRQ/HPD handlers, link services, ...) reaches MMIO through
// AmdRegisterAccess::hwReadReg32 / readValidateReg32 / writeReg32 directly, with Navi23 offsets and
// no remap. PHY A still dies asynchronously during commit_streams (odmsrc2, dcc1) with no DAL access
// at fault; guard those paths too: never touch PHY holes, never write the PHY block, and check
// RDPCSTX0 around each access. Log the caller as an offset in the extracted kext (__TEXT 0xbfe6000).
constexpr mach_vm_address_t FbTextBase = 0xbfe6000;
mach_vm_address_t gFbText, orgHwRead, orgValRead, orgRawWrite;

uint32_t callerOff(void *ra) { return static_cast<uint32_t>(reinterpret_cast<mach_vm_address_t>(ra) - gFbText + FbTextBase); }

// AmdRadeonControllerNavi2::setupBootWatermarks copies DCHUBBUB watermark set A to B/C/D with raw
// DCN 3.0.2 offsets (on 3.1.2 they hit other watermarks and DCHUBBUB_TEST_DEBUG_INDEX/DATA): remap.
// Located by symbol (15.0 0xbfea1d2, 15.8.1 at another offset); same 0x36e-byte body in both.
constexpr uint32_t BootWmSize = 0x36e;
uint32_t gBootWmFirst, gBootWmLast;
bool isNaviRawCaller(uint32_t caller) { return gBootWmFirst && caller >= gBootWmFirst && caller <= gBootWmLast; }

bool isDisplayReg(uint32_t r) { return (r >= 0xc0 && r < 0x1260) || (r >= Seg2 && r < 0x8000); }

void logRawAccess(bool rd, uint32_t r, uint32_t val, uint32_t caller) {
	static uint64_t seen[256];
	static uint32_t n;
	uint64_t key = (uint64_t)r << 32 | caller;
	for (uint32_t i = 0; i < n; i++)
		if (seen[i] == key)
			return;
	if (n >= 256)
		return;
	seen[n++] = key;
	auto m = find(r);
	LOG("raw: %s %x%s%s = %x from fb+%x", rd ? "rd" : "wr", r, m ? " " : "", m ? m->name : "", val, caller);
}

bool rawBlocked(bool rd, uint32_t r, uint32_t caller) {
	if (gKeepPhyWrites || !(rd ? isPhyHole(r) : (r >= PhyFirst && r <= PhyLast)))
		return false;
	static uint32_t logged;
	if (logged++ < 60)
		LOG("phy: skipped non-DAL %s of reg %x from fb+%x", rd ? "read" : "write", r, caller);
	return true;
}

void rawPhyCheck(bool before, bool rd, uint32_t r, uint32_t val, uint32_t caller) {
	if (!phyPollOn())
		return;
	bool alive = mmioRead(PhyA) != 0;
	static uint32_t logged;
	if (gPhyAlive && !alive && logged++ < 5) {
		LOG("phy: RDPCSTX0 died %s non-DAL %s reg %x val %x from fb+%x", before ? "before" : "right after", rd ? "read" : "write", r,
			val, caller);
		traceDump("died around a non-DAL access");
	}
	gPhyAlive = alive;
}

template <mach_vm_address_t &Org>
uint64_t wrapRawRead(uint64_t self, uint64_t reg) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (gDalThread == current_thread() || !isDisplayReg(r))
		return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(Org)(self, reg);
	uint32_t caller = callerOff(__builtin_return_address(0));
	if (isNaviRawCaller(caller))
		reg = r = remap(r, false);
	if (rawBlocked(true, r, caller))
		return 0;
	rawPhyCheck(true, true, r, 0, caller);
	uint64_t v = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(Org)(self, reg);
	traceAccess(true, r | 0x80000000, caller, v & 0xFFFFFFFF);
	rawPhyCheck(false, true, r, v & 0xFFFFFFFF, caller);
	logRawAccess(true, r, v & 0xFFFFFFFF, caller);
	return v;
}

uint64_t wrapRawWrite(uint64_t self, uint64_t reg, uint64_t val) {
	uint32_t r = reg & 0xFFFFFFFF;
	if (gDalThread == current_thread() || !isDisplayReg(r))
		return reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(orgRawWrite)(self, reg, val);
	uint32_t caller = callerOff(__builtin_return_address(0));
	if (isNaviRawCaller(caller))
		reg = r = remap(r, true);
	if (rawBlocked(false, r, caller))
		return 0;
	rawPhyCheck(true, false, r, val & 0xFFFFFFFF, caller);
	traceAccess(false, r | 0x80000000, caller, val & 0xFFFFFFFF);
	pwrseqWatch(true, "raw write", r, val & 0xFFFFFFFF);
	auto ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(orgRawWrite)(self, reg, val);
	pwrseqWatch(false, "raw write", r, val & 0xFFFFFFFF);
	rawPhyCheck(false, false, r, val & 0xFFFFFFFF, caller);
	logRawAccess(false, r, val & 0xFFFFFFFF, caller);
	return ret;
}

// VBIOS command tables DAL runs through the CPU AtomBIOS interpreter (MMIO not seen by wrapWrite).
// Navi23 dcn10_link_encoder_hw_init sends DIG1TransmitterControl INIT; Linux DCN31 never does.
mach_vm_address_t orgExecTable;
bool gKeepTxInit;
constexpr uint32_t Dig1TransmitterControl = 77, TxActionInit = 7;

uint64_t wrapExecTable(uint64_t helper, uint64_t index, uint64_t params, uint64_t d, uint64_t e, uint64_t f) {
	uint32_t idx = index & 0xFFFFFFFF;
	auto p = reinterpret_cast<const uint8_t *>(params);
	uint32_t p0 = p ? *reinterpret_cast<const uint32_t *>(p) : 0, p1 = p ? *reinterpret_cast<const uint32_t *>(p + 4) : 0,
			 p2 = p ? *reinterpret_cast<const uint32_t *>(p + 8) : 0;
	if (!gKeepTxInit && idx == Dig1TransmitterControl && p && p[1] == TxActionInit) {
		LOG("atom: skipped table %u (DIG1TransmitterControl INIT) params %08x %08x %08x, PHY %x", idx, p0, p1, p2, phyPeek());
		return 1;
	}
	uint32_t before = phyPeek();
	pwrseqWatch(true, "atom table", idx, p0);
	auto ret = reinterpret_cast<Fn6>(orgExecTable)(helper, index, params, d, e, f);
	pwrseqWatch(false, "atom table", idx, p0);
	LOG("atom: table %u params %08x %08x %08x = %llx, PHY %x -> %x", idx, p0, p1, p2, ret & 0xFF, before, phyPeek());
	return ret;
}

}  // namespace

void edpAllowPowerOff(const char *why) {
	if (gKeepEdpOffAlways || gKeepEdpOff)
		return;
	gPanelOffOk = why != nullptr;
	LOG("dmub-cmd: eDP POWER_OFF %s (%s)", why ? "allowed" : "blocked again", why ? why : "powered on");
}

void edpPowerOffNow(const char *why) {
	if (gKeepEdpOffAlways || gKeepEdpOff)
		return;
	if (gEdpOffDone) {
		LOG("dmub-cmd: eDP already powered off, no replay at %s", why);
		return;
	}
	bool synth = gEdpOffSaved != 3;
	if (synth) {
		if (gEdpOnSaved != 3) {
			LOG("dmub-cmd: no eDP POWER_OFF/ON pair saved, can't power the panel off at %s", why);
			return;
		}
		for (uint32_t k = 0; k < 2; k++)
			for (uint32_t i = 0; i < 8; i++)
				gEdpOffCmd[k][i] = gEdpOnCmd[k][i];
		gEdpOffCmd[0][0] = (gEdpOffCmd[0][0] & ~(0xFFull << 32)) | 0x0dull << 32;  // LVTMA action
		gEdpOffCmd[1][0] = (gEdpOffCmd[1][0] & ~(0xFFull << 40)) | 0x0dull << 40;  // transmitter action
	}
	uint64_t fbBase = (uint64_t)(mmioRead(0x1260 + 0x16fc) & 0xFFFFFF) << 24;
	uint64_t ring = ((uint64_t)mmioRead(DmcubCw4OffHi) << 32 | mmioRead(DmcubCw4OffLo)) - fbBase;
	uint32_t hwW = mmioRead(DmcubInbox1Wptr), w = gDalWptr != ~0u && !gAhead ? gDalWptr : hwW;
	for (uint32_t k = 0; k < 2; k++, w = (w + 64) % 0x2000)
		for (uint32_t i = 0; i < 16; i++)
			vramWrite32(ring + w + i * 4, (gEdpOffCmd[k][i / 2] >> (i % 2 * 32)) & 0xFFFFFFFF);
	mmioWrite(DmcubInbox1Wptr, w);
	if (gAhead)
		gAhead += 2;
	else
		gDalWptr = w;
	uint32_t ms = 0;
	while (mmioRead(DmcubInbox1Rptr) != w && ms < 500) {
		IODelay(1000);
		ms++;
	}
	gEdpPowered = false;
	gEdpOffDone = true;
	LOG("dmub-cmd: replayed eDP POWER_OFF%s at %s: wptr %x -> %x, rptr %x after %u ms", synth ? " (made from POWER_ON)" : "",
		why, hwW, w, mmioRead(DmcubInbox1Rptr), ms);
}

void inboxReset() { inboxResetState(); }

void edpPanelPowerLost() {
	gEdpPowered = false;
	gDcnCold = !gNoColdPg;
}

void traceWrite(uint32_t reg, uint32_t hw, uint32_t val) {
	traceAccess(false, reg, hw, val);
}

void traceDump(const char *why) {
	uint64_t now = nowUs();
	uint32_t n = gTraceN, first = n > TraceLen ? n - TraceLen : 0;
	LOG("trace: %s; last %u of %u DAL accesses (t = us before now):", why, n - first, n);
	for (uint32_t i = first; i < n; i++) {
		auto &e = gTrace[i % TraceLen];
		if (e.reg & 0x80000000) {
			auto m = find(e.reg & 0x7FFFFFFF);
			LOG("trace:   -%llu us %s %x%s%s = %x NON-DAL from fb+%x", now - e.t, e.rd ? "rd" : "wr", e.reg & 0x7FFFFFFF, m ? " " : "",
				m ? m->name : "", e.val, e.hw);
			continue;
		}
		auto m = find(e.reg);
		LOG("trace:   -%llu us %s %x (hw %x)%s%s = %x", now - e.t, e.rd ? "rd" : "wr", e.reg, e.hw, m ? " " : "", m ? m->name : "",
			e.val);
	}
	LOG("trace: DMUB inbox1 rptr %x wptr %x CNTL %x SCRATCH0 %x; MP1 C2PMSG 66/82/90 %x %x %x 67/83/91 %x %x %x 74/51/52 %x %x %x",
		mmioRead(DmcubInbox1Rptr), mmioRead(DmcubInbox1Wptr), mmioRead(DmcubCntl), mmioRead(DmcubScratch0),
		mmioRead(0x16282), mmioRead(0x16292), mmioRead(0x1629a), mmioRead(0x16283), mmioRead(0x16293), mmioRead(0x1629b),
		mmioRead(0x1628a), mmioRead(0x16273), mmioRead(0x16274));
}

namespace {

// DAL's generic_reg_wait (static C, no symbol): log the waits that time out, with the caller's
// function name and line. Located by offset from the kext's __TEXT (0xbfe6000 in the extracted
// binary), checked against its prologue bytes before routing.
using RegWaitFn = void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, unsigned, unsigned, const char *, int);
mach_vm_address_t orgRegWait;
void wrapRegWait(void *ctx, uint32_t addr, uint32_t shift, uint32_t mask, uint32_t value, unsigned delay, unsigned tries,
	const char *fn, int line) {
	reinterpret_cast<RegWaitFn>(orgRegWait)(ctx, addr, shift, mask, value, delay, tries, fn, line);
	if (addr >= PgFirst && addr <= PgLast)
		return;  // shadowed: DAL saw the state it asked for
	auto m = find(addr);
	uint32_t real = m ? m->to : addr;
	uint32_t v = mmioRead(real);
	if (((v & mask) >> shift) != value) {
		static uint32_t logged;
		if (logged++ < 40)
			LOG("dcn: REG_WAIT timeout %s:%d reg %x%s%s (hw %x) = %x, want field(mask %x) == %x", fn ? fn : "?", line, addr,
				m ? " " : "", m ? m->name : "", real, v, mask, value);
	}
}

// DAL's dm_logger_write(logger, major, fmt, ...) drops messages whose major isn't enabled in the
// logger mask; print everything (first 6000 lines) to serial instead.
void wrapDalLog(void *, int major, const char *fmt, ...) {
	static uint32_t lines;
	if (!fmt)
		return;
	// dc_commit_streams logs each stream as "pix_clk_khz: .., pixelencoder:.., displaycolorDepth:.."
	// then "link: N"; keep the last non-eDP one for fixHdmiPixClk.
	bool streamLine = strstr(fmt, "pixelencoder") != nullptr, linkLine = !streamLine && strstr(fmt, "link: %d") != nullptr;
	if (!streamLine && !linkLine && (lines >= 6000 || !rmbVerbose()))
		return;
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (streamLine) {
		auto num = [&](const char *key) -> uint32_t {
			const char *p = strstr(buf, key);
			return p ? (uint32_t)strtoul(p + strlen(key), nullptr, 10) : 0;
		};
		gPendStream = {num("pix_clk_khz: "), num("pixelencoder:"), num("displaycolorDepth:")};
	} else if (linkLine) {
		const char *p = strstr(buf, "link: ");
		if (p && strtoul(p + 6, nullptr, 10) != 0 && gPendStream.pixKhz)
			gExtStream = gPendStream;
	}
	if (lines >= 6000 || !rmbVerbose())
		return;
	size_t n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	lines++;
	LOG("dal[%d] %s", major, buf);
}

// AmdLogger: writeLog checks level (+0x30) >= severity and the type bit in mask (+0x28) inline.
// Open both up after init (framebuffer/link bring-up debugging).
mach_vm_address_t orgLoggerInit;
// -rmbamdlog: force Apple's AMD logger to all levels/categories. Off by default: it prints 2 serial
// lines per cursor move ([CUR:0] saveCursorPosition), and serial printf is synchronous -> laggy mouse.
bool gAmdLogAll;

uint64_t wrapLoggerInit(uint64_t logger, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgLoggerInit)(logger, b, c, d, e, f);
	if (!gAmdLogAll)
		return r;
	LOG("amdlog: logger %llx level %d mask %llx -> all", logger, *reinterpret_cast<int *>(logger + 0x30),
		*reinterpret_cast<uint64_t *>(logger + 0x28));
	*reinterpret_cast<int *>(logger + 0x30) = 100;
	*reinterpret_cast<uint64_t *>(logger + 0x28) = ~0ULL;
	return r;
}

// AmdLink::detectLink(reason, DetectedLinkStatus *): log result and the status it filled.
mach_vm_address_t orgDetectLink;
uint64_t wrapDetectLink(uint64_t link, uint64_t reason, uint64_t status, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(orgDetectLink)(link, reason, status, d, e, f);
	static uint32_t logged;
	if (logged++ < 24) {
		auto q = reinterpret_cast<const uint32_t *>(status);
		LOG("link %llx detect(reason %llu) = %llx status %08x %08x %08x %08x %08x %08x %08x %08x", link, reason & 0xFFFFFFFF,
			r, status ? q[0] : 0, status ? q[1] : 0, status ? q[2] : 0, status ? q[3] : 0, status ? q[4] : 0, status ? q[5] : 0,
			status ? q[6] : 0, status ? q[7] : 0);
	}
	return r;
}

#define DTRACE(name)                                                                              \
	mach_vm_address_t org_##name;                                                                  \
	uint64_t wrap_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
		LOG(">> " #name "(%llx)", b);                                                              \
		auto r = reinterpret_cast<Fn6>(org_##name)(a, b, c, d, e, f);                             \
		LOG("<< " #name "(%llx) = %llx", b, r);                                                    \
		return r;                                                                                  \
	}
DTRACE(detectAllDisplays)
DTRACE(detectDisplayOnLink)

// establishBootDisplay adopts the firmware's boot display (start-up mode from the GOP/NVRAM);
// with no AMD GOP in the VM it returns NotFound and no framebuffer gets the eDP. Treat the eDP
// like a hotplug instead: detectDisplays(mask of link 0), the path used for displays attached
// after boot.
mach_vm_address_t org_establishBootDisplay, gGetLink, gResetConnected;
DTRACE(detectDisplays)

// Boot detection (during power-up, controller not yet enabled) sends link 0's connect event too
// early. Later, forget link 0's cached state (+0x494 connected, +0x4a0 1 KB EDID) so a hotplug-style
// detectDisplays sees a new connection and sends the event again.
// displaypolicyd (the AGDC policy engine that assigns links to framebuffers) polls
// AppleGraphicsDeviceControl::sWindowServerReady for 20 s, then exits and is respawned. WindowServer
// sets it only once it has a working display, which it never gets here, so set it ourselves.
// Offset from the extracted kext: sWindowServerReady 0x13e23914 - __TEXT 0x13e1d000.
mach_vm_address_t gAgdcBase;
constexpr mach_vm_address_t AgdcWsReady = 0x6914;

void replayConnect(uint64_t ctrl) {
	IOSleep(15000);
	if (gAgdcBase) {
		auto ready = reinterpret_cast<volatile int32_t *>(gAgdcBase + AgdcWsReady);
		LOG("replay: AGDC sWindowServerReady %d -> 1", *ready);
		if (*ready == 0)
			*ready = 1;
		IOSleep(40000);  // let displaypolicyd respawn, see it and start its policy engine
	}
	auto link = reinterpret_cast<Fn6>(gGetLink)(ctrl, 0, 0, 0, 0, 0);
	LOG("replay: link0 %llx cached connected %u, new %u", link, link ? *reinterpret_cast<uint8_t *>(link + 0x494) : 0,
		link ? *reinterpret_cast<uint8_t *>(link + 0x54) : 0);
	if (!link)
		return;
	// Since earlyOpen, link 0 is already connected here. detectDisplays re-detects EVERY link, and
	// re-detecting the HDMI link swapped its sink under WindowServer's validateDetailedTiming:
	// auto-1020-1205 panicked (stream from a sink with link NULL). Replay only if still needed
	// (`-rmbreplayall` = always, old behaviour).
	if (*reinterpret_cast<uint8_t *>(link + 0x494) && !checkKernelArgument("-rmbreplayall")) {
		LOG("replay: link0 already connected, skipped");
		return;
	}
	*reinterpret_cast<uint8_t *>(link + 0x494) = 0;
	memset(reinterpret_cast<void *>(link + 0x4a0), 0, 0x400);
	auto d2 = reinterpret_cast<Fn6>(org_detectDisplays)(ctrl, 1, 0, 0, 0, 0);
	LOG("replay: detectDisplays(link 0) = %llx", d2);
}
uint64_t gCtrl;
uint64_t wrap_establishBootDisplay(uint64_t ctrl, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	auto r = reinterpret_cast<Fn6>(org_establishBootDisplay)(ctrl, b, c, d, e, f);
	LOG("establishBootDisplay = %llx", r & 0xFFFFFFFF);
	if ((r & 0xFFFFFFFF) == 0xe00002f0 && org_detectDisplays && gGetLink) {
		gCtrl = ctrl;
		thread_t t;
		kernel_thread_start([](void *, wait_result_t) { replayConnect(gCtrl); }, nullptr, &t);
		thread_deallocate(t);
	}
	return r;
}

// After S3 wake macOS lights the panel, sleeps the display 1-2 s later, then WindowServer reprobes
// and sets the mode again. auto-1022-1243/1313: WS reprobed but never set a mode on the panel (no
// IOFBSetDisplayModeAndDepth, FB:0 stayed 1 x 1) while powerd said display on and HID input arrived
// -> panel dark for good. Good wakes (1212, 1257) differ only in WS taking the main display offline
// first. Watch for that state after a wake: display wrangler on but no OTG running for 8 s -> replay
// a link 0 connect like at boot, so WS sees a hotplug and sets a mode (up to 2 times, 120 s window;
// -rmbnowakewatch: off).
}  // namespace
bool gWakeWatchArmed;
void panelWakeWatch() {
	if (!gWakeWatchArmed || !gCtrl || !gGetLink || !org_detectDisplays || checkKernelArgument("-rmbnowakewatch"))
		return;
	gWakeWatchArmed = false;
	thread_t t;
	kernel_thread_start(
		[](void *, wait_result_t) {
			auto wrangler = IOService::copyMatchingService(IOService::serviceMatching("IODisplayWrangler"));
			LOG("wakewatch: started (wrangler %s)", wrangler ? "found" : "MISSING");
			// auto-1024-0453: replay #1 returned at once without detecting (DAL only powered up), #2 two
			// seconds later gave the hotplug -> dark from 4 s on, and a no-op replay is redone after 1 s.
			constexpr uint32_t DarkLimit = 4;
			uint32_t darkSecs = 0, replays = 0, noops = 0;
			for (uint32_t s = 0; wrangler && s < 120 && replays < 2; s++) {
				IOSleep(1000);
				auto ps = wrangler->getPowerState();
				bool otg = anyOtgOn();
				darkSecs = (ps >= 3 && !otg) ? darkSecs + 1 : 0;
				if (darkSecs < DarkLimit)
					continue;
				auto link = reinterpret_cast<Fn6>(gGetLink)(gCtrl, 0, 0, 0, 0, 0);
				if (!link)
					break;
				replays++;
				LOG("wakewatch: +%u s display on (wrangler %u) but no OTG for %u s -> replay link 0 connect #%u", s + 1,
				    (unsigned)ps, darkSecs, replays);
				*reinterpret_cast<uint8_t *>(link + 0x494) = 0;
				memset(reinterpret_cast<void *>(link + 0x4a0), 0, 0x400);
				uint64_t t0 = nowUs();
				auto d = reinterpret_cast<Fn6>(org_detectDisplays)(gCtrl, 1, 0, 0, 0, 0);
				uint64_t us = nowUs() - t0;
				bool noop = us < 100000 && noops < 5;
				LOG("wakewatch: detectDisplays(link 0) = %llx in %llu ms%s", d & 0xFFFFFFFF, us / 1000,
				    noop ? " (no detect ran?) -> again in 1 s" : "");
				if (noop) {
					noops++;
					replays--;
					darkSecs = DarkLimit - 1;
					continue;
				}
				darkSecs = 0;
			}
			LOG("wakewatch: done, %u replays, OTG %s", replays, anyOtgOn() ? "on" : "off");
			OSSafeReleaseNULL(wrangler);
		},
		nullptr, &t);
	thread_deallocate(t);
}
// auto-1023-1814: after wake the panel was black with no glow while the whole GPU side was lit (OTG0,
// HUBP0 fetching, PHY 28ff0f0, PWRSEQ0 STATE 41f, DP0 4 lanes TRAINING_COMPLETE, video stream on). Host
// AUX read of the panel: DPCD 100/101 = 06 01 (reset defaults), 202 = 00 (no lane lock), 600 = 00 -> the
// sink reset after DAL trained it (at the PWRSEQ0 deaths) and nobody retrained it; the old retrain here
// (link 0 connect replay) got detectDisplays = NotFound and did nothing. A host-side CPU link training
// (Linux dce_aux.c AUX sequence + dcn10 link encoder training pattern regs) lit the panel again. So watch
// the eDP sink's lane status every 2 s and retrain it from the CPU when it is lost, at the link rate and
// lane count the sink had while good (else its max caps). DAL's own AUX0 accesses take gAuxLock too, and
// we start only when DAL left AUX0 alone for 200 ms. Log "edplink:"; -rmbnolinkwatch = off.
constexpr uint32_t Aux0Control = Seg2 + 0x1f50, Aux0SwControl = Aux0Control + 1, Aux0Arb = Aux0Control + 2,
				   Aux0IntCtl = Aux0Control + 3, Aux0SwStatus = Aux0Control + 4, Aux0SwData = Aux0Control + 6,
				   Aux0Last = Aux0Control + 0x1b, Dp0LinkCntl = Seg2 + 0x2108, Dp0Config = Seg2 + 0x210b,
				   Dp0VidStream = Seg2 + 0x210c, Dp0DphyCntl = Seg2 + 0x2117, Dp0TrainPat = Seg2 + 0x2118,
				   Dp0PrbsCntl = Seg2 + 0x211d, Otg0Control = Seg2 + 0x1b41;
constexpr uint32_t AuxIndexWr = 0x80000000u, AuxIndex = 0x1F0000u, AuxDataMask = 0xFF00u, AuxRead = 1u;
IOLock *gAuxLock;
uint64_t gAuxDalUs;
bool gEdpLinkPause;
volatile bool gEdpLinkKick;

void auxDalTouch(uint32_t hw) {
	if (hw < Aux0Control || hw > Aux0Last || !gAuxLock)
		return;
	IOLockLock(gAuxLock);
	gAuxDalUs = nowUs();
	IOLockUnlock(gAuxLock);
}

// One native AUX transaction on AUX0 (cmd 0x80 write / 0x90 read, n <= 16). Returns bytes replied, -1 no
// reply/timeout/HPD low, -2 NACK/DEFER.
int auxXfer(uint8_t cmd, uint32_t addr, uint8_t *buf, uint32_t n) {
	bool write = cmd == 0x80;
	mmioWrite(Aux0IntCtl, mmioRead(Aux0IntCtl) | 2);  // AUX_SW_DONE_ACK
	for (int i = 0; i < 100 && (mmioRead(Aux0SwStatus) & 1); i++)
		IODelay(10);
	mmioWrite(Aux0SwControl, (mmioRead(Aux0SwControl) & ~0x1F00F1u) | (4 + (write ? n : 0)) << 16);  // WR_BYTES
	uint32_t base = mmioRead(Aux0SwData) & ~(AuxIndexWr | AuxIndex | AuxDataMask | AuxRead);
	mmioWrite(Aux0SwData, base | AuxIndexWr | (uint32_t)(cmd | ((addr >> 16) & 0xf)) << 8);  // index 0
	uint8_t hdr[3] = {uint8_t(addr >> 8), uint8_t(addr), uint8_t(n - 1)};
	for (auto b : hdr)
		mmioWrite(Aux0SwData, base | uint32_t(b) << 8);
	for (uint32_t i = 0; write && i < n; i++)
		mmioWrite(Aux0SwData, base | uint32_t(buf[i]) << 8);
	mmioWrite(Aux0SwControl, mmioRead(Aux0SwControl) | 1);  // AUX_SW_GO
	uint32_t st = 0;
	for (int i = 0; i < 1000 && !((st = mmioRead(Aux0SwStatus)) & 1); i++)
		IODelay(10);
	uint32_t cnt = (st >> 24) & 0x1f;
	if (!(st & 1) || (st & 0x200) || !cnt)
		return -1;
	mmioWrite(Aux0SwData, base);  // the Linux read setup, one field at a time: index 0, autoinc off, read
	mmioWrite(Aux0SwData, base | AuxIndexWr);
	mmioWrite(Aux0SwData, base | AuxIndexWr | AuxRead);
	if ((mmioRead(Aux0SwData) >> 8 & 0xff) >> 4)
		return -2;
	cnt--;
	for (uint32_t i = 0; i < cnt; i++) {
		uint8_t b = mmioRead(Aux0SwData) >> 8;
		if (!write && i < n)
			buf[i] = b;
	}
	return cnt;
}
int dpcdRead(uint32_t a, uint8_t *b, uint32_t n) { return auxXfer(0x90, a, b, n); }
template <uint32_t N> int dpcdWrite(uint32_t a, const uint8_t (&d)[N]) {
	uint8_t b[N];
	memcpy(b, d, N);
	return auxXfer(0x80, a, b, N);
}

// All of the first nl lanes CR + EQ + symbol locked and interlane aligned (DPCD 202-204).
bool lanesOk(const uint8_t *st, uint32_t nl, uint8_t bits) {
	for (uint32_t i = 0; i < nl; i++)
		if (((st[i / 2] >> (4 * (i & 1))) & bits) != bits)
			return false;
	return bits != 7 || (st[2] & 1);
}

void dp0Pattern(uint32_t pat) {
	mmioWrite(Dp0TrainPat, pat);
	mmioWrite(Dp0LinkCntl, mmioRead(Dp0LinkCntl) & ~0x10u);	   // DP_LINK_TRAINING_COMPLETE
	mmioWrite(Dp0DphyCntl, mmioRead(Dp0DphyCntl) & ~0x10000u);  // DPHY_BYPASS
	mmioWrite(Dp0PrbsCntl, mmioRead(Dp0PrbsCntl) & ~1u);		   // DPHY_PRBS_EN
}

// Host test (scripts/edp-train.py): CR locked on try 2 with the sink's requested levels written to the
// sink only (the PHY keeps DAL's levels), EQ on try 1.
bool edpTrain(uint8_t bw, uint8_t lanes, uint32_t nl, uint32_t &crTries, uint32_t &eqTries, uint8_t *st) {
	dpcdWrite(0x600, (const uint8_t[]){1});  // SET_POWER D0
	IODelay(2000);
	dpcdWrite(0x100, (const uint8_t[]){bw, lanes});
	dpcdWrite(0x108, (const uint8_t[]){1});  // 8b/10b
	dp0Pattern(0);			// TPS1
	uint8_t set[4] = {};
	bool ok = false;
	for (crTries = 1; crTries <= 5 && !ok; crTries++) {
		dpcdWrite(0x102, (const uint8_t[]){0x21, set[0], set[1], set[2], set[3]});
		IODelay(1000);
		if (dpcdRead(0x202, st, 6) != 6)
			break;
		ok = lanesOk(st, nl, 1);
		uint32_t adj = st[4] | st[5] << 8;
		for (uint32_t i = 0; i < 4; i++)
			set[i] = ((adj >> (4 * i)) & 3) | (((adj >> (4 * i + 2)) & 3) << 3);
	}
	crTries--;
	bool eq = false;
	if (ok) {
		dp0Pattern(1);  // TPS2
		for (eqTries = 1; eqTries <= 5 && !eq; eqTries++) {
			dpcdWrite(0x102, (const uint8_t[]){0x22, set[0], set[1], set[2], set[3]});
			IOSleep(20);
			if (dpcdRead(0x202, st, 6) != 6)
				break;
			eq = lanesOk(st, nl, 7);
			uint32_t adj = st[4] | st[5] << 8;
			for (uint32_t i = 0; i < 4; i++)
				set[i] = ((adj >> (4 * i)) & 3) | (((adj >> (4 * i + 2)) & 3) << 3);
		}
		eqTries--;
	}
	dpcdWrite(0x102, (const uint8_t[]){0});
	mmioWrite(Dp0LinkCntl, mmioRead(Dp0LinkCntl) | 0x10u);  // back to video
	return eq;
}

// One check; with gAuxLock held and AUX0 acquired.
void edpLinkCheck() {
	static uint8_t goodBw, goodLanes;
	static uint32_t fails, retrains, logged;
	static uint64_t nextTryUs;
	uint32_t nl = (mmioRead(Dp0Config) & 3) + 1;
	uint8_t st[6] = {}, cfg[2] = {}, pwr = 0;
	if (int r = dpcdRead(0x202, st, 3); r != 3) {
		static uint32_t auxFails;
		if (auxFails++ < 10)
			LOG("edplink: DPCD 202 read failed (%d, AUX_SW_STATUS %x)", r, mmioRead(Aux0SwStatus));
		return;
	}
	if (lanesOk(st, nl, 7)) {
		if (!goodBw && dpcdRead(0x100, cfg, 2) == 2 && cfg[0] && (cfg[1] & 0x1f) == nl) {
			goodBw = cfg[0];
			goodLanes = cfg[1];
			LOG("edplink: watching eDP link, sink bw %x lanes %x status %02x %02x %02x", goodBw, goodLanes, st[0], st[1], st[2]);
		}
		fails = 0;
		return;
	}
	uint64_t now = nowUs();
	if (now < nextTryUs)
		return;
	dpcdRead(0x100, cfg, 2);
	dpcdRead(0x600, &pwr, 1);
	uint8_t bw = goodBw, lanes = goodLanes;
	if (!bw) {
		uint8_t caps[2] = {};
		dpcdRead(0x001, caps, 2);
		bw = caps[0];
		lanes = uint8_t(nl | (caps[1] & 0x80));
	}
	uint8_t before[3] = {st[0], st[1], st[2]};
	uint32_t cr = 0, eq = 0;
	bool ok = bw && edpTrain(bw, lanes, nl, cr, eq, st);
	retrains++;
	fails = ok ? 0 : fails + 1;
	nextTryUs = now + (fails >= 3 ? 30000000 : 0);  // keep failing -> every 30 s
	if (logged++ < 40)
		LOG("edplink: lost (202 %02x %02x %02x, sink 100 %02x %02x, 600 %02x) -> retrain #%u bw %x lanes %x: CR tries %u EQ "
			"tries %u, now %02x %02x %02x %s",
			before[0], before[1], before[2], cfg[0], cfg[1], pwr, retrains, bw, lanes, cr, eq, st[0], st[1], st[2],
			ok ? "OK" : "FAILED");
}

void edpLinkWatch() {
	for (;;) {
		for (int i = 0; i < 20 && !gEdpLinkKick; i++)
			IOSleep(100);
		gEdpLinkKick = false;
		if (gEdpLinkPause || !mmioAvailable())
			continue;
		if (!(mmioRead(Otg0Control) & 1) || !(mmioRead(Dp0VidStream) & 1))
			continue;  // eDP off
		// auto-1024-0546: after a wake DAL left DP_LINK_TRAINING_COMPLETE clear with the stream on and the sink
		// reset -> the old gate on it skipped every check (panel dark). DAL training is AUX traffic, so with
		// the bit clear just want a longer DAL AUX quiet time.
		bool trained = mmioRead(Dp0LinkCntl) & 0x10;
		IOLockLock(gAuxLock);
		uint32_t arb = mmioRead(Aux0Arb);
		if (nowUs() - gAuxDalUs >= (trained ? 200000 : 1000000) && ((arb >> 2) & 3) != 2 && (mmioRead(Aux0Control) & 1)) {
			static bool loggedUntrained;
			if (!trained && !loggedUntrained) {
				loggedUntrained = true;
				LOG("edplink: DP0 TRAINING_COMPLETE clear with stream on (LINK_CNTL %x) -> checking sink anyway", mmioRead(Dp0LinkCntl));
			}
			mmioWrite(Aux0Arb, arb | 0x10000u);  // AUX_SW_USE_AUX_REG_REQ
			if (((mmioRead(Aux0Arb) >> 2) & 3) == 1)
				edpLinkCheck();
			mmioWrite(Aux0Arb, (mmioRead(Aux0Arb) & ~0x10000u) | 0x20000u);  // AUX_SW_DONE_USING_AUX_REG
		}
		IOLockUnlock(gAuxLock);
	}
}

void edpLinkWatchStart() {
	if (gAuxLock || checkKernelArgument("-rmbnolinkwatch"))
		return;
	gAuxLock = IOLockAlloc();
	thread_t t;
	if (kernel_thread_start([](void *, wait_result_t) { edpLinkWatch(); }, nullptr, &t) == KERN_SUCCESS)
		thread_deallocate(t);
	LOG("edplink: watchdog started");
}

// After a PWRSEQ0 revive the panel had lost VDD: check the link now.
void panelRetrain() { gEdpLinkKick = true; }

namespace {

// WindowServer waits up to 60 s in GPUWranglerWaitForPublish until the GPU has its AGDC nub
// (AmdAgdcServices). AMD creates it in AmdRadeonController::powerUp (createAgdc), which only runs on
// the first IOFramebuffer::open. With the svga fb ignored (ignore-ndrv) nothing opens the AMD fb
// before WindowServer, which is the one waiting: auto-1017-2146 WS timed out at 78 s, AGDC came at
// 87 s, panel + login only after that. Open framebuffer 0 ourselves once the accelerator is up, as
// WindowServer did at ~26 s while it still had the svga display (`-rmbnoopen` = off).
// AMDRadeonX6000_AmdRadeonFramebuffer::open(), by symbol (moves between macOS builds). Its
// IOFramebuffer::open() vtable slot (303 on 15.0) is found by scanning, in case IOGraphics grew.
mach_vm_address_t gFbOpen;
constexpr size_t FbOpenSlot = 303, FbOpenSlotMin = 200, FbOpenSlotMax = 420;

void earlyOpen(void *, wait_result_t) {
	constexpr uint64_t Timeout = 120ULL * 1000 * 1000 * 1000;
	auto matching = IOService::serviceMatching("AMDRadeonX6000_AMDNavi23GraphicsAccelerator");
	auto accel = matching ? IOService::waitForMatchingService(matching, Timeout) : nullptr;
	OSSafeReleaseNULL(matching);
	if (!accel) {
		LOG("open: no accelerator, not opening fb");
		return;
	}
	accel->release();
	IOSleep(1000);
	IOService *fb0 = nullptr;
	matching = IOService::serviceMatching("AMDRadeonX6000_AmdRadeonFramebuffer");
	auto iter = matching ? IOService::getMatchingServices(matching) : nullptr;
	OSSafeReleaseNULL(matching);
	while (auto svc = iter ? OSDynamicCast(IOService, iter->getNextObject()) : nullptr) {
		auto loc = svc->getLocation();
		if (loc && !strcmp(loc, "0")) {
			fb0 = svc;
			fb0->retain();
			break;
		}
	}
	OSSafeReleaseNULL(iter);
	if (!fb0) {
		LOG("open: framebuffer 0 not found");
		return;
	}
	auto vt = *reinterpret_cast<mach_vm_address_t **>(fb0);
	size_t slot = 0;
	for (size_t i = FbOpenSlotMin; gFbOpen && i <= FbOpenSlotMax && !slot; i++)
		if (vt[i] == gFbOpen)
			slot = i;
	if (!slot) {
		LOG("open: framebuffer open %llx not in vtable slots %zu-%zu (slot %zu = %llx), not opening", gFbOpen,
		    FbOpenSlotMin, FbOpenSlotMax, FbOpenSlot, vt[FbOpenSlot]);
	} else {
		LOG("open: opening framebuffer 0 before WindowServer (vtable slot %zu)", slot);
		safeModesLog();
		auto r = reinterpret_cast<IOReturn (*)(IOService *)>(vt[slot])(fb0);
		LOG("open: framebuffer 0 open = %x", r);
	}
	fb0->release();
}

// Hardware video (VCN) hangs the whole GPU: Rembrandt's VCN is power gated (our SMU shim ignores
// PowerPlay's power-up request) and Navi's MMHUB 2.0 VM regs (MMVM_CONTEXT2_*, INVALIDATE_ENG6) are
// other regs on MMHUB 2.3, so the VMPT job's invalidate-ack poll never ends -> SDMA0_PAGE hang ->
// resetHardware -> host death (App Store videos auto-1024-1711/1332, AirPlay 1238). Hide the codec
// properties VideoToolbox looks for so it decodes/encodes in software (`-rmbvcn` = keep them).
bool isVideoKey(const char *k) {
	return !strncmp(k, "IOGVA", 5) || !strcmp(k, "IOVARendererID") || !strcmp(k, "IOVARendererSubID") ||
	       !strcmp(k, "IODVDBundleName");
}

unsigned stripVideoProps(IORegistryEntry *accel) {
	unsigned n = 0;
	auto props = accel->dictionaryWithProperties();
	auto iter = props ? OSCollectionIterator::withCollection(props) : nullptr;
	while (auto key = iter ? OSDynamicCast(OSSymbol, iter->getNextObject()) : nullptr) {
		if (isVideoKey(key->getCStringNoCopy())) {
			accel->removeProperty(key);
			n++;
		}
	}
	OSSafeReleaseNULL(iter);
	OSSafeReleaseNULL(props);
	return n;
}

void noVideoEngine(void *, wait_result_t) {
	constexpr uint64_t Timeout = 120ULL * 1000 * 1000 * 1000;
	auto matching = IOService::serviceMatching("AMDRadeonX6000_AMDNavi23GraphicsAccelerator");
	auto accel = matching ? IOService::waitForMatchingService(matching, Timeout) : nullptr;
	OSSafeReleaseNULL(matching);
	if (!accel) {
		LOG("novcn: no accelerator");
		return;
	}
	LOG("novcn: hid %u hardware video properties (VideoToolbox uses software codecs)", stripVideoProps(accel));
	// The driver may publish more at runtime (engine powerUp); keep them off for 3 min.
	for (int i = 0; i < 180; i++) {
		IOSleep(1000);
		if (auto n = stripVideoProps(accel))
			LOG("novcn: hid %u more video properties at +%d s", n, i + 1);
	}
	accel->release();
}

// Brightness: AMD's fb has no 'brgt' display parameter ('parm' = 'pscn', an unused 0 slot, then
// IOFramebuffer's list), so AppleBacklightDisplay got no slider and DAL never sent ABM SET_BACKLIGHT
// (auto-1021-1459). For the panel (fb@0) put 'brgt' in the 0 slot and serve it from BL_PWM
// (`-rmbnobrgt` = off).
constexpr uint32_t AttrBrgt = 'brgt', AttrParm = 'parm';
constexpr uint32_t BrgtMax = 1024;
using GetAttrFn = IOReturn (*)(IOService *, int32_t, uint32_t, uintptr_t *);
using SetAttrFn = IOReturn (*)(IOService *, int32_t, uint32_t, uintptr_t);
mach_vm_address_t orgGetAttr, orgSetAttr;
uint32_t gBrgt = ~0u;

bool isPanelFb(IOService *fb) {
	static IOService *panel;
	if (panel)
		return fb == panel;
	auto loc = fb->getLocation();
	if (loc && !strcmp(loc, "0")) {
		panel = fb;
		return true;
	}
	return false;
}

uint32_t currentBrightness() {
	if (gBrgt <= BrgtMax)
		return gBrgt;
	uint32_t per = mmioRead(BlPwmPeriod), bits = per >> 16 & 0x1f;
	if (!bits || bits > 16)
		bits = 16;
	uint32_t period = per & ((1u << bits) - 1);
	if (!period)
		return BrgtMax;
	uint32_t active = (mmioRead(BlPwmCntl) & 0xffff) >> (16 - bits);
	uint32_t v = ((uint64_t)active * BrgtMax + period / 2) / period;
	return v > BrgtMax ? BrgtMax : v;
}

// AppleBacklight's AppleIntelPanelA (under PNLF) only publishes "brightness"/"linear-brightness"
// (what DisplayServices, the Control Center slider and the F14/F15 keys use) if the fb answers
// getAttributeForConnection(0, 'bklt') = {current, min, max}; it then sets the raw level with
// setAttributeForConnection(0, 'bklt', v) (AppleBacklight 180.11 setDisplay/doIntegerSet).
constexpr uint32_t AttrBklt = 'bklt';
constexpr uint32_t BkltMax = 0xffff;

IOReturn wrapGetAttr(IOService *fb, int32_t conn, uint32_t attr, uintptr_t *value) {
	if (attr == AttrBklt && conn == 0 && value && isPanelFb(fb)) {
		value[0] = currentBrightness() * BkltMax / BrgtMax;
		value[1] = 0;
		value[2] = BkltMax;
		static uint32_t logged;
		if (logged++ < 8)
			LOG("bl: bklt get -> %lu of %u", value[0], BkltMax);
		return kIOReturnSuccess;
	}
	if (attr == AttrBrgt && conn == 0 && value && isPanelFb(fb)) {
		value[0] = currentBrightness();
		value[1] = 0;
		value[2] = BrgtMax;
		return kIOReturnSuccess;
	}
	auto r = reinterpret_cast<GetAttrFn>(orgGetAttr)(fb, conn, attr, value);
	if (attr == AttrParm && conn == 0 && r == kIOReturnSuccess && value && !value[1] && isPanelFb(fb)) {
		value[1] = AttrBrgt;
		static uint32_t logged;
		if (logged++ < 4)
			LOG("bl: panel fb parameters += brgt (now %u of %u)", currentBrightness(), BrgtMax);
	}
	return r;
}

IOReturn wrapSetAttr(IOService *fb, int32_t conn, uint32_t attr, uintptr_t value) {
	if (attr == AttrBklt && conn == 0 && isPanelFb(fb)) {
		uint32_t v = value > BkltMax ? BkltMax : static_cast<uint32_t>(value);
		gBrgt = (v * BrgtMax + BkltMax / 2) / BkltMax;
		static uint32_t logged;
		if (logged++ < 40)
			LOG("bl: bklt set %lu", value);
		setBacklight(static_cast<uint32_t>((uint64_t)v * 0x10000 / BkltMax));
		return kIOReturnSuccess;
	}
	if (attr == AttrBrgt && conn == 0 && isPanelFb(fb)) {
		gBrgt = value > BrgtMax ? BrgtMax : static_cast<uint32_t>(value);
		static uint32_t logged;
		if (logged++ < 40)
			LOG("bl: brgt set %lu", value);
		setBacklight(static_cast<uint32_t>((uint64_t)gBrgt * 0x10000 / BrgtMax));
		return kIOReturnSuccess;
	}
	return reinterpret_cast<SetAttrFn>(orgSetAttr)(fb, conn, attr, value);
}

// AppleIntelPanel::setDisplay looks the panel up in its personality's "ApplePanels" by
// F<_UID>{P,I,T..}<DisplayProductID> keys. The GPD panel has none, and the "Default" fallback stores
// a NULL panel entry (setObject fails right after ApplePanels was removed) → setDisplay false →
// AppleIntelPanelA publishes only "commit", no "brightness" (auto-1021-1631). Add an F<uid>T<pid>
// table first (`-rmbnopanel` = off). Values are the raw 'bklt' levels sent to wrapSetAttr.
using PanelSetDisplayFn = bool (*)(IOService *, IOService *);
mach_vm_address_t orgPanelSetDisplay;
constexpr size_t PanelUidOff = 0x108;  // AppleIntelPanel _UID field (AppleBacklight 180.11 start)
// AGDCBacklightControl F10T9c84 curve (max 0x375), scaled to 0..BkltMax below.
const uint16_t kPanelCurve[] {0x0000, 0x002f, 0x0036, 0x0040, 0x004b, 0x0058, 0x0067, 0x0077, 0x008b,
                              0x00af, 0x00dd, 0x0118, 0x0160, 0x01ba, 0x022c, 0x02bf, 0x0375};

void addPanelEntry(IOService *panel, IOService *display, char *key, size_t keySize) {
	auto pid = OSDynamicCast(OSNumber, display->getProperty("DisplayProductID"));
	if (!pid)
		return;
	uint32_t uid = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(panel) + PanelUidOff);
	snprintf(key, keySize, "F%uT%04x", uid, pid->unsigned32BitValue());
	// Registry-owned collections panic on in-place change ("Trying to change a collection in the
	// registry", auto-1021-1643) → build a copy and set it as the new property.
	auto cur = OSDynamicCast(OSDictionary, panel->getProperty("ApplePanels"));
	if (!cur && panel->getProperty("ApplePanel"))
		return;
	if (cur && cur->getObject(key))
		return;
	auto panels = cur ? OSDictionary::withDictionary(cur, cur->getCount() + 1) : OSDictionary::withCapacity(1);
	if (!panels)
		return;
	constexpr size_t n = arrsize(kPanelCurve);
	uint8_t buf[2 + 2 * n] {0, n};
	for (size_t i = 0; i < n; i++) {
		uint32_t v = kPanelCurve[i] * BkltMax / kPanelCurve[n - 1];
		buf[2 + 2 * i] = v >> 8;
		buf[3 + 2 * i] = v & 0xff;
	}
	// Kept retained forever: setDisplay takes the entry unretained, then removeProperty("ApplePanels")
	// frees our copy (only the registry held it) → setObject("ApplePanel", freed data) panicked
	// (auto-1021-1647). Apple's personality dict is kept alive by the catalogue, ours isn't.
	static OSData *data;
	if (!data)
		data = OSData::withBytes(buf, sizeof(buf));
	if (data) {
		panels->setObject(key, data);
		panel->setProperty("ApplePanels", panels);
	}
	panels->release();
	if (!data)
		return;
	LOG("bl: ApplePanels += %s (%zu levels)", key, n);
}

bool wrapPanelSetDisplay(IOService *panel, IOService *display) {
	char key[32] {};
	if (panel && display)
		addPanelEntry(panel, display, key, sizeof(key));
	bool r = reinterpret_cast<PanelSetDisplayFn>(orgPanelSetDisplay)(panel, display);
	LOG("bl: AppleIntelPanel::setDisplay %s = %d", key[0] ? key : "(no DisplayProductID)", r);
	return r;
}

}  // namespace

void displayRouteBacklight(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (checkKernelArgument("-rmbnopanel"))
		return;
	KernelPatcher::RouteRequest req {"__ZN15AppleIntelPanel10setDisplayEP9IODisplay", wrapPanelSetDisplay, orgPanelSetDisplay};
	if (!patcher.routeMultiple(index, &req, 1, address, size))
		LOG("bl: failed to route AppleIntelPanel::setDisplay (err %d)", patcher.getError());
	else
		LOG("bl: AppleIntelPanel::setDisplay hook installed");
	patcher.clearError();
}

void displaySetAgdcBase(mach_vm_address_t base) {
	gAgdcBase = base;
	LOG("AGDC at %llx", base);
}

bool rmbVerbose() {
	static int on = -1;
	if (on < 0)
		on = checkKernelArgument("-rmbverbose");
	return on;
}

void displayRoute(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	gKeepPwrGate = checkKernelArgument("-rmbpwrgate");
	gNoFwBl = checkKernelArgument("-rmbnofwbl");
	gNoRevive = checkKernelArgument("-rmbnorevive");
	gFwAbmSet = checkKernelArgument("-rmbfwabmset");
	if (!gInboxLock)
		gInboxLock = IOLockAlloc();
	edpLinkWatchStart();
	gNoColdPg = checkKernelArgument("-rmbnocoldpg");
	gKeepPhyWrites = checkKernelArgument("-rmbphywr");
	gPhyBCpu = checkKernelArgument("-rmbphybcpu");
	gKeepDcc = checkKernelArgument("-rmbdcc");
	gNoAperture = checkKernelArgument("-rmbnoaperture");
	gNoDet = checkKernelArgument("-rmbnodet");
	gKeepDto = checkKernelArgument("-rmbdto");
	gKeepStutter = checkKernelArgument("-rmbstutter");
	gKeepEdpOff = checkKernelArgument("-rmbedpoff");
	gKeepEdpOffAlways = checkKernelArgument("-rmbedpoffkeep");
	gKeepPllPd = checkKernelArgument("-rmbpllpd");
	gDropAllPllPd = checkKernelArgument("-rmbpllpdall");
	gNoPllPdLate = checkKernelArgument("-rmbnopllpdlate");
	gKeepEdpOn = checkKernelArgument("-rmbedpon");
	gNoHold = checkKernelArgument("-rmbnohold");
	gAmdLogAll = checkKernelArgument("-rmbamdlog");
	gKeepTxInit = checkKernelArgument("-rmbtxinit");
	gKeepPixClk = checkKernelArgument("-rmbpixclk");
	gNoDispClk = checkKernelArgument("-rmbnodispclk");
	gKeepFams = checkKernelArgument("-rmbfams");
	gKeepAbm = checkKernelArgument("-rmbabm");
	gNoClkMgr = checkKernelArgument("-rmbnoclkmgr");
	gNoFwTrace = checkKernelArgument("-rmbnofwtrace");
	gNoOtgClk = checkKernelArgument("-rmbotgclk");
	KernelPatcher::RouteRequest reqs[] {
		{"__ZN27AMDRadeonX6000_AmdDalHelper21dalAtomReadRegister32EPvj", wrapRead, orgRead},
		{"__ZN27AMDRadeonX6000_AmdDalHelper22dalAtomWriteRegister32EPvjj", wrapWrite, orgWrite},
		{"__ZN27AMDRadeonX6000_AmdDalHelper19dalAtomExecCmdTableEPvjS0_", wrapExecTable, orgExecTable},
		{"__ZN24AMDRadeonX6000_AmdLogger15initWithPciInfoEP11IOPCIDevice", wrapLoggerInit, orgLoggerInit},
		{"__ZN22AMDRadeonX6000_AmdLink10detectLinkE17AmdFbDetectReasonP18DetectedLinkStatus", wrapDetectLink, orgDetectLink},
		{"__ZN34AMDRadeonX6000_AmdRadeonController17detectAllDisplaysEv", wrap_detectAllDisplays, org_detectAllDisplays},
		{"__ZN34AMDRadeonX6000_AmdRadeonController14detectDisplaysEj", wrap_detectDisplays, org_detectDisplays},
		{"__ZN34AMDRadeonX6000_AmdRadeonController19detectDisplayOnLinkEh", wrap_detectDisplayOnLink, org_detectDisplayOnLink},
		{"__ZN34AMDRadeonX6000_AmdRadeonController20establishBootDisplayEv", wrap_establishBootDisplay, org_establishBootDisplay},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess11hwReadReg32Ej", wrapRawRead<orgHwRead>, orgHwRead},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess17readValidateReg32Ej", wrapRawRead<orgValRead>, orgValRead},
		{"__ZN32AMDRadeonX6000_AmdRegisterAccess10writeReg32Ejj", wrapRawWrite, orgRawWrite},
	};
	gFbText = address;
	gFbOpen = patcher.solveSymbol(index, "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer4openEv", address, size);
	patcher.clearError();
	if (auto wm = patcher.solveSymbol(index, "__ZN39AMDRadeonX6000_AmdRadeonControllerNavi219setupBootWatermarksEv", address, size)) {
		gBootWmFirst = static_cast<uint32_t>(wm - address + FbTextBase);
		gBootWmLast = gBootWmFirst + BootWmSize;
	} else {
		LOG("dcn: setupBootWatermarks not found, its raw writes stay unremapped");
	}
	patcher.clearError();
	if (!checkKernelArgument("-rmbnoopen")) {
		thread_t t;
		if (kernel_thread_start(earlyOpen, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
	if (!checkKernelArgument("-rmbvcn")) {
		thread_t t;
		if (kernel_thread_start(noVideoEngine, nullptr, &t) == KERN_SUCCESS)
			thread_deallocate(t);
	}
	gGetLink = patcher.solveSymbol(index, "__ZNK34AMDRadeonX6000_AmdRadeonController7getLinkEh", address, size);
	gResetConnected = patcher.solveSymbol(index, "__ZN22AMDRadeonX6000_AmdLink20resetConnectedStatusEv", address, size);
	patcher.clearError();
	if (!patcher.routeMultiple(index, reqs, address, size))
		LOG("dcn: failed to route DAL register callbacks (err %d)", patcher.getError());
	else
		LOG("dcn: DAL register remap installed (%zu registers)", sizeof(kDcnRemap) / sizeof(kDcnRemap[0]));
	patcher.clearError();
	if (!checkKernelArgument("-rmbnobrgt")) {
		KernelPatcher::RouteRequest attrReqs[] {
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25getAttributeForConnectionEijPm", wrapGetAttr, orgGetAttr},
			{"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer25setAttributeForConnectionEijm", wrapSetAttr, orgSetAttr},
		};
		if (!patcher.routeMultiple(index, attrReqs, address, size))
			LOG("bl: failed to route fb attribute hooks (err %d)", patcher.getError());
		else
			LOG("bl: brgt attribute hooks installed");
		patcher.clearError();
	}

	constexpr mach_vm_address_t TextBase = FbTextBase, RegWait = 0xc0f6bd7;
	static const uint8_t prologue[] {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
	auto at = address + (RegWait - TextBase);
	if (memcmp(reinterpret_cast<void *>(at), prologue, sizeof(prologue))) {
		LOG("dcn: generic_reg_wait prologue mismatch, not hooked");
		return;
	}
	orgRegWait = patcher.routeFunction(at, reinterpret_cast<mach_vm_address_t>(wrapRegWait), true);
	LOG("dcn: generic_reg_wait %s", orgRegWait ? "hooked" : "hook FAILED");
	patcher.clearError();

	constexpr mach_vm_address_t DalLog = 0xc14fb83;
	static const uint8_t logPrologue[] {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};
	auto logAt = address + (DalLog - TextBase);
	if (memcmp(reinterpret_cast<void *>(logAt), logPrologue, sizeof(logPrologue))) {
		LOG("dcn: dm_logger_write prologue mismatch, not hooked");
		return;
	}
	auto r = patcher.routeFunction(logAt, reinterpret_cast<mach_vm_address_t>(wrapDalLog), false);
	LOG("dcn: dm_logger_write %s", r ? "hooked" : "hook FAILED");
	patcher.clearError();
}
