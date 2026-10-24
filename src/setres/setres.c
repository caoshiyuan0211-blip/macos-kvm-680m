// List / set display modes of the GPD panel (vendor 0x2889; else the first large display), including HiDPI duplicates.
// No SDK: CoreGraphics/CoreFoundation via dlsym. Usage: setres            (list, [index] per mode)
//                                                      setres W H        (set permanently, prefers HiDPI)
//                                                      setres idx N      (set list mode N for this session only)
//                                                      setres cgs        (raw WindowServer modes: number, size, depth, Hz)
//                                                      setres cgs N      (set raw mode number N for this session only)
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
int atoi(const char *);
int strcmp(const char *, const char *);

typedef uint32_t D;
typedef const void *R;
typedef struct { double x, y, w, h; } Rect;

int main(int argc, char **argv) {
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	if (!cg || !cf) { printf("dlopen failed\n"); return 1; }
	int (*list)(uint32_t, D *, uint32_t *) = dlsym(cg, "CGGetOnlineDisplayList");
	Rect (*bounds)(D) = dlsym(cg, "CGDisplayBounds");
	uint32_t (*vendor)(D) = dlsym(cg, "CGDisplayVendorNumber");
	R (*allModes)(D, R) = dlsym(cg, "CGDisplayCopyAllDisplayModes");
	R (*curMode)(D) = dlsym(cg, "CGDisplayCopyDisplayMode");
	long (*mW)(R) = dlsym(cg, "CGDisplayModeGetWidth");
	long (*mH)(R) = dlsym(cg, "CGDisplayModeGetHeight");
	long (*mPW)(R) = dlsym(cg, "CGDisplayModeGetPixelWidth");
	long (*mPH)(R) = dlsym(cg, "CGDisplayModeGetPixelHeight");
	double (*mHz)(R) = dlsym(cg, "CGDisplayModeGetRefreshRate");
	int (*mOK)(R) = dlsym(cg, "CGDisplayModeIsUsableForDesktopGUI");
	int32_t (*mID)(R) = dlsym(cg, "CGDisplayModeGetIODisplayModeID");
	uint32_t (*mFl)(R) = dlsym(cg, "CGDisplayModeGetIOFlags");
	int (*begin)(void **) = dlsym(cg, "CGBeginDisplayConfiguration");
	int (*cfgMode)(void *, D, R, R) = dlsym(cg, "CGConfigureDisplayWithDisplayMode");
	int (*complete)(void *, int) = dlsym(cg, "CGCompleteDisplayConfiguration");
	R *dupKey = dlsym(cg, "kCGDisplayShowDuplicateLowResolutionModes");
	R *cfTrue = dlsym(cf, "kCFBooleanTrue");
	R (*dictCreate)(R, R *, R *, long, R, R) = dlsym(cf, "CFDictionaryCreate");
	long (*arrCount)(R) = dlsym(cf, "CFArrayGetCount");
	R (*arrGet)(R, long) = dlsym(cf, "CFArrayGetValueAtIndex");
	void *kcb = dlsym(cf, "kCFTypeDictionaryKeyCallBacks"), *vcb = dlsym(cf, "kCFTypeDictionaryValueCallBacks");

	D ds[16], d = 0; uint32_t n = 0;
	list(16, ds, &n);
	for (uint32_t i = 0; i < n && !d; i++) if (vendor(ds[i]) == 0x2889) d = ds[i];
	for (uint32_t i = 0; i < n && !d; i++) if (bounds(ds[i]).w >= 1000) d = ds[i];
	for (uint32_t i = 0; i < n && !d; i++) d = ds[i];
	if (!d) { printf("no display\n"); return 1; }
	if (argc > 1 && !strcmp(argv[1], "cgs")) {
		// raw mode table (one entry per mode x depth); layout as in displayplacer's modes_D4
		void *sl = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", 1);
		int (*num)(D, int *) = dlsym(cg, "CGSGetNumberOfDisplayModes");
		int (*desc)(D, int, void *, int) = dlsym(cg, "CGSGetDisplayModeDescriptionOfLength");
		int (*curNum)(D, int *) = dlsym(cg, "CGSGetCurrentDisplayMode");
		int (*cfgRaw)(void *, D, int) = dlsym(cg, "CGSConfigureDisplayMode");
		if (sl && !num) num = dlsym(sl, "SLSGetNumberOfDisplayModes");
		if (sl && !desc) desc = dlsym(sl, "SLSGetDisplayModeDescriptionOfLength");
		if (sl && !curNum) curNum = dlsym(sl, "SLSGetCurrentDisplayMode");
		if (sl && !cfgRaw) cfgRaw = dlsym(sl, "SLSConfigureDisplayMode");
		if (!num || !desc || !curNum || !cfgRaw) { printf("no CGS mode API\n"); return 1; }
		int nm = 0, cm = -1;
		num(d, &nm);
		curNum(d, &cm);
		printf("display %u raw modes %d current %d\n", d, nm, cm);
		if (argc > 2) {
			void *cfg = 0;
			begin(&cfg);
			printf("configure raw %d: %d\n", atoi(argv[2]), cfgRaw(cfg, d, atoi(argv[2])));
			printf("complete %d\n", complete(cfg, 1 /* kCGConfigureForSession */));
			curNum(d, &cm);
			printf("now raw %d\n", cm);
			return 0;
		}
		for (int i = 0; i < nm; i++) {
			union { uint8_t raw[0xD4]; struct { uint32_t mode, flags, w, h, depth, dc2[42]; uint16_t dc3, freq; uint32_t dc4[4]; float density; } m; } u;
			for (int j = 0; j < 0xD4; j++) u.raw[j] = 0;
			desc(d, i, &u, 0xD4);
			printf("  raw %d%s: %ux%u depth %u %uHz scale %.1f mode 0x%x flags 0x%x\n", i, i == cm ? " *" : "", u.m.w, u.m.h,
			       u.m.depth, u.m.freq, u.m.density, u.m.mode, u.m.flags);
		}
		return 0;
	}
	R k[1] = { *dupKey }, v[1] = { *cfTrue };
	R opts = dictCreate(0, k, v, 1, kcb, vcb);
	R modes = allModes(d, opts);
	R cur = curMode(d);
	printf("display %u current %ldx%ld (pixels %ldx%ld)\n", d, mW(cur), mH(cur), mPW(cur), mPH(cur));
	long cnt = arrCount(modes);
	int idx = argc > 2 && !strcmp(argv[1], "idx") ? atoi(argv[2]) : -1;
	int wantW = argc > 2 && idx < 0 ? atoi(argv[1]) : 0, wantH = argc > 2 && idx < 0 ? atoi(argv[2]) : 0;
	R best = 0;
	for (long i = 0; i < cnt; i++) {
		R m = arrGet(modes, i);
		int hidpi = mPW(m) > mW(m);
		if (!wantW && idx < 0)
			printf("  [%ld] %ldx%ld pixels %ldx%ld %.2fHz id 0x%x flags 0x%x%s%s\n", i, mW(m), mH(m), mPW(m), mPH(m),
			       mHz(m), mID(m), mFl(m), hidpi ? " HiDPI" : "", mOK(m) ? "" : " (not GUI)");
		if (wantW && mW(m) == wantW && mH(m) == wantH && (!best || (hidpi && !(mPW(best) > mW(best))))) best = m;
	}
	if (idx >= 0) {
		if (idx >= cnt) { printf("index %d out of range (%ld modes)\n", idx, cnt); return 1; }
		best = arrGet(modes, idx);
		printf("set [%d] %ldx%ld pixels %ldx%ld id 0x%x\n", idx, mW(best), mH(best), mPW(best), mPH(best), mID(best));
	} else if (!wantW) return 0;
	if (!best) { printf("mode %dx%d not found\n", wantW, wantH); return 1; }
	void *cfg = 0;
	begin(&cfg);
	printf("configure %d\n", cfgMode(cfg, d, best, 0));
	printf("complete %d\n", complete(cfg, idx >= 0 ? 1 /* kCGConfigureForSession */ : 2 /* kCGConfigurePermanently */));
	cur = curMode(d);
	printf("now %ldx%ld (pixels %ldx%ld) id 0x%x\n", mW(cur), mH(cur), mPW(cur), mPH(cur), mID(cur));
	return 0;
}
