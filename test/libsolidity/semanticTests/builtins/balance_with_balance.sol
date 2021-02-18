contract ClientReceipt {
    constructor() payable {}
}
// ====
// compileViaYul: also
// ----
// constructor(), 1000 wei ->
// contract.balance -> 1000
