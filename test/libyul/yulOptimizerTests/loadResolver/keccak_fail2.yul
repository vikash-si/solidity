{
    mstore(0, 10)
    // Although, this can be evaluated, we're skipping it.
    let val := keccak256(0, 31)
    sstore(0, val)
}
// ----
// step: loadResolver
//
// {
//     let _1 := 10
//     let _2 := 0
//     mstore(_2, _1)
//     sstore(_2, keccak256(_2, 31))
// }
