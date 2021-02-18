contract ClientReceipt {
    constructor() payable {}
}
// ====
// compileViaYul: also
// ----
// constructor(), 1 ether ->
// contract.balance -> 1000000000000000000
