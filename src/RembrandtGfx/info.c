#include <mach/mach_types.h>
extern kern_return_t RembrandtGfx_kern_start(kmod_info_t *, void *);
extern kern_return_t RembrandtGfx_kern_stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.hkvm.RembrandtGfx, "0.1.0", RembrandtGfx_kern_start, RembrandtGfx_kern_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
