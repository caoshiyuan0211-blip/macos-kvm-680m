// Logs mouse events as macOS delivers them (session event tap, listen-only) + cursor position changes.
// Usage: clicklog <seconds>. No SDK: CoreGraphics/CoreFoundation via dlsym.
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
static int (*fflush_)(void *);
static int (*atoi_)(const char *);
typedef struct { double x, y; } P;
typedef const void *R;
static P (*getLoc)(R);
static R (*evCreate)(R);
static void (*cfRelease)(R);
static int64_t (*getInt)(R, int);
static double (*now)(void);
static R cb(R proxy, uint32_t type, R ev, void *u) {
	P p = getLoc(ev);
	printf("%.3f ev type %u at %.1f,%.1f delta %ld,%ld\n", now(), type, p.x, p.y, getInt(ev, 4), getInt(ev, 5));
	fflush_(0);
	return ev;
}
int main(int argc, char **argv) {
	void *sys = dlopen("/usr/lib/libSystem.B.dylib", 1);
	fflush_ = dlsym(sys, "fflush");
	atoi_ = dlsym(sys, "atoi");
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	getLoc = dlsym(cg, "CGEventGetLocation");
	evCreate = dlsym(cg, "CGEventCreate");
	getInt = dlsym(cg, "CGEventGetIntegerValueField");
	cfRelease = dlsym(cf, "CFRelease");
	now = dlsym(cf, "CFAbsoluteTimeGetCurrent");
	R (*tapCreate)(int, int, int, uint64_t, void *, void *) = dlsym(cg, "CGEventTapCreate");
	R (*srcCreate)(R, R, long) = dlsym(cf, "CFMachPortCreateRunLoopSource");
	R (*curLoop)(void) = dlsym(cf, "CFRunLoopGetCurrent");
	void (*addSrc)(R, R, R) = dlsym(cf, "CFRunLoopAddSource");
	int (*runIn)(R, double, int) = dlsym(cf, "CFRunLoopRunInMode");
	R mode = *(R *)dlsym(cf, "kCFRunLoopDefaultMode");
	uint64_t mask = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);
	R tap = tapCreate(argc > 2 ? atoi_(argv[2]) : 0, 0, 1, mask, cb, 0);
	printf("tap %p\n", tap);
	if (tap)
		addSrc(curLoop(), srcCreate(0, tap, 0), mode);
	double end = now() + (argc > 1 ? atoi_(argv[1]) : 60);
	P last = {-1, -1};
	while (now() < end) {
		runIn(mode, 0.02, 1);
		R e = evCreate(0);
		P p = getLoc(e);
		cfRelease(e);
		if (p.x != last.x || p.y != last.y) {
			printf("%.3f cursor %.1f,%.1f\n", now(), p.x, p.y);
			fflush_(0);
			last = p;
		}
	}
	return 0;
}
