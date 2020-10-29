function l() {
    s();
}
function s() {
	new C();
}
contract D {
	function f() public {
		l();
	}
}
contract C {
	constructor() { new D(); }
}
// ----
// TypeError 7813: (91-92): Circular reference for free function code access.
