// What does the JXA bridge make of MTLSize? Usage: osascript -l JavaScript sizetest.js
ObjC.import('Metal');
function run() {
	var out = [];
	var s;
	try { s = $.MTLSizeMake(3, 5, 7); out.push('MTLSizeMake: typeof ' + typeof s + ' json ' + JSON.stringify(s)); }
	catch (e) { out.push('MTLSizeMake threw: ' + e); }
	out.push('typeof $.MTLSizeMake = ' + typeof $.MTLSizeMake);
	return out.join('\n');
}
