//
//  Finder/zip/exe icon scramble: Apple's AMDRadeonX6000MTLDriver (dyld shared cache) gives every
//  64KB-swizzled surface a pipe/bank XOR (addrlib Gfx10 HwlComputePipeBankXor), and the managed
//  upload path and the GPU disagree about it on Rembrandt → textures ≥72 px land XOR-shifted.
//  textest `x` (in-process patch, 2026-10-24) proved pipeBankXor 0 makes every path correct.
//  Here the same 8 bytes are patched system-wide in every code page the kernel validates
//  (cs_validate_page, like Lilu's slow user patcher; on 15.0 x86 whole 4K pages never go through
//  cs_validate_range, that is only the sub-page chunk path). `-rmbnoxor0` keeps Apple's code.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include "rmb.hpp"

namespace {

// movslq (%rdi,%rax,4),%rax; add %rdi,%rax; mov 4(%rsi),%esi; and $7,%esi;
// [mov (%rax,%rsi,4),%eax; add $2,%cl; shl %cl,%eax] -> xor %eax,%eax + nops; mov %eax,4(%rdx)
const uint8_t xorPat[] {0x48, 0x63, 0x04, 0x87, 0x48, 0x01, 0xf8, 0x8b, 0x76, 0x04, 0x83, 0xe6, 0x07, 0x8b,
                        0x04, 0xb0, 0x80, 0xc1, 0x02, 0xd3, 0xe0, 0x89, 0x42, 0x04};
const uint8_t xorRep[] {0x31, 0xc0, 0x0f, 0x1f, 0x40, 0x00, 0x66, 0x90};
constexpr size_t xorRepOff = 13;

mach_vm_address_t orgCsValidateRange;
mach_vm_address_t orgCsValidatePage;
uint32_t gXorPatched;

void writeRep(uint8_t *at, const char *how, size_t off) {
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		LOG("umd: no write access for pipeBankXor patch");
		return;
	}
	memcpy(at, xorRep, sizeof(xorRep));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	if (++gXorPatched <= 4)
		LOG("umd: HwlComputePipeBankXor patched -> pipeBankXor 0 (%s, page off 0x%lx, #%u)", how,
		    (unsigned long)off, gXorPatched);
}

void patchPage(const void *data, size_t size) {
	auto p = static_cast<uint8_t *>(const_cast<void *>(data));
	if (!p || size < sizeof(xorPat))
		return;
	// 15.0 UMD: the pattern sits at page offset 0xbfc, i.e. it straddles a 4K page boundary and each
	// page is validated alone → match the pattern's tail at the start of the range (k cut bytes, the
	// replaced bytes are all in this page while k <= xorRepOff; every such tail is unique in the UMD).
	for (size_t k = 1; k <= xorRepOff; k++) {
		if (!memcmp(p, xorPat + k, sizeof(xorPat) - k)) {
			if (memcmp(p + xorRepOff - k, xorRep, sizeof(xorRep)))
				writeRep(p + xorRepOff - k, "split", xorRepOff - k);
			break;
		}
	}
	for (size_t o = 0, end = size - sizeof(xorPat); o <= end; o++) {
		if (p[o] != 0x48 || memcmp(p + o, xorPat, sizeof(xorPat)))
			continue;
		writeRep(p + o + xorRepOff, "whole", o + xorRepOff);
	}
}

boolean_t wrapCsValidateRange(void *vp, memory_object_t pager, memory_object_offset_t off, const void *data,
                              vm_size_t size, unsigned *tainted) {
	boolean_t res = FunctionCast(wrapCsValidateRange, orgCsValidateRange)(vp, pager, off, data, size, tainted);
	if (res)
		patchPage(data, size);
	return res;
}

// xnu-11215: void cs_validate_page(vp, pager, page_offset, data, int *validated_p, int *tainted_p, int *nx_p)
void wrapCsValidatePage(void *vp, memory_object_t pager, memory_object_offset_t off, const void *data,
                        int *validated, int *tainted, int *nx) {
	FunctionCast(wrapCsValidatePage, orgCsValidatePage)(vp, pager, off, data, validated, tainted, nx);
	if (validated && *validated && tainted && !*tainted)
		patchPage(data, PAGE_SIZE);
}

}  // namespace

void umdRoute(KernelPatcher &patcher) {
	if (checkKernelArgument("-rmbnoxor0"))
		return;
	KernelPatcher::RouteRequest req[] {
		{"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage},
		{"_cs_validate_range", wrapCsValidateRange, orgCsValidateRange},
	};
	if (!patcher.routeMultipleLong(KernelPatcher::KernelID, req, arrsize(req)))
		LOG("umd: failed to route cs_validate_page/range (err %d)", patcher.getError());
	else
		LOG("umd: pipeBankXor 0 page patch installed (cs_validate_page + range)");
	patcher.clearError();
}
