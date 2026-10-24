// Extended desktop with the external screen as main: unmirror every display, put each 680M screen at
// its largest 1x (max pixels) mode, place the external screen at (0,0) and the GPD panel to its
// right. Panel = EDID vendor 0x2889 (GPD); external = largest other screen >= 1920 px wide (skips the
// 1280x800 vmware-svga screen). No SDK: CoreGraphics/CoreFoundation via dlsym.
// Usage: setext [list]
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);

typedef uint32_t D;
typedef const void *R;
typedef struct { double x, y, w, h; } Rect;

static R (*allModes)(D, R);
static R (*curMode)(D);
static long (*mW)(R), (*mH)(R), (*mPW)(R), (*mPH)(R);
static double (*mHz)(R);
static int (*mOK)(R);
static long (*arrCount)(R);
static R (*arrGet)(R, long);

// largest usable 1x mode (pixels == points), highest refresh on ties. maxRate > 0: only modes with
// w*h*Hz below it. 4K60 RGB/444 on HDMI (594 MHz) needs HDMI 2.0 scrambling, which gave "no signal"
// on the LG TV (auto-1017-1943) while 4K60 YCbCr 4:2:0 (297 MHz) worked (1843); 230e6 also leaves
// room for 10 bpc under the 340 MHz TMDS limit. maxPx > 0: only modes with w*h up to it (4K25/30 is
// still 297 MHz, x1.25 at 10 bpc = 371 MHz -> no signal, auto-1017-2023).
static R maxMode(D d, double maxRate, long maxPx) {
	R modes = allModes(d, 0), best = 0;
	for (long i = 0, n = modes ? arrCount(modes) : 0; i < n; i++) {
		R m = arrGet(modes, i);
		if (!mOK(m) || mPW(m) != mW(m)) continue;
		if (maxRate > 0 && (double)mPW(m) * mPH(m) * (mHz(m) > 0 ? mHz(m) : 60) > maxRate) continue;
		if (maxPx > 0 && (mPW(m) * mPH(m) > maxPx || (mHz(m) > 0 && mHz(m) < 50))) continue;
		if (!best || mPW(m) * mPH(m) > mPW(best) * mPH(best) ||
		    (mPW(m) * mPH(m) == mPW(best) * mPH(best) && mHz(m) > mHz(best))) best = m;
	}
	return best;
}

// same size as cur (points and pixels), highest refresh: the GPD panel EDID has 60 Hz (286.8 MHz) and
// 40 Hz (191.17 MHz) timings and macOS falls back to 40 Hz (visible flicker) after a screen
// reconfiguration (auto-1020-1055).
static R fastestSameSize(D d, R cur) {
	R modes = allModes(d, 0), best = 0;
	for (long i = 0, n = modes ? arrCount(modes) : 0; i < n; i++) {
		R m = arrGet(modes, i);
		if (!mOK(m) || mW(m) != mW(cur) || mH(m) != mH(cur) || mPW(m) != mPW(cur) || mPH(m) != mPH(cur)) continue;
		if (!best || mHz(m) > mHz(best)) best = m;
	}
	return best;
}

// own configuration step: switching the panel together with the TV hit an MPC idle timeout
// (auto-1017-2023)
static void panelHz(D panel, int (*begin)(void **), int (*cfgMode)(void *, D, R, R), int (*complete)(void *, int)) {
	if (!panel) return;
	R cur = curMode(panel), m = fastestSameSize(panel, cur);
	printf("panel %ldx%ld %.2fHz", mW(cur), mH(cur), mHz(cur));
	if (!m || mHz(m) <= mHz(cur) + 0.5) { printf(" ok\n"); return; }
	void *cfg = 0;
	begin(&cfg);
	printf(" -> %.2fHz: %d", mHz(m), cfgMode(cfg, panel, m, 0));
	printf(" complete %d\n", complete(cfg, 2));
}

int main(int argc, char **argv) {
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	if (!cg || !cf) { printf("dlopen failed\n"); return 1; }
	int (*list)(uint32_t, D *, uint32_t *) = dlsym(cg, "CGGetOnlineDisplayList");
	D (*mainD)(void) = dlsym(cg, "CGMainDisplayID");
	uint32_t (*vendor)(D) = dlsym(cg, "CGDisplayVendorNumber");
	D (*mirrorOf)(D) = dlsym(cg, "CGDisplayMirrorsDisplay");
	Rect (*bounds)(D) = dlsym(cg, "CGDisplayBounds");
	int (*begin)(void **) = dlsym(cg, "CGBeginDisplayConfiguration");
	int (*cfgMirror)(void *, D, D) = dlsym(cg, "CGConfigureDisplayMirrorOfDisplay");
	int (*cfgMode)(void *, D, R, R) = dlsym(cg, "CGConfigureDisplayWithDisplayMode");
	int (*origin)(void *, D, int32_t, int32_t) = dlsym(cg, "CGConfigureDisplayOrigin");
	int (*complete)(void *, int) = dlsym(cg, "CGCompleteDisplayConfiguration");
	allModes = dlsym(cg, "CGDisplayCopyAllDisplayModes");
	curMode = dlsym(cg, "CGDisplayCopyDisplayMode");
	mW = dlsym(cg, "CGDisplayModeGetWidth");
	mH = dlsym(cg, "CGDisplayModeGetHeight");
	mPW = dlsym(cg, "CGDisplayModeGetPixelWidth");
	mPH = dlsym(cg, "CGDisplayModeGetPixelHeight");
	mHz = dlsym(cg, "CGDisplayModeGetRefreshRate");
	mOK = dlsym(cg, "CGDisplayModeIsUsableForDesktopGUI");
	arrCount = dlsym(cf, "CFArrayGetCount");
	arrGet = dlsym(cf, "CFArrayGetValueAtIndex");

	D ds[16], panel = 0, ext = 0; uint32_t n = 0;
	list(16, ds, &n);
	printf("n %u main %u\n", n, mainD());
	long extPx = 0;
	for (uint32_t i = 0; i < n; i++) {
		Rect r = bounds(ds[i]);
		R cur = curMode(ds[i]), mx = maxMode(ds[i], 0, 0);
		long px = mx ? mPW(mx) : 0;
		printf("  id %u vendor %x mirrors %u bounds %g,%g %gx%g cur %ldx%ld px %ldx%ld %.2fHz max1x %ldx%ld\n", ds[i],
		       vendor(ds[i]), mirrorOf(ds[i]), r.x, r.y, r.w, r.h, mW(cur), mH(cur), mPW(cur), mPH(cur),
		       mHz(cur), mx ? mPW(mx) : 0, mx ? mPH(mx) : 0);
		if (vendor(ds[i]) == 0x2889 || vendor(ds[i]) == 0x8928) { if (!panel) panel = ds[i]; }
		// vendor 'unkn' = no EDID (phantom/unreadable screen, auto-1017-2000): don't make it main
		else if (vendor(ds[i]) != 0x756e6b6e && px >= 1920 && px > extPx) { ext = ds[i]; extPx = px; }
	}
	printf("panel %u ext %u\n", panel, ext);
	if (argc > 1 && argv[1][0] == 'l') return 0;
	if (!ext) { printf("no external screen\n"); panelHz(panel, begin, cfgMode, complete); return 0; }

	void *cfg = 0;
	begin(&cfg);
	for (uint32_t i = 0; i < n; i++) printf("unmirror %u: %d\n", ds[i], cfgMirror(cfg, ds[i], 0));
	// Only the external screen's mode: the panel keeps the user's own mode (2048x1280), and
	// switching it together with the TV hit an MPC idle timeout and crashed WindowServer
	// (auto-1017-2023). 1080p60 = 148.5 MHz needs no HDMI 2.0 scrambling at any bpc; cap the rate
	// at 61 Hz for 1080p (the 230e6 limit let 1080p100 = 297 MHz through, auto-1019-1155).
	R m = maxMode(ext, 1920.0 * 1080 * 61, 1920 * 1080);
	if (m) printf("mode %u %ldx%ld %.2fHz: %d\n", ext, mW(m), mH(m), mHz(m), cfgMode(cfg, ext, m, 0));
	printf("complete1 %d\n", complete(cfg, 2 /* kCGConfigurePermanently */));

	begin(&cfg);
	double x = bounds(ext).w;
	printf("origin ext %d\n", origin(cfg, ext, 0, 0));
	if (panel) { printf("origin panel -> %g: %d\n", x, origin(cfg, panel, (int32_t)x, 0)); x += bounds(panel).w; }
	for (uint32_t i = 0; i < n; i++)
		if (ds[i] != ext && ds[i] != panel) { printf("origin %u -> %g: %d\n", ds[i], x, origin(cfg, ds[i], (int32_t)x, 0)); x += bounds(ds[i]).w; }
	printf("complete2 %d\n", complete(cfg, 2));
	printf("main now %u (ext %u)\n", mainD(), ext);
	panelHz(panel, begin, cfgMode, complete);
	return 0;
}
