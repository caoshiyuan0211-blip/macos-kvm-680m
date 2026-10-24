// Keeps the GPU dispatching a recognizable grid (3x5x7 threadgroups of 32) for a few seconds so the
// kext's 100 ms dispatch sampler sees its registers. Usage: osascript -l JavaScript spintest.js [count]
ObjC.import('Metal');
ObjC.import('Foundation');
function run(argv) {
	var count = argv.length ? parseInt(argv[0]) : 400;
	var d = $.MTLCreateSystemDefaultDevice();
	var src = '#include <metal_stdlib>\nusing namespace metal;\n' +
		'kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * 3 + 7; }';
	var err = Ref();
	var lib = d.newLibraryWithSourceOptionsError($(src), $(), err);
	var pso = d.newComputePipelineStateWithFunctionError(lib.newFunctionWithName($('k')), err);
	var N = 3 * 5 * 7 * 32;
	var buf = d.newBufferWithLengthOptions(N * 4, 0);
	var q = d.newCommandQueue;
	var t0 = $.NSDate.date, last;
	for (var i = 0; i < count; i++) {
		var cb = q.commandBuffer;
		var enc = cb.computeCommandEncoder;
		enc.setComputePipelineState(pso);
		enc['setBuffer:offset:atIndex:'](buf, 0, 0);
		enc['dispatchThreadgroups:threadsPerThreadgroup:']($.MTLSizeMake(3, 5, 7), $.MTLSizeMake(32, 1, 1));
		enc.endEncoding;
		cb.commit;
		if (i % 20 == 19)
			cb.waitUntilCompleted;
		last = cb;
	}
	last.waitUntilCompleted;
	var ms = -t0.timeIntervalSinceNow * 1000;
	var data = $.NSData.dataWithBytesLength(buf.contents, 16);
	return count + ' command buffers in ' + ms.toFixed(0) + ' ms, status ' + last.status + ', first bytes ' + data.description.js;
}
