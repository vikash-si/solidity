contract ClientReceipt {
}
// ====
// compileViaYul: also
// ----
// smoke.test0 ->
// smoke.test1: 1 -> 1
// smoke.test2: 2, 3 -> 2, 3
// smoke.test2: 2, 3, 4 -> FAILURE
// smoke.test0 ->
// smoke_test3: 2, 3 -> 2, 3
// smoke_test3: 2, 3, 4 -> FAILURE
