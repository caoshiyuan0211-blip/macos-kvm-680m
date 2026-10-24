// Native Metal texture-path test (no SDK: Objective-C runtime via dlsym). Fills RGBA8Uint textures
// with pixel (x,y) = {x&255, x>>8, y&255, y>>8} through several upload paths and reads them back
// through several download paths; reports mismatches and where the first bad pixel's value came from.
// Usage: textest [r] [x] [size...]   (default 16 64 100 128 256 512 1024)
//   r = render-target cases only; x = first patch addrlib Gfx10 HwlComputePipeBankXor in the loaded
//   AMDRadeonX6000MTLDriver so every surface gets pipeBankXor 0 (icon test: does XOR 0 fix managed paths?)
#include <stddef.h>
#include <stdint.h>

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int printf(const char *, ...);
int atoi(const char *);
void *malloc(size_t);
void free(void *);
char *strstr(const char *, const char *);
unsigned _dyld_image_count(void);
const char *_dyld_get_image_name(unsigned);
const void *_dyld_get_image_header(unsigned);
extern unsigned mach_task_self_;
int vm_protect(unsigned, unsigned long, unsigned long, int, int);

typedef void *id;
typedef void *SEL;
typedef unsigned long ul;
typedef struct { ul width, height, depth; } MTLSize;
typedef struct { ul x, y, z; } MTLOrigin;
typedef struct { MTLOrigin origin; MTLSize size; } MTLRegion;

static id (*msg)(id, SEL, ...);
static SEL (*sel)(const char *);
static id (*cls)(const char *);
#define S(x) sel(x)

enum { RGBA8Uint = 73, Shared = 0, Managed = 1, Private = 2, UsRead = 1, UsWrite = 2, UsRT = 4 };

static id dev, q, psoRead, psoWrite, rpso;
static ul onlyRender;

static id nsstr(const char *s) { return msg(cls("NSString"), S("stringWithUTF8String:"), s); }

static uint32_t expect(ul x, ul y) { return (x & 255) | (x >> 8) << 8 | (y & 255) << 16 | (y >> 8) << 24; }

static id newTex(ul w, ul h, ul storage, ul usage) {
	id d = msg(cls("MTLTextureDescriptor"), S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
		(ul)RGBA8Uint, w, h, (ul)0);
	msg(d, S("setStorageMode:"), storage);
	msg(d, S("setUsage:"), usage);
	return msg(dev, S("newTextureWithDescriptor:"), d);
}

static void run(id cb) {
	msg(cb, S("commit"));
	msg(cb, S("waitUntilCompleted"));
	ul st = (ul)msg(cb, S("status"));
	if (st != 4)
		printf("  cb status %lu\n", st);
}

static void dispatch2d(id pso, id tex, id buf, ul w, ul h) {
	uint32_t wh[2] = {(uint32_t)w, (uint32_t)h};
	id cb = msg(q, S("commandBuffer"));
	id enc = msg(cb, S("computeCommandEncoder"));
	msg(enc, S("setComputePipelineState:"), pso);
	msg(enc, S("setTexture:atIndex:"), tex, 0UL);
	msg(enc, S("setBuffer:offset:atIndex:"), buf, 0UL, 0UL);
	msg(enc, S("setBytes:length:atIndex:"), wh, 8UL, 1UL);
	MTLSize g = {(w + 7) / 8, (h + 7) / 8, 1}, t = {8, 8, 1};
	((void (*)(id, SEL, MTLSize, MTLSize))msg)(enc, S("dispatchThreadgroups:threadsPerThreadgroup:"), g, t);
	msg(enc, S("endEncoding"));
	run(cb);
}

static void blitBufToTex(id buf, id tex, ul w, ul h) {
	id cb = msg(q, S("commandBuffer"));
	id enc = msg(cb, S("blitCommandEncoder"));
	MTLSize sz = {w, h, 1};
	MTLOrigin o = {0, 0, 0};
	((void (*)(id, SEL, id, ul, ul, ul, MTLSize, id, ul, ul, MTLOrigin))msg)(enc,
		S("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
		buf, 0UL, w * 4, w * h * 4, sz, tex, 0UL, 0UL, o);
	msg(enc, S("endEncoding"));
	run(cb);
}

static void blitTexToBuf(id tex, id buf, ul w, ul h) {
	id cb = msg(q, S("commandBuffer"));
	id enc = msg(cb, S("blitCommandEncoder"));
	MTLSize sz = {w, h, 1};
	MTLOrigin o = {0, 0, 0};
	((void (*)(id, SEL, id, ul, ul, MTLOrigin, MTLSize, id, ul, ul, ul))msg)(enc,
		S("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
		tex, 0UL, 0UL, o, sz, buf, 0UL, w * 4, w * h * 4);
	msg(enc, S("endEncoding"));
	run(cb);
}

static void blitTexToTex(id src, id dst) {
	id cb = msg(q, S("commandBuffer"));
	id enc = msg(cb, S("blitCommandEncoder"));
	msg(enc, S("copyFromTexture:toTexture:"), src, dst);
	msg(enc, S("endEncoding"));
	run(cb);
}

static void fillPattern(uint32_t *p, ul w, ul h) {
	for (ul y = 0; y < h; y++)
		for (ul x = 0; x < w; x++)
			p[y * w + x] = expect(x, y);
}

static void check(const char *what, const uint32_t *p, ul w, ul h) {
	ul bad = 0, fx = 0, fy = 0;
	uint32_t fv = 0;
	for (ul y = 0; y < h; y++)
		for (ul x = 0; x < w; x++)
			if (p[y * w + x] != expect(x, y) && !bad++)
				fx = x, fy = y, fv = p[y * w + x];
	if (!bad) {
		printf("  %-34s OK\n", what);
		return;
	}
	// Decode where the first bad pixel's value came from, if it looks like a pattern pixel.
	ul sx = (fv & 255) | ((fv >> 8) & 255) << 8, sy = ((fv >> 16) & 255) | (fv >> 24) << 8;
	printf("  %-34s BAD %lu/%lu, first (%lu,%lu) = %08x", what, bad, w * h, fx, fy, fv);
	if (sx < w && sy < h)
		printf(" = pixel (%lu,%lu)", sx, sy);
	printf("\n");
	// Where each power-of-two probe point's value came from, to decode the swizzle permutation.
	printf("    map:");
	for (ul k = 0; k < 25; k++) {
		ul b = k ? 1UL << (k - 1) / 2 : 0, px = k & 1 ? b : 0, py = k & 1 ? 0 : b;
		if (px >= w || py >= h)
			continue;
		uint32_t v = p[py * w + px];
		ul vx = (v & 255) | ((v >> 8) & 255) << 8, vy = ((v >> 16) & 255) | (v >> 24) << 8;
		if (vx < w && vy < h)
			printf(" (%lu,%lu)<-(%lu,%lu)", px, py, vx, vy);
		else
			printf(" (%lu,%lu)=%08x", px, py, v);
	}
	printf("\n");
	// Coarse 8x8-tile map of the first 16x16 tiles: '.' ok, 'x' bad.
	ul tw = (w + 7) / 8 < 32 ? (w + 7) / 8 : 32, th = (h + 7) / 8 < 16 ? (h + 7) / 8 : 16;
	for (ul ty = 0; ty < th; ty++) {
		printf("    ");
		for (ul tx = 0; tx < tw; tx++) {
			int b = 0;
			for (ul y = ty * 8; y < ty * 8 + 8 && y < h; y++)
				for (ul x = tx * 8; x < tx * 8 + 8 && x < w; x++)
					b |= p[y * w + x] != expect(x, y);
			printf("%c", b ? 'x' : '.');
		}
		printf("\n");
	}
}

static void clear(uint32_t *p, ul n) {
	for (ul i = 0; i < n; i++)
		p[i] = 0xDEADBEEF;
}

typedef struct { double r, g, b, a; } MTLClearColor;

// Render pass into tex: loadAction Clear (to clear color c) and, if draw, a full-screen triangle whose
// fragment shader writes the pattern (CB path, which can compress with DCC).
static void renderPass(id tex, int draw, uint32_t c) {
	id rp = msg(cls("MTLRenderPassDescriptor"), S("renderPassDescriptor"));
	id att = msg(msg(rp, S("colorAttachments")), S("objectAtIndexedSubscript:"), 0UL);
	msg(att, S("setTexture:"), tex);
	msg(att, S("setLoadAction:"), 2UL);
	msg(att, S("setStoreAction:"), 1UL);
	MTLClearColor cc = {c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24};
	((void (*)(id, SEL, MTLClearColor))msg)(att, S("setClearColor:"), cc);
	id cb = msg(q, S("commandBuffer"));
	id enc = msg(cb, S("renderCommandEncoderWithDescriptor:"), rp);
	if (draw) {
		msg(enc, S("setRenderPipelineState:"), rpso);
		msg(enc, S("drawPrimitives:vertexStart:vertexCount:"), 3UL, 0UL, 3UL);
	}
	msg(enc, S("endEncoding"));
	run(cb);
}

static void checkConst(const char *what, const uint32_t *p, ul n, uint32_t c) {
	ul bad = 0;
	for (ul i = 0; i < n; i++)
		bad += p[i] != c;
	if (bad)
		printf("  %-34s BAD %lu/%lu, first %08x (want %08x)\n", what, bad, n, p[0], c);
	else
		printf("  %-34s OK\n", what);
}

static void testRender(ul w, ul h, ul us) {
	ul n = w * h;
	id out = msg(dev, S("newBufferWithLength:options:"), n * 4, 0UL);
	uint32_t *o = (uint32_t *)msg(out, S("contents"));
	uint32_t c = 0x04030201;
	id t = newTex(w, h, Private, us);
	renderPass(t, 0, c);
	clear(o, n);
	dispatch2d(psoRead, t, out, w, h);
	checkConst("private render clear -> kernel", o, n, c);
	clear(o, n);
	blitTexToBuf(t, out, w, h);
	checkConst("private render clear -> blit", o, n, c);
	renderPass(t, 1, c);
	clear(o, n);
	dispatch2d(psoRead, t, out, w, h);
	check("private render draw -> kernel", o, w, h);
	clear(o, n);
	blitTexToBuf(t, out, w, h);
	check("private render draw -> blit", o, w, h);
	msg(t, S("release"));
	msg(out, S("release"));
}

static void testSize(ul w, ul h) {
	printf("size %lux%lu\n", w, h);
	ul n = w * h;
	MTLRegion r = {{0, 0, 0}, {w, h, 1}};
	uint32_t *pat = malloc(n * 4), *tmp = malloc(n * 4);
	fillPattern(pat, w, h);
	id src = msg(dev, S("newBufferWithBytes:length:options:"), pat, n * 4, 0UL);
	id out = msg(dev, S("newBufferWithLength:options:"), n * 4, 0UL);
	uint32_t *o = (uint32_t *)msg(out, S("contents"));
	void (*replace)(id, SEL, MTLRegion, ul, const void *, ul) = (void (*)(id, SEL, MTLRegion, ul, const void *, ul))msg;
	void (*getBytes)(id, SEL, void *, ul, MTLRegion, ul) = (void (*)(id, SEL, void *, ul, MTLRegion, ul))msg;

	for (int rt = 0; rt < 2; rt++) {
		ul us = UsRead | UsWrite | (rt ? UsRT : 0);
		printf(" usage %s\n", rt ? "read|write|RT (DCC-able)" : "read|write");
		if (rt)
			testRender(w, h, us);
		if (onlyRender)
			continue;

		id tm = newTex(w, h, Managed, us);
		replace(tm, S("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), r, 0UL, pat, w * 4);
		clear(tmp, n);
		getBytes(tm, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"), tmp, w * 4, r, 0UL);
		check("managed replace -> getBytes", tmp, w, h);
		clear(o, n);
		dispatch2d(psoRead, tm, out, w, h);
		check("managed replace -> kernel read", o, w, h);
		clear(o, n);
		blitTexToBuf(tm, out, w, h);
		check("managed replace -> blit to buf", o, w, h);

		id tp = newTex(w, h, Private, us);
		blitBufToTex(src, tp, w, h);
		clear(o, n);
		dispatch2d(psoRead, tp, out, w, h);
		check("private blit from buf -> kernel", o, w, h);
		clear(o, n);
		blitTexToBuf(tp, out, w, h);
		check("private blit from buf -> blit", o, w, h);

		id tw = newTex(w, h, Private, us);
		dispatch2d(psoWrite, tw, out, w, h);
		clear(o, n);
		blitTexToBuf(tw, out, w, h);
		check("private kernel write -> blit", o, w, h);
		clear(o, n);
		dispatch2d(psoRead, tw, out, w, h);
		check("private kernel write -> kernel", o, w, h);

		id t2 = newTex(w, h, Private, us);
		blitTexToTex(tm, t2);
		clear(o, n);
		dispatch2d(psoRead, t2, out, w, h);
		check("managed -> tex2tex blit -> kernel", o, w, h);

		msg(tm, S("release"));
		msg(tp, S("release"));
		msg(tw, S("release"));
		msg(t2, S("release"));
	}
	msg(src, S("release"));
	msg(out, S("release"));
	free(pat);
	free(tmp);
}

static id pipeline(id lib, const char *name) {
	id err = 0;
	id fn = msg(lib, S("newFunctionWithName:"), nsstr(name));
	id pso = msg(dev, S("newComputePipelineStateWithFunction:error:"), fn, &err);
	if (!pso)
		printf("pipeline %s failed\n", name);
	return pso;
}

// HwlComputePipeBankXor tail: movslq (%rdi,%rax,4),%rax; add %rdi,%rax; mov 4(%rsi),%esi; and $7,%esi;
// [mov (%rax,%rsi,4),%eax; add $2,%cl; shl %cl,%eax] -> xor %eax,%eax + nops; mov %eax,4(%rdx)
static void patchXor0(void) {
	static const unsigned char pat[] = {0x48, 0x63, 0x04, 0x87, 0x48, 0x01, 0xf8, 0x8b, 0x76, 0x04, 0x83, 0xe6, 0x07, 0x8b,
		0x04, 0xb0, 0x80, 0xc1, 0x02, 0xd3, 0xe0, 0x89, 0x42, 0x04};
	static const unsigned char rep[] = {0x31, 0xc0, 0x0f, 0x1f, 0x40, 0x00, 0x66, 0x90};
	for (unsigned i = 0, n = _dyld_image_count(); i < n; i++) {
		const char *name = _dyld_get_image_name(i);
		if (!name || !strstr(name, "AMDRadeonX6000MTLDriver"))
			continue;
		const unsigned char *h = _dyld_get_image_header(i);
		ul text = 0;
		const unsigned char *lc = h + 32;
		for (unsigned c = 0, nc = *(const unsigned *)(h + 16); c < nc; c++, lc += *(const unsigned *)(lc + 4))
			if (*(const unsigned *)lc == 0x19 && !strstr((const char *)lc + 8, "__TEXT_") && strstr((const char *)lc + 8, "__TEXT"))
				text = *(const ul *)(lc + 32);  // vmsize
		int hits = 0;
		unsigned char *at = 0;
		for (ul o = 0; o + sizeof pat <= text; o++) {
			ul k = 0;
			while (k < sizeof pat && h[o + k] == pat[k])
				k++;
			if (k == sizeof pat)
				hits++, at = (unsigned char *)h + o + 13;
		}
		printf("xor0: %s header %p __TEXT 0x%lx, pattern hits %d\n", name, h, text, hits);
		if (hits != 1)
			return;
		ul pg = (ul)at & ~0xfffUL;
		int r = vm_protect(mach_task_self_, pg, 0x2000, 0, 1 | 2 | 0x10);  // READ|WRITE|COPY
		if (r) {
			printf("xor0: vm_protect rw failed %d\n", r);
			return;
		}
		for (unsigned k = 0; k < sizeof rep; k++)
			at[k] = rep[k];
		r = vm_protect(mach_task_self_, pg, 0x2000, 0, 1 | 4);
		printf("xor0: patched HwlComputePipeBankXor at %p (rx back: %d) -> every surface pipeBankXor 0\n", at, r);
		return;
	}
	printf("xor0: AMDRadeonX6000MTLDriver not loaded\n");
}

int main(int argc, char **argv) {
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
	dev = createDevice();
	if (!dev) {
		printf("no Metal device\n");
		return 1;
	}
	printf("device: %s\n", (const char *)msg(msg(dev, S("name")), S("UTF8String")));
	int a0 = 1;
	if (argc > a0 && argv[a0][0] == 'r' && !argv[a0][1])
		onlyRender = 1, a0++;
	if (argc > a0 && argv[a0][0] == 'x' && !argv[a0][1])
		patchXor0(), a0++;

	const char *src = "#include <metal_stdlib>\nusing namespace metal;\n"
		"kernel void rd(texture2d<uint, access::read> t [[texture(0)]], device uint *o [[buffer(0)]],\n"
		"  constant uint2 &wh [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
		"  if (g.x >= wh.x || g.y >= wh.y) return;\n"
		"  uint4 c = t.read(g); o[g.y * wh.x + g.x] = c.x | c.y << 8 | c.z << 16 | c.w << 24; }\n"
		"kernel void wr(texture2d<uint, access::write> t [[texture(0)]], device uint *o [[buffer(0)]],\n"
		"  constant uint2 &wh [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
		"  if (g.x >= wh.x || g.y >= wh.y) return;\n"
		"  t.write(uint4(g.x & 255, g.x >> 8, g.y & 255, g.y >> 8), g); }\n"
		"vertex float4 vs(uint v [[vertex_id]]) {\n"
		"  return float4(v == 1 ? 3.0 : -1.0, v == 2 ? 3.0 : -1.0, 0.0, 1.0); }\n"
		"fragment uint4 fs(float4 p [[position]]) {\n"
		"  uint x = uint(p.x), y = uint(p.y); return uint4(x & 255, x >> 8, y & 255, y >> 8); }\n";
	id err = 0;
	id lib = msg(dev, S("newLibraryWithSource:options:error:"), nsstr(src), (id)0, &err);
	if (!lib) {
		printf("compile failed: %s\n", err ? (const char *)msg(msg(err, S("description")), S("UTF8String")) : "?");
		return 1;
	}
	psoRead = pipeline(lib, "rd");
	psoWrite = pipeline(lib, "wr");
	if (!psoRead || !psoWrite)
		return 1;
	q = msg(dev, S("newCommandQueue"));
	id rd = msg(msg(cls("MTLRenderPipelineDescriptor"), S("alloc")), S("init"));
	msg(rd, S("setVertexFunction:"), msg(lib, S("newFunctionWithName:"), nsstr("vs")));
	msg(rd, S("setFragmentFunction:"), msg(lib, S("newFunctionWithName:"), nsstr("fs")));
	msg(msg(msg(rd, S("colorAttachments")), S("objectAtIndexedSubscript:"), 0UL), S("setPixelFormat:"), (ul)RGBA8Uint);
	rpso = msg(dev, S("newRenderPipelineStateWithDescriptor:error:"), rd, &err);
	if (!rpso) {
		printf("render pipeline failed: %s\n", err ? (const char *)msg(msg(err, S("description")), S("UTF8String")) : "?");
		return 1;
	}

	static const ul defSizes[] = {16, 64, 100, 128, 256, 512, 1024};
	if (argc > a0)
		for (int i = a0; i < argc; i++)
			testSize(atoi(argv[i]), atoi(argv[i]));
	else
		for (unsigned i = 0; i < sizeof defSizes / sizeof *defSizes; i++)
			testSize(defSizes[i], defSizes[i]);
	return 0;
}
