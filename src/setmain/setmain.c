// Make the display with the given vendor (default AMD 0x1002 via CGDisplayVendorNumber) the main
// display by moving it to origin (0,0) and placing every other display to its right. No SDK:
// CoreGraphics via dlsym. Usage: setmain [list]
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);


typedef uint32_t D;
typedef struct { double x, y, w, h; } Rect;

int main(int argc, char **argv) {
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	if (!cg) { printf("no CoreGraphics\n"); return 1; }
	int (*list)(uint32_t, D *, uint32_t *) = dlsym(cg, "CGGetOnlineDisplayList");
	D (*mainD)(void) = dlsym(cg, "CGMainDisplayID");
	uint32_t (*vendor)(D) = dlsym(cg, "CGDisplayVendorNumber");
	Rect (*bounds)(D) = dlsym(cg, "CGDisplayBounds");
	int (*begin)(void **) = dlsym(cg, "CGBeginDisplayConfiguration");
	int (*origin)(void *, D, int32_t, int32_t) = dlsym(cg, "CGConfigureDisplayOrigin");
	int (*complete)(void *, int) = dlsym(cg, "CGCompleteDisplayConfiguration");
	D ds[16]; uint32_t n = 0;
	int e = list(16, ds, &n);
	printf("list err %d n %u main %u\n", e, n, mainD());
	D amd = 0; double amdW = 0;
	for (uint32_t i = 0; i < n; i++) {
		Rect r = bounds(ds[i]);
		printf("  id %u vendor %x bounds %g,%g %gx%g\n", ds[i], vendor(ds[i]), r.x, r.y, r.w, r.h);
		if (r.w >= 2000 && !amd) { amd = ds[i]; amdW = r.w; }
	}
	if (argc > 1 && argv[1][0] == 'l') return 0;
	if (!amd) { printf("no large display found\n"); return 1; }
	void *cfg = 0;
	printf("begin %d\n", begin(&cfg));
	double x = amdW;
	for (uint32_t i = 0; i < n; i++) {
		if (ds[i] == amd) printf("origin amd %d\n", origin(cfg, ds[i], 0, 0));
		else { printf("origin %u -> %g: %d\n", ds[i], x, origin(cfg, ds[i], (int32_t)x, 0)); x += bounds(ds[i]).w; }
	}
	printf("complete %d\n", complete(cfg, 2 /* kCGConfigurePermanently */));
	printf("main now %u\n", mainD());
	return 0;
}
