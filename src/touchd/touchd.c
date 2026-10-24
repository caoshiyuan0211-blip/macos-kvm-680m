// Guest touchscreen daemon: the host (scripts/touch-host.py) reads the GPD touchscreen and sends UDP
// packets {u8 type 0 move / 1 down / 2 up / 3 right-click, u8 pad, u16 x, u16 y} (x/y 0..65535 over the
// panel, already rotated) to port 7778; we post mouse events (they move the cursor; CGWarpMouseCursorPosition
// would swallow the next real mouse move) on the GPD panel
// (EDID vendor 0x2889, else the main display). No virtual tablet: macOS snaps the cursor back to a
// tablet's last position on every later mouse click (2026-10-22 test), so touch can't be a 2nd pointer.
// No SDK: libSystem/CoreGraphics via dlsym. Usage: touchd [port]   |   touchd test X Y (one click)
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
typedef const void *R;
typedef uint32_t D;
typedef struct { double x, y; } P;
typedef struct { double x, y, w, h; } Rect;
static int (*fflush_)(void *);
static int (*atoi_)(const char *);
static R (*mouseEv)(R, uint32_t, P, uint32_t);
static void (*post)(uint32_t, R);
static void (*cfRelease)(R);
static void (*setInt)(R, uint32_t, int64_t);
static int (*activeList)(uint32_t, D *, uint32_t *);
static uint32_t (*vendor)(D);
static D (*mainDisp)(void);
static Rect (*bounds)(D);
static double (*now)(void);
#define LOG(...) (printf(__VA_ARGS__), fflush_(0))

enum { LDown = 1, LUp = 2, RDown = 3, RUp = 4, Moved = 5, LDrag = 6 };

static Rect panel(void) {
	D d[16];
	uint32_t n = 0;
	activeList(16, d, &n);
	for (uint32_t i = 0; i < n; i++)
		if (vendor(d[i]) == 0x2889)
			return bounds(d[i]);
	return bounds(mainDisp());
}

static void send(uint32_t type, P p, int64_t clicks) {
	R e = mouseEv(0, type, p, type == RDown || type == RUp ? 1 : 0);
	if (clicks)
		setInt(e, 1, clicks);  // kCGMouseEventClickState
	post(0, e);              // kCGHIDEventTap
	cfRelease(e);
}

int main(int argc, char **argv) {
	void *sys = dlopen("/usr/lib/libSystem.B.dylib", 1);
	void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	fflush_ = dlsym(sys, "fflush");
	atoi_ = dlsym(sys, "atoi");
	int (*sock)(int, int, int) = dlsym(sys, "socket");
	int (*bind_)(int, const void *, uint32_t) = dlsym(sys, "bind");
	long (*recv_)(int, void *, unsigned long, int) = dlsym(sys, "recv");
	mouseEv = dlsym(cg, "CGEventCreateMouseEvent");
	post = dlsym(cg, "CGEventPost");
	setInt = dlsym(cg, "CGEventSetIntegerValueField");
	activeList = dlsym(cg, "CGGetActiveDisplayList");
	vendor = dlsym(cg, "CGDisplayVendorNumber");
	mainDisp = dlsym(cg, "CGMainDisplayID");
	bounds = dlsym(cg, "CGDisplayBounds");
	cfRelease = dlsym(cf, "CFRelease");
	now = dlsym(cf, "CFAbsoluteTimeGetCurrent");
	Rect r = panel();
	LOG("touchd: panel at %.0f,%.0f %.0fx%.0f\n", r.x, r.y, r.w, r.h);
	if (argc > 3 && argv[1][0] == 't') {
		P p = {atoi_(argv[2]), atoi_(argv[3])};
		send(Moved, p, 0);
		send(LDown, p, 1);
		send(LUp, p, 1);
		LOG("touchd: test click at %.0f,%.0f\n", p.x, p.y);
		return 0;
	}
	int s = sock(2, 2, 0);  // AF_INET, SOCK_DGRAM
	int port = argc > 1 ? atoi_(argv[1]) : 7778;
	uint8_t sa[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port};  // sockaddr_in, INADDR_ANY
	if (s < 0 || bind_(s, sa, 16) < 0) {
		LOG("touchd: can't bind udp %d\n", port);
		return 1;
	}
	int down = 0, clicks = 0;
	double lastUp = 0, lastPanel = now();
	P downAt = {0, 0}, cur = {0, 0};
	for (;;) {
		uint8_t b[8];
		if (recv_(s, b, sizeof b, 0) < 6)
			continue;
		if (now() - lastPanel > 2) {  // displays may be rearranged
			r = panel();
			lastPanel = now();
		}
		uint32_t x = b[2] | b[3] << 8, y = b[4] | b[5] << 8;
		P p = {r.x + x * (r.w - 1) / 65535.0, r.y + y * (r.h - 1) / 65535.0};
		cur = p;
		switch (b[0]) {
		case 1:
			// double click: 2nd tap within 0.5 s and 12 px of the 1st
			if (now() - lastUp < 0.5 && (p.x - downAt.x) * (p.x - downAt.x) + (p.y - downAt.y) * (p.y - downAt.y) < 144)
				clicks++;
			else
				clicks = 1;
			downAt = p;
			send(LDown, p, clicks);
			down = 1;
			break;
		case 0:
			if (down)
				send(LDrag, p, clicks);
			else
				send(Moved, p, 0);
			break;
		case 2:
			if (down)
				send(LUp, cur, clicks);
			down = 0;
			lastUp = now();
			break;
		case 3:
			send(RDown, p, 1);
			send(RUp, p, 1);
			break;
		}
	}
}
