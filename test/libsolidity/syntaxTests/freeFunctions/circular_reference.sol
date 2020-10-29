contract D {
	function f() public {
		l();
	}
}
contract C {
	constructor() { new D(); }
}
function l() {
	new C();
}
// ----
// TypeError 7813: (38-39): Circular reference for free function code access.
