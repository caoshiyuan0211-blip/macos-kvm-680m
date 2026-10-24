// Metal blit test (JXA): fill a shared buffer and copy between shared buffers, then read back.
ObjC.import('Metal');
ObjC.import('Foundation');
function hexOf(buf, n) { return $.NSData.dataWithBytesLength(buf.contents, n).description.js.substr(0, 110); }
function run() {
	var d = $.MTLCopyAllDevices().objectAtIndex(0);
	var out = [d.name.js];
	var pat = $.NSString.alloc.initWithString('ABCD'.repeat(64)).dataUsingEncoding(4);
	var a = d.newBufferWithBytesLengthOptions(pat.bytes, 256, 0);
	var b = d.newBufferWithLengthOptions(256, 0);
	var c = d.newBufferWithLengthOptions(256, 0);
	var q = d.newCommandQueue;
	var cb = q.commandBuffer;
	var bl = cb.blitCommandEncoder;
	bl['copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:'](a, 0, b, 0, 256);
	bl['fillBuffer:range:value:'](c, $.NSMakeRange(0, 256), 0x5a);
	bl.endEncoding;
	cb.commit;
	cb.waitUntilCompleted;
	out.push('status=' + cb.status + ' error=' + (cb.error.js ? cb.error.localizedDescription.js : 'none'));
	out.push('copy dst: ' + hexOf(b, 256));
	out.push('fill dst: ' + hexOf(c, 256));
	return out.join('\n');
}
