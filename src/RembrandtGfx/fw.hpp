#pragma once
// Firmware blobs embedded by fw.s.
#define FW_DECL(n) extern "C" const uint8_t fw_yc_##n[], fw_yc_##n##_end[];
FW_DECL(me)
FW_DECL(pfp)
FW_DECL(ce)
FW_DECL(mec)
FW_DECL(mec_jt)
FW_DECL(rlc)
FW_DECL(srlist_cntl)
FW_DECL(srlist_gpm)
FW_DECL(srlist_srm)
FW_DECL(rlc_iram)
FW_DECL(rlc_dram)
FW_DECL(sdma)
FW_DECL(toc)
FW_DECL(dmcub)
#define FW_SIZE(n) static_cast<uint32_t>(fw_yc_##n##_end - fw_yc_##n)
