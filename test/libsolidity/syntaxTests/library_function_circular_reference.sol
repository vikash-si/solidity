library L{
	function f() internal {
		new C();
	}
}

contract D {
	function f() public {
		L.f();
	}
}
contract C {
	constructor() { new D(); }
}

// ----
// TypeError 4579: (133-138): Circular reference for contract creation (cannot create instance of derived or same contract).
