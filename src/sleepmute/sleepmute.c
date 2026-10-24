// Guest sleep-entry input mute: sleep entry takes ~40 s in the VM and any HID event in that window
// aborts it ("Requesting full wake due to dark wake activity tickle", auto-1022-1811: mouse 0.55 s
// after Apple menu Sleep). On kIOMessageSystemWillSleep we send UDP "mute" to the host
// (10.0.2.2:7779 = host 127.0.0.1:7779, scripts/sleepsync.py), which sets QEMU input-linux mute-ms;
// on WillNotSleep/WillPowerOn/HasPoweredOn we send "unmute". Root LaunchDaemon com.rmb.sleepmute.
// No SDK: libSystem/IOKit/CoreFoundation via dlsym. Usage: sleepmute [port]   |   sleepmute send WORD
#include <stdint.h>
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int printf(const char *, ...);
typedef const void *R;
static int (*fflush_)(void *);
static int (*atoi_)(const char *);
static long (*sendto_)(int, const void *, unsigned long, int, const void *, uint32_t);
static int (*strlen_)(const char *);
static int (*allow)(uint32_t, intptr_t);
static uint32_t root;
static int sock = -1;
static uint8_t host[16];
#define LOG(...) (printf(__VA_ARGS__), fflush_(0))

static void tell(const char *w) {
	long n = sendto_(sock, w, strlen_(w), 0, host, 16);
	LOG("sleepmute: sent %s (%ld)\n", w, n);
}

static void power(void *ref, uint32_t svc, uint32_t msg, void *arg) {
	switch (msg) {
	case 0xe0000270:  // kIOMessageCanSystemSleep
		allow(root, (intptr_t)arg);
		break;
	case 0xe0000280:  // kIOMessageSystemWillSleep
		tell("mute");
		allow(root, (intptr_t)arg);
		break;
	case 0xe0000290:  // kIOMessageSystemWillNotSleep
	case 0xe0000320:  // kIOMessageSystemWillPowerOn
	case 0xe0000300:  // kIOMessageSystemHasPoweredOn
		tell("unmute");
		break;
	}
}

int main(int argc, char **argv) {
	void *sys = dlopen("/usr/lib/libSystem.B.dylib", 1);
	void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", 1);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 1);
	fflush_ = dlsym(sys, "fflush");
	atoi_ = dlsym(sys, "atoi");
	strlen_ = dlsym(sys, "strlen");
	sendto_ = dlsym(sys, "sendto");
	int (*socket_)(int, int, int) = dlsym(sys, "socket");
	uint32_t (*reg)(void *, R *, void *, uint32_t *) = dlsym(io, "IORegisterForSystemPower");
	R (*portSrc)(R) = dlsym(io, "IONotificationPortGetRunLoopSource");
	allow = dlsym(io, "IOAllowPowerChange");
	R (*curLoop)(void) = dlsym(cf, "CFRunLoopGetCurrent");
	void (*addSrc)(R, R, R) = dlsym(cf, "CFRunLoopAddSource");
	void (*run)(void) = dlsym(cf, "CFRunLoopRun");
	R *defMode = dlsym(cf, "kCFRunLoopDefaultMode");

	int port = 7779;
	int sendOnly = argc > 2 && argv[1][0] == 's';
	if (argc > 1 && !sendOnly)
		port = atoi_(argv[1]);
	sock = socket_(2, 2, 0);  // AF_INET, SOCK_DGRAM
	uint8_t sa[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, 10, 0, 2, 2};  // sockaddr_in 10.0.2.2
	for (int i = 0; i < 16; i++)
		host[i] = sa[i];
	if (sendOnly) {
		tell(argv[2]);
		return 0;
	}
	R notifyPort = 0;
	uint32_t notifier = 0;
	root = reg(0, &notifyPort, (void *)power, &notifier);
	if (!root) {
		LOG("sleepmute: IORegisterForSystemPower failed\n");
		return 1;
	}
	addSrc(curLoop(), portSrc(notifyPort), *defMode);
	LOG("sleepmute: watching system power, host udp %d\n", port);
	run();
	return 0;
}
