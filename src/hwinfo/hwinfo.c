// Dump the 0x204-byte HW info struct that AMD GL/MTL userspace drivers fetch with
// IOConnectCallMethod selector 0x100. Tries user-client types 0..15. No SDK (dlsym).
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
typedef uint32_t mach_port_t;

int main(void) {
	void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", 1);
	void *(*matching)(const char *) = dlsym(io, "IOServiceMatching");
	mach_port_t (*getsvc)(mach_port_t, void *) = dlsym(io, "IOServiceGetMatchingService");
	int (*open)(mach_port_t, mach_port_t, uint32_t, mach_port_t *) = dlsym(io, "IOServiceOpen");
	int (*close)(mach_port_t) = dlsym(io, "IOServiceClose");
	int (*call)(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, uint64_t,
	            uint64_t *, uint32_t *, void *, uint64_t *) = dlsym(io, "IOConnectCallMethod");
	mach_port_t (*self)(void) = dlsym(dlopen("/usr/lib/libSystem.B.dylib", 1), "mach_task_self");
	mach_port_t svc = getsvc(0, matching("IOAccelerator"));
	printf("svc %x\n", svc);
	for (uint32_t t = 0; t < 16; t++) {
		mach_port_t c = 0;
		int e = open(svc, self(), t, &c);
		if (e) { printf("type %u open %x\n", t, e); continue; }
		static uint8_t buf[0x204];
		uint64_t sz = sizeof buf;
		e = call(c, 0x100, 0, 0, 0, 0, 0, 0, buf, &sz);
		printf("type %u call %x size %llu\n", t, e, (unsigned long long)sz);
		if (!e && sz) {
			for (unsigned i = 0; i < sz; i += 4) {
				if (i % 32 == 0) printf("\n%03x:", i);
				printf(" %08x", *(uint32_t *)(buf + i));
			}
			printf("\n");
			close(c);
			return 0;
		}
		close(c);
	}
	return 1;
}
