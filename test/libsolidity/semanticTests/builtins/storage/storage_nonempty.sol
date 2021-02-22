contract StorageNotEmpty {
    uint256 x;
    function set(uint256 _a) public { x = _a; }
}
// ====
// compileViaYul: also
// ----
// storage.isEmpty -> true
// set(uint256): 1 ->
// storage.isEmpty -> false
