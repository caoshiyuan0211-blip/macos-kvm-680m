//
//  Guest system sleep: the QEMU vmware-svga is driven by IONDRVFramebuffer ("Display_boot"), whose
//  power states carry kIOPMPreventSystemSleep, so IOPMrootDomain refuses every system sleep
//  ("System sleep prevented by kPMChildPreventSystemSleep"). Drop that veto; the svga is reset by
//  QEMU on S3 resume anyway. `-rmbsvgaveto` keeps Apple's behaviour.
//

#include <IOKit/IOService.h>
#include <Headers/kern_api.hpp>
#include "rmb.hpp"

static mach_vm_address_t orgUpdatePreventSystemSleepList;

static void wrapUpdatePreventSystemSleepList(void *root, IOService *svc, bool add) {
	if (add && svc && svc->metaCast("IONDRVFramebuffer")) {
		LOG("sleep: ignored system-sleep veto from %s (%s)", svc->getName(), svc->getMetaClass()->getClassName());
		return;
	}
	FunctionCast(wrapUpdatePreventSystemSleepList, orgUpdatePreventSystemSleepList)(root, svc, add);
}

void sleepRoute(KernelPatcher &patcher) {
	if (checkKernelArgument("-rmbsvgaveto"))
		return;
	KernelPatcher::RouteRequest req {"__ZN14IOPMrootDomain28updatePreventSystemSleepListEP9IOServiceb",
	                                 wrapUpdatePreventSystemSleepList, orgUpdatePreventSystemSleepList};
	if (!patcher.routeMultiple(KernelPatcher::KernelID, &req, 1))
		LOG("sleep: failed to route updatePreventSystemSleepList (err %d)", patcher.getError());
	else
		LOG("sleep: svga sleep veto filter installed");
	patcher.clearError();
}
