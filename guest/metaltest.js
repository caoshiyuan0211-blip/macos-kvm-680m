// Metal smoke test (JXA): list devices, compile an MSL kernel at runtime, run it on each device and
// check the output. Usage: osascript -l JavaScript metaltest.js
ObjC.import('Metal');
ObjC.import('Foundation');
function run() {
	var out = [];
	var devs = $.MTLCopyAllDevices();
	var n = devs.js ? devs.count : 0;
	out.push('devices: ' + n);
	for (var i = 0; i < n; i++) {
		var d = devs.objectAtIndex(i);
		out.push('[' + i + '] ' + d.name.js + ' lowPower=' + d.isLowPower + ' headless=' + d.isHeadless +
			' removable=' + d.isRemovable + ' unified=' + d.hasUnifiedMemory + ' maxBuf=' + d.maxBufferLength +
			' workingSet=' + d.recommendedMaxWorkingSetSize);
		var src = '#include <metal_stdlib>\nusing namespace metal;\n' +
			'kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * 3 + 7; }';
		console.log('step compile'); var err = Ref();
		var lib = d.newLibraryWithSourceOptionsError($(src), $(), err);
		if (!lib.js) { out.push('  compile failed: ' + (err[0] ? err[0].localizedDescription.js : '?')); continue; }
		console.log('step function'); var fn = lib.newFunctionWithName($('k'));
		console.log('step pipeline'); var pso = d.newComputePipelineStateWithFunctionError(fn, err);
		if (!pso.js) { out.push('  pipeline failed'); continue; }
		console.log('step buffers'); var N = 32;
		var init = $.NSMutableData.dataWithLength(N * 4); init.resetBytesInRange($.NSMakeRange(0, N * 4)); var pat = $.NSString.alloc.initWithString('ABCD'.repeat(N)).dataUsingEncoding(4); var buf = d.newBufferWithBytesLengthOptions(pat.bytes, N * 4, 0);
		console.log('step queue'); var priv = d.newBufferWithLengthOptions(N * 4, 0x20); var q = d.newCommandQueue;
		var cb = q.commandBuffer;
		var enc = cb.computeCommandEncoder;
		enc.setComputePipelineState(pso);
		enc['setBuffer:offset:atIndex:'](priv, 0, 0);
		enc['dispatchThreadgroups:threadsPerThreadgroup:']($.MTLSizeMake(1, 1, 1), $.MTLSizeMake(N, 1, 1));
		console.log('step encoded'); enc.endEncoding; var bl = cb.blitCommandEncoder; bl['copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:'](priv, 0, buf, 0, N * 4); bl.endEncoding;
		var t0 = $.NSDate.date;
		console.log('step commit'); cb.commit;
		console.log('step waiting'); cb.waitUntilCompleted;
		console.log('completed'); var ms = -t0.timeIntervalSinceNow * 1000;
		var data = $.NSData.dataWithBytesLength(buf.contents, N * 4);
		var hex = data.description.js.replace(/^.*bytes = 0x|[{}<> ]|length.*?=\s*\d+,?/g, '');
		var bad = 0, first = [];
		for (var j = 0; j < N; j++) {
			var h = hex.substr(j * 8, 8);
			var v = parseInt(h.substr(6, 2) + h.substr(4, 2) + h.substr(2, 2) + h.substr(0, 2), 16);
			if (j < 4) first.push(v);
			if (v !== j * 3 + 7) bad++;
		}
		out.push('  first values: ' + first.join(',') + ' raw: ' + data.description.js.substr(0, 120));
		out.push('  compute: status=' + cb.status + ' error=' + (cb.error.js ? cb.error.localizedDescription.js : 'none') +
			' time=' + ms.toFixed(2) + 'ms gpu=' + ((cb.GPUEndTime - cb.GPUStartTime) * 1e6).toFixed(1) + 'us exec=' + pso.threadExecutionWidth + ' maxTG=' + pso.maxTotalThreadsPerThreadgroup + ' mismatches=' + bad + '/' + N);
	}
	return out.join('\n');
}
