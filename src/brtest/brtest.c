// Brightness path test for the GPD panel: reads/sets "brightness" and "linear-brightness" on every
// IODisplay (IODisplayGet/SetFloatParameter) and the main display via DisplayServices (what the
// Control Center slider uses). RembrandtGfx logs "bl: brgt set N" on serial when a set reaches it.
// No SDK: IOKit/CoreFoundation/DisplayServices via dlsym.
// Usage: brtest [level 0..1]  (default: step 1.0 -> 0.3 -> 1.0, 3 s apart)
//        brtest step D        (add D to the current level, no test)
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);

typedef const void *R;
typedef uint32_t P;

static R (*cfstr)(R, const char *, uint32_t);
static void (*cfshow)(R);
static R (*matching)(const char *);
static int (*getServices)(P, R, P *);
static P (*iterNext)(P);
static R (*copyProp)(P, R, R, uint32_t);
static int (*getF)(P, uint32_t, R, float *);
static int (*setF)(P, uint32_t, R, float);
static int (*dsGet)(uint32_t, float *);
static int (*dsSet)(uint32_t, float);
static uint32_t (*mainId)(void);
static unsigned (*sleep)(unsigned);
static double (*atof)(const char *);
static int (*fflush)(void *);
#define printf(...) (printf(__VA_ARGS__), fflush ? fflush(0) : 0)

static R S(const char *s) { return cfstr(0, s, 0x08000100); }

static void each(float set) {
	P it = 0;
	getServices(0, matching("IODisplayConnect"), &it);
	for (P s; (s = iterNext(it)); ) {
		R params = copyProp(s, S("IODisplayParameters"), 0, 0);
		const char *keys[] = { "brightness", "linear-brightness", "brgt" };
		for (int k = 0; k < 3; k++) {
			float v = -1;
			int gr = getF(s, 0, S(keys[k]), &v);
			int sr = set >= 0 ? setF(s, 0, S(keys[k]), set) : 0;
			float v2 = -1;
			getF(s, 0, S(keys[k]), &v2);
			printf("display %u %-18s get %#x %.3f  set(%.2f) %#x  now %.3f\n", s, keys[k], gr, v, set, sr, v2);
		}
		if (set < 0 && params) cfshow(params);
	}
}

// Fn brightness key fallback (host scripts/screens-after-login.sh brkeys): move every display's
// "brightness" (AppleIntelPanelA, keeps the macOS UI in sync) or else "brgt" (RembrandtGfx fb
// attribute) by d, clamped to 2..100%.
static void step(float d) {
	P it = 0;
	getServices(0, matching("IODisplayConnect"), &it);
	for (P s; (s = iterNext(it)); ) {
		const char *keys[] = { "brightness", "brgt" };
		for (int k = 0; k < 2; k++) {
			float v = -1;
			if (getF(s, 0, S(keys[k]), &v)) {
				if (k == 1) printf("display %u: no brightness/brgt\n", s);
				continue;
			}
			float n = v + d;
			n = n < 0.02f ? 0.02f : n > 1 ? 1 : n;
			int sr = setF(s, 0, S(keys[k]), n);
			printf("display %u %s %.3f -> %.3f: %#x\n", s, keys[k], v, n, sr);
			break;
		}
	}
}

int main(int argc, char **argv) {
	void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	void *ds = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", 1);
	void *sys = dlopen("/usr/lib/libSystem.B.dylib", 1);
	if (!iok || !cf || !cg || !sys) { printf("dlopen failed\n"); return 1; }
	sleep = dlsym(sys, "sleep"); atof = dlsym(sys, "atof"); fflush = dlsym(sys, "fflush");
	cfstr = dlsym(cf, "CFStringCreateWithCString"); cfshow = dlsym(cf, "CFShow");
	matching = dlsym(iok, "IOServiceMatching"); getServices = dlsym(iok, "IOServiceGetMatchingServices");
	iterNext = dlsym(iok, "IOIteratorNext"); copyProp = dlsym(iok, "IORegistryEntryCreateCFProperty");
	getF = dlsym(iok, "IODisplayGetFloatParameter"); setF = dlsym(iok, "IODisplaySetFloatParameter");
	mainId = dlsym(cg, "CGMainDisplayID");
	if (ds) { dsGet = dlsym(ds, "DisplayServicesGetBrightness"); dsSet = dlsym(ds, "DisplayServicesSetBrightness"); }
	if (argc > 2 && argv[1][0] == 's') {
		step(atof(argv[2]));
		return 0;
	}
	printf("DisplayServices %p get %p set %p\n", ds, dsGet, dsSet);

	each(-1);
	float steps[] = { 0.3f, 1.0f }, one[1];
	float *lv = steps; int n = 2;
	if (argc > 1) { one[0] = atof(argv[1]); lv = one; n = 1; }
	for (int i = 0; i < n; i++) {
		if (dsGet && dsSet) {
			float v = -1, v2 = -1;
			int gr = dsGet(mainId(), &v), sr = dsSet(mainId(), lv[i]);
			dsGet(mainId(), &v2);
			printf("DisplayServices main %u get %d %.3f  set(%.2f) %d  now %.3f\n", mainId(), gr, v, lv[i], sr, v2);
		}
		sleep(3);
		each(lv[i]);
		sleep(3);
	}
	return 0;
}
