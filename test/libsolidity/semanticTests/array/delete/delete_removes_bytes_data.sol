contract c {
    fallback() external { data = msg.data; }
    function del() public returns (bool) { delete data; return true; }
    bytes data;
}
// ====
// compileViaYul: also
// ----
// (): 7 ->
// storage.isEmpty -> false
// del(): 7 -> true
// storage.isEmpty -> true
