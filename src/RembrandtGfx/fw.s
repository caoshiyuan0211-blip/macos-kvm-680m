// Rembrandt (yellow_carp) firmware pieces, split from /usr/lib/firmware/amdgpu by
// scripts/fw-extract.py. AMD's redistributable firmware; kept local to this machine.
	.section __DATA,__const
	.globl _fw_yc_me, _fw_yc_me_end
	.p2align 8
_fw_yc_me:
	.incbin "fw/me.bin"
_fw_yc_me_end:
	.globl _fw_yc_pfp, _fw_yc_pfp_end
	.p2align 8
_fw_yc_pfp:
	.incbin "fw/pfp.bin"
_fw_yc_pfp_end:
	.globl _fw_yc_ce, _fw_yc_ce_end
	.p2align 8
_fw_yc_ce:
	.incbin "fw/ce.bin"
_fw_yc_ce_end:
	.globl _fw_yc_mec, _fw_yc_mec_end
	.p2align 8
_fw_yc_mec:
	.incbin "fw/mec.bin"
_fw_yc_mec_end:
	.globl _fw_yc_mec_jt, _fw_yc_mec_jt_end
	.p2align 8
_fw_yc_mec_jt:
	.incbin "fw/mec_jt.bin"
_fw_yc_mec_jt_end:
	.globl _fw_yc_rlc, _fw_yc_rlc_end
	.p2align 8
_fw_yc_rlc:
	.incbin "fw/rlc.bin"
_fw_yc_rlc_end:
	.globl _fw_yc_srlist_cntl, _fw_yc_srlist_cntl_end
	.p2align 8
_fw_yc_srlist_cntl:
	.incbin "fw/srlist_cntl.bin"
_fw_yc_srlist_cntl_end:
	.globl _fw_yc_srlist_gpm, _fw_yc_srlist_gpm_end
	.p2align 8
_fw_yc_srlist_gpm:
	.incbin "fw/srlist_gpm.bin"
_fw_yc_srlist_gpm_end:
	.globl _fw_yc_srlist_srm, _fw_yc_srlist_srm_end
	.p2align 8
_fw_yc_srlist_srm:
	.incbin "fw/srlist_srm.bin"
_fw_yc_srlist_srm_end:
	.globl _fw_yc_rlc_iram, _fw_yc_rlc_iram_end
	.p2align 8
_fw_yc_rlc_iram:
	.incbin "fw/rlc_iram.bin"
_fw_yc_rlc_iram_end:
	.globl _fw_yc_rlc_dram, _fw_yc_rlc_dram_end
	.p2align 8
_fw_yc_rlc_dram:
	.incbin "fw/rlc_dram.bin"
_fw_yc_rlc_dram_end:
	.globl _fw_yc_sdma, _fw_yc_sdma_end
	.p2align 8
_fw_yc_sdma:
	.incbin "fw/sdma.bin"
_fw_yc_sdma_end:
	.globl _fw_yc_toc, _fw_yc_toc_end
	.p2align 8
_fw_yc_toc:
	.incbin "fw/toc.bin"
_fw_yc_toc_end:
	.globl _fw_yc_dmcub, _fw_yc_dmcub_end
	.p2align 8
_fw_yc_dmcub:
	.incbin "fw/dmcub.bin"
_fw_yc_dmcub_end:
