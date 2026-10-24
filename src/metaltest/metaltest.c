// Native Metal compute test (no SDK: Objective-C runtime via dlsym). Dispatches a kernel writing
// o[i] = i * 3 + 7 over a 3x5x7 grid of 32-thread groups and checks every element.
// Usage: metaltest [iterations] [threadgroups] [inner-loop count]
#include <stddef.h>
#include <stdint.h>

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int printf(const char *, ...);
int atoi(const char *);

typedef void *id;
typedef void *SEL;
typedef struct { unsigned long width, height, depth; } MTLSize;

static id (*msg)(id, SEL, ...);
static SEL (*sel)(const char *);
static id (*cls)(const char *);

#define S(x) sel(x)

int main(int argc, char **argv) {
	int iters = argc > 1 ? atoi(argv[1]) : 1;
	unsigned groupsN = argc > 2 ? atoi(argv[2]) : 3 * 5 * 7;
	unsigned loop = argc > 3 ? atoi(argv[3]) : 0;
	void *objc = dlopen("/usr/lib/libobjc.A.dylib", 1);
	dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 1);
	void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", 1);
	if (!objc || !metal) {
		printf("dlopen failed: %s\n", dlerror());
		return 1;
	}
	msg = dlsym(objc, "objc_msgSend");
	sel = dlsym(objc, "sel_registerName");
	cls = dlsym(objc, "objc_getClass");
	id (*createDevice)(void) = dlsym(metal, "MTLCreateSystemDefaultDevice");
	id dev = createDevice();
	if (!dev) {
		printf("no Metal device\n");
		return 1;
	}
	id name = msg(dev, S("name"));
	printf("device: %s\n", (const char *)msg(name, S("UTF8String")));

	const char *src = "#include <metal_stdlib>\nusing namespace metal;\n"
		"kernel void k(device uint *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
		"  float a = i, b = 1.0001f; for (uint j = 0; j < n; j++) a = fma(a, b, 0.5f);\n"
		"  o[i] = i * 3 + 7 + (a == -1.0f ? 1 : 0); }";
	id nsSrc = msg(cls("NSString"), S("stringWithUTF8String:"), src);
	id err = 0;
	id lib = msg(dev, S("newLibraryWithSource:options:error:"), nsSrc, (id)0, &err);
	if (!lib) {
		printf("compile failed\n");
		return 1;
	}
	id fn = msg(lib, S("newFunctionWithName:"), msg(cls("NSString"), S("stringWithUTF8String:"), "k"));
	id pso = msg(dev, S("newComputePipelineStateWithFunction:error:"), fn, &err);
	if (!pso) {
		printf("pipeline failed\n");
		return 1;
	}
	const unsigned N = groupsN * 32;
	id buf = msg(dev, S("newBufferWithLength:options:"), (unsigned long)N * 4, 0UL);
	uint32_t *p = (uint32_t *)msg(buf, S("contents"));
	id q = msg(dev, S("newCommandQueue"));
	MTLSize groups = {groupsN, 1, 1}, threads = {32, 1, 1};
	void (*dispatch)(id, SEL, MTLSize, MTLSize) = (void (*)(id, SEL, MTLSize, MTLSize))msg;
	unsigned long (*status)(id, SEL) = (unsigned long (*)(id, SEL))msg;
	double (*dbl)(id, SEL) = (double (*)(id, SEL))msg;
	int bad = 0;
	for (int it = 0; it < iters; it++) {
		for (unsigned i = 0; i < N; i++)
			p[i] = 0xDEADBEEF;
		id cb = msg(q, S("commandBuffer"));
		id enc = msg(cb, S("computeCommandEncoder"));
		msg(enc, S("setComputePipelineState:"), pso);
		msg(enc, S("setBuffer:offset:atIndex:"), buf, 0UL, 0UL);
		msg(enc, S("setBytes:length:atIndex:"), &loop, 4UL, 1UL);
		dispatch(enc, S("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
		msg(enc, S("endEncoding"));
		msg(cb, S("commit"));
		msg(cb, S("waitUntilCompleted"));
		bad = 0;
		for (unsigned i = 0; i < N; i++)
			bad += p[i] != i * 3 + 7;
		if (it == 0 || it == iters - 1)
			printf("iter %d: status %lu gpu %.1f us (%.1f GFLOPS), first %u %u %u %u, last %u, mismatches %d/%u\n", it,
				status(cb, S("status")), (dbl(cb, S("GPUEndTime")) - dbl(cb, S("GPUStartTime"))) * 1e6,
				2.0 * N * loop / ((dbl(cb, S("GPUEndTime")) - dbl(cb, S("GPUStartTime"))) * 1e9),
				p[0], p[1], p[2], p[3], p[N - 1], bad, N);
	}
	return bad != 0;
}
