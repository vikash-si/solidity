/*
    This file is part of solidity.

    solidity is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    solidity is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Unit tests for stack-reusing code generator.
 */

#include <test/Common.h>

#include <libyul/AssemblyStack.h>
#include <libevmasm/Instruction.h>

#include <boost/test/unit_test.hpp>

using namespace std;

namespace solidity::yul::test
{

namespace
{
string assemble(string const& _input)
{
	solidity::frontend::OptimiserSettings settings = solidity::frontend::OptimiserSettings::full();
	settings.runYulOptimiser = false;
	settings.optimizeStackAllocation = true;
	AssemblyStack asmStack(langutil::EVMVersion{}, AssemblyStack::Language::StrictAssembly, settings);
	BOOST_REQUIRE_MESSAGE(asmStack.parseAndAnalyze("", _input), "Source did not parse: " + _input);
	return evmasm::disassemble(asmStack.assemble(AssemblyStack::Machine::EVM).bytecode->bytecode);
}
}

BOOST_AUTO_TEST_SUITE(StackReuseCodegen, *boost::unit_test::label("nooptions"))

BOOST_AUTO_TEST_CASE(smoke_test)
{
	string out = assemble("{}");
	BOOST_CHECK_EQUAL(out, "");
}

BOOST_AUTO_TEST_CASE(single_var)
{
	string out = assemble("{ let x }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x0 POP ");
}

BOOST_AUTO_TEST_CASE(single_var_assigned)
{
	string out = assemble("{ let x := 1 }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x1 POP ");
}

BOOST_AUTO_TEST_CASE(single_var_assigned_plus_code)
{
	string out = assemble("{ let x := 1 mstore(3, 4) }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x1 POP PUSH1 0x4 PUSH1 0x3 MSTORE ");
}

BOOST_AUTO_TEST_CASE(single_var_assigned_plus_code_and_reused)
{
	string out = assemble("{ let x := 1 mstore(3, 4) pop(mload(x)) }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x1 PUSH1 0x4 PUSH1 0x3 MSTORE DUP1 MLOAD POP POP ");
}

BOOST_AUTO_TEST_CASE(multi_reuse_single_slot)
{
	string out = assemble("{ let x := 1 x := 6 let y := 2 y := 4 }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x1 PUSH1 0x6 SWAP1 POP POP PUSH1 0x2 PUSH1 0x4 SWAP1 POP POP ");
}

BOOST_AUTO_TEST_CASE(multi_reuse_single_slot_nested)
{
	string out = assemble("{ let x := 1 x := 6 { let y := 2 y := 4 } }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x1 PUSH1 0x6 SWAP1 POP POP PUSH1 0x2 PUSH1 0x4 SWAP1 POP POP ");
}

BOOST_AUTO_TEST_CASE(multi_reuse_same_variable_name)
{
	string out = assemble("{ let z := mload(0) { let x := 1 x := 6 z := x } { let x := 2 z := x x := 4 } }");
	BOOST_CHECK_EQUAL(out,
		"PUSH1 0x0 MLOAD "
		"PUSH1 0x1 PUSH1 0x6 SWAP1 POP DUP1 SWAP2 POP POP "
		"PUSH1 0x2 DUP1 SWAP2 POP PUSH1 0x4 SWAP1 POP POP "
		"POP "
	);
}

BOOST_AUTO_TEST_CASE(last_use_in_nested_block)
{
	string out = assemble("{ let z := 0 { pop(z) } let x := 1 }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x0 DUP1 POP POP PUSH1 0x1 POP ");
}

BOOST_AUTO_TEST_CASE(if_)
{
	// z is only removed after the if (after the jumpdest)
	string out = assemble("{ let z := mload(0) if z { let x := z } let t := 3 }");
	BOOST_CHECK_EQUAL(out, "PUSH1 0x0 MLOAD DUP1 ISZERO PUSH1 0xA JUMPI DUP1 POP JUMPDEST POP PUSH1 0x3 POP ");
}

BOOST_AUTO_TEST_CASE(switch_)
{
	string out = assemble("{ let z := 0 switch z case 0 { let x := 2 let y := 3 } default { z := 3 } let t := 9 }");
	BOOST_CHECK_EQUAL(out,
		"PUSH1 0x0 DUP1 "
		"PUSH1 0x0 DUP2 EQ PUSH1 0x11 JUMPI "
		"PUSH1 0x3 SWAP2 POP PUSH1 0x18 JUMP "
		"JUMPDEST PUSH1 0x2 POP PUSH1 0x3 POP "
		"JUMPDEST POP POP " // This is where z and its copy (switch condition) can be removed.
		"PUSH1 0x9 POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_slots)
{
	// x and y should reuse the slots of b and d
	string out = assemble("{ let a, b, c, d let x := 2 let y := 3 mstore(x, a) mstore(y, c) }");
	BOOST_CHECK_EQUAL(out,
		"PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 "
		"POP " // d is removed right away
		"PUSH1 0x2 SWAP2 POP " // x is stored at b's slot
		"PUSH1 0x3 DUP4 DUP4 MSTORE "
		"DUP2 DUP2 MSTORE "
		"POP POP POP POP "
	);
}

BOOST_AUTO_TEST_CASE(for_1)
{
	// Special scoping rules, but can remove z early
	string out = assemble("{ for { let z := 0 } 1 { } { let x := 3 } let t := 2 }");
	BOOST_CHECK_EQUAL(out,
		"PUSH1 0x0 POP "
		"JUMPDEST PUSH1 0x1 ISZERO PUSH1 0x11 JUMPI "
		"PUSH1 0x3 POP JUMPDEST PUSH1 0x3 JUMP "
		"JUMPDEST PUSH1 0x2 POP "
	);
}

BOOST_AUTO_TEST_CASE(for_2)
{
	// Special scoping rules, cannot remove z until after the loop!
	string out = assemble("{ for { let z := 0 } 1 { } { z := 8 let x := 3 } let t := 2 }");
	BOOST_CHECK_EQUAL(out,
		"PUSH1 0x0 "
		"JUMPDEST PUSH1 0x1 ISZERO PUSH1 0x14 JUMPI "
		"PUSH1 0x8 SWAP1 POP "
		"PUSH1 0x3 POP "
		"JUMPDEST PUSH1 0x2 JUMP "
		"JUMPDEST POP " // z is removed
		"PUSH1 0x2 POP "
	);
}

BOOST_AUTO_TEST_CASE(function_trivial)
{
	string in = R"({
		function f() { }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x6 JUMP JUMPDEST JUMPDEST JUMP JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam)
{
	string in = R"({
		function f() -> x, y { }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0xC JUMP "
		"JUMPDEST PUSH1 0x0 PUSH1 0x0 JUMPDEST SWAP1 SWAP2 JUMP "
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_params)
{
	string in = R"({
		function f(a, b) { }
	})";
	BOOST_CHECK_EQUAL(assemble(in), "PUSH1 0x8 JUMP JUMPDEST POP POP JUMPDEST JUMP JUMPDEST ");
}

BOOST_AUTO_TEST_CASE(function_params_and_retparams)
{
	string in = R"({
		function f(a, b, c, d) -> x, y { }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x10 JUMP JUMPDEST POP POP POP POP PUSH1 0x0 PUSH1 0x0 JUMPDEST SWAP1 SWAP2 JUMP JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_params_and_retparams_partly_unused)
{
	string in = R"({
		function f(a, b, c, d) -> x, y { b := 3 let s := 9 y := 2 mstore(s, y) }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x1E JUMP "
		"JUMPDEST "
		"POP " // a is entirely unused and popped early
		"PUSH1 0x3 SWAP1 POP " // b := 3, where b is now at the top of the stack
		"POP POP POP " // b, c and d are unused and can be popped
		"PUSH1 0x0 PUSH1 0x0 " // initialize x and y
		"PUSH1 0x9 " // allocate s
  		"PUSH1 0x2 SWAP2 POP " // y := 2
		"DUP2 DUP2 MSTORE " // mstore(s, y)
		"POP JUMPDEST SWAP1 SWAP2 JUMP "
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_unassigned)
{
	string in = R"({
		function f() -> x { pop(callvalue()) }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0xB JUMP "
		"JUMPDEST "
		"CALLVALUE POP "
		"PUSH1 0x0 "
		"JUMPDEST "
		"SWAP1 JUMP "
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_unassigned_multiple)
{
	string in = R"({
		function f() -> x, y, z { pop(callvalue()) }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x11 JUMP "
		"JUMPDEST "
		"CALLVALUE POP "
		"PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 "
		"JUMPDEST SWAP1 SWAP2 SWAP3 JUMP JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_leave)
{
	string in = R"({
		function f() -> x { pop(address()) leave pop(callvalue()) }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x10 JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"PUSH1 0xD JUMP " // leave
		"CALLVALUE POP " // pop(callvalue())
		"JUMPDEST " // function exit
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_declaration)
{
	string in = R"({
		function f() -> x { pop(address()) let y := callvalue() }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0xD JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"CALLVALUE " // let y := callvalue()
		"POP " // y out of scope
		"JUMPDEST " // exit tag
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_read)
{
	string in = R"({
		function f() -> x { pop(address()) sstore(0, x) pop(callvalue()) }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x11 JUMP " // jump over f
		"JUMPDEST " // entry of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"DUP1 PUSH1 0x0 SSTORE " // sstore(0, x)
		"CALLVALUE POP " // pop(callvalue())
		"JUMPDEST " // exit tag
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}


BOOST_AUTO_TEST_CASE(function_retparam_block)
{
	string in = R"({
		function f() -> x { pop(address()) { pop(callvalue()) } }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0xD JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"CALLVALUE POP " // pop(callvalue())
		"JUMPDEST " // exit tag
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_retparam_if)
{
	string in = R"({
		function f() -> x { pop(address()) if 1 { pop(callvalue()) } }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x14 JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"PUSH1 0x1 ISZERO PUSH1 0x10 JUMPI CALLVALUE POP JUMPDEST " // if 1 { pop(vallvalue()) }
		"JUMPDEST " // exit tag
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}


BOOST_AUTO_TEST_CASE(function_retparam_for)
{
	string in = R"({
		function f() -> x { pop(address()) for { pop(callvalue()) } 0 {} { } }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x19 JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"PUSH1 0x0 " // init of x
		"CALLVALUE POP JUMPDEST PUSH1 0x0 ISZERO PUSH1 0x15 JUMPI JUMPDEST PUSH1 0xA JUMP JUMPDEST " // for loop
		"JUMPDEST " // exit tag
		"SWAP1 " // swap x and return tag
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_argument_reuse)
{
	string in = R"({
		function f(a, b, c) -> x { pop(address()) sstore(a, c) pop(callvalue()) x := b }
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x17 JUMP " // jump over f
		"JUMPDEST " // start of f
		"ADDRESS POP " // pop(address())
		"DUP3 DUP2 SSTORE "  // sstore(a, c)
		"POP " // a and c are no longer used; a can be popped
		"CALLVALUE POP " // pop(callvalue())
		"PUSH1 0x0 SWAP2 POP " // init of x at slot of c
		"DUP1 SWAP2 POP "  // x := b
  		"POP " // b can be popped
		"JUMPDEST " // exit tag
		"SWAP1 " // swap return tag and x
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_many_arguments)
{
	string in = R"({
		function f(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20) -> x {
			mstore(0x0100, a1)
			mstore(0x0120, a2)
			mstore(0x0140, a3)
			mstore(0x0160, a4)
			mstore(0x0180, a5)
			mstore(0x01A0, a6)
			mstore(0x01C0, a7)
			mstore(0x01E0, a8)
			mstore(0x0200, a9)
			mstore(0x0220, a10)
			mstore(0x0240, a11)
			mstore(0x0260, a12)
			mstore(0x0280, a13)
			mstore(0x02A0, a14)
			mstore(0x02C0, a15)
			mstore(0x02E0, a16)
			mstore(0x0300, a17)
			mstore(0x0320, a18)
			mstore(0x0340, a19)
			x := a20
		}
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x80 JUMP " // jump over f
		"JUMPDEST " // start of f
		"DUP1 PUSH2 0x100 MSTORE POP " // store a1
		"DUP1 PUSH2 0x120 MSTORE POP " // store a2
		"DUP1 PUSH2 0x140 MSTORE POP " // store a3
		"DUP1 PUSH2 0x160 MSTORE POP " // store a4
		"DUP1 PUSH2 0x180 MSTORE POP " // store a5
		"DUP1 PUSH2 0x1A0 MSTORE POP " // store a6
		"DUP1 PUSH2 0x1C0 MSTORE POP " // store a7
		"DUP1 PUSH2 0x1E0 MSTORE POP " // store a8
		"DUP1 PUSH2 0x200 MSTORE POP " // store a9
		"DUP1 PUSH2 0x220 MSTORE POP " // store a10
		"DUP1 PUSH2 0x240 MSTORE POP " // store a11
		"DUP1 PUSH2 0x260 MSTORE POP " // store a12
		"DUP1 PUSH2 0x280 MSTORE POP " // store a13
		"DUP1 PUSH2 0x2A0 MSTORE POP " // store a14
		"DUP1 PUSH2 0x2C0 MSTORE POP " // store a15
		"DUP1 PUSH2 0x2E0 MSTORE POP " // store a16
		"DUP1 PUSH2 0x300 MSTORE POP " // store a17
		"DUP1 PUSH2 0x320 MSTORE POP " // store a18
		"DUP1 PUSH2 0x340 MSTORE POP " // store a19
		"PUSH1 0x0 DUP2 SWAP1 POP " // x := a20
		"JUMPDEST " // exit tag
		"SWAP2 SWAP1 " // move x and return tag
		"POP " // pop a20
		"JUMP " // return
		"JUMPDEST "
	);
}

BOOST_AUTO_TEST_CASE(function_with_body_embedded)
{
	string in = R"({
		let b := 3
		function f(a, r) -> t {
			let x := a a := 3 t := a
		}
		b := 7
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x3 "
		"PUSH1 0x17 JUMP "
  		"JUMPDEST " // start of f
  		"PUSH1 0x0 SWAP2 POP " // initialize t at slot of r
		"DUP1 POP " // let x := a, immediately discarding, since x is unused
  		"PUSH1 0x3 SWAP1 POP " // a := 3
		"DUP1 SWAP2 POP " // t := a
		"POP " // pop a
		"JUMPDEST SWAP1 JUMP "
		"JUMPDEST PUSH1 0x7 SWAP1 "
		"POP POP "
	);
}

BOOST_AUTO_TEST_CASE(function_call)
{
	string in = R"({
		let b := f(1, 2)
		function f(a, r) -> t { }
		b := f(3, 4)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x9 PUSH1 0x2 PUSH1 0x1 PUSH1 0xD JUMP "
		"JUMPDEST PUSH1 0x15 JUMP " // jump over f
		"JUMPDEST POP POP PUSH1 0x0 JUMPDEST SWAP1 JUMP " // f
		"JUMPDEST PUSH1 0x1F PUSH1 0x4 PUSH1 0x3 PUSH1 0xD JUMP "
		"JUMPDEST SWAP1 POP POP "
	);
}


BOOST_AUTO_TEST_CASE(functions_multi_return)
{
	string in = R"({
		function f(a, b) -> t { }
		function g() -> r, s { }
		let x := f(1, 2)
		x := f(3, 4)
		let y, z := g()
		y, z := g()
		let unused := 7
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x14 JUMP "
		"JUMPDEST POP POP PUSH1 0x0 JUMPDEST SWAP1 JUMP " // f
		"JUMPDEST PUSH1 0x0 PUSH1 0x0 JUMPDEST SWAP1 SWAP2 JUMP " // g
		"JUMPDEST PUSH1 0x1E PUSH1 0x2 PUSH1 0x1 PUSH1 0x3 JUMP " // f(1, 2)
		"JUMPDEST PUSH1 0x28 PUSH1 0x4 PUSH1 0x3 PUSH1 0x3 JUMP " // f(3, 4)
		"JUMPDEST SWAP1 POP " // assignment to x
		"POP " // remove x
		"PUSH1 0x31 PUSH1 0xB JUMP " // g()
		"JUMPDEST PUSH1 0x37 PUSH1 0xB JUMP " // g()
		"JUMPDEST SWAP2 POP SWAP2 POP " // assignments
		"POP POP " // removal of y and z
		"PUSH1 0x7 POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_slots_function)
{
	string in = R"({
		function f() -> x, y, z, t {}
		let a, b, c, d := f() let x1 := 2 let y1 := 3 mstore(x1, a) mstore(y1, c)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x12 JUMP "
		"JUMPDEST PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 JUMPDEST SWAP1 SWAP2 SWAP3 SWAP4 JUMP "
		"JUMPDEST PUSH1 0x18 PUSH1 0x3 JUMP "
		// Stack: a b c d
		"JUMPDEST POP " // d is unused
		// Stack: a b c
		"PUSH1 0x2 SWAP2 POP " // x1 reuses b's slot
		"PUSH1 0x3 "
		// Stack: a x1 c y1
		"DUP4 DUP4 MSTORE "
		"DUP2 DUP2 MSTORE "
		"POP POP POP POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_slots_function_with_gaps)
{
	string in = R"({
		// Only x3 is actually used, the slots of
		// x1 and x2 will be reused right away.
		let x1 := 5 let x2 := 6 let x3 := 7
		mstore(x1, x2)
		function f() -> x, y, z, t {}
		let a, b, c, d := f() mstore(x3, a) mstore(c, d)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x5 PUSH1 0x6 PUSH1 0x7 "
		"DUP2 DUP4 MSTORE "
		"PUSH1 0x1B JUMP " // jump across function
		"JUMPDEST PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 PUSH1 0x0 JUMPDEST SWAP1 SWAP2 SWAP3 SWAP4 JUMP "
		"JUMPDEST PUSH1 0x21 PUSH1 0xC JUMP "
		// stack: x1 x2 x3 a b c d
		"JUMPDEST SWAP6 POP " // move d into x1
		// stack: d x2 x3 a b c
		"SWAP4 POP "
		// stack: d c x3 a b
		"POP "
		// stack: d c x3 a
		"DUP1 DUP3 MSTORE "
		"POP POP "
		// stack: d c
		"DUP2 DUP2 MSTORE "
		"POP POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_on_decl_assign_to_last_used)
{
	string in = R"({
		let x := 5
		let y := x // y should reuse the stack slot of x
		sstore(y, y)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x5 "
		"DUP1 SWAP1 POP "
		"DUP1 DUP2 SSTORE "
		"POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_on_decl_assign_to_last_used_expr)
{
	string in = R"({
		let x := 5
		let y := add(x, 2) // y should reuse the stack slot of x
		sstore(y, y)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x5 "
		"PUSH1 0x2 DUP2 ADD "
		"SWAP1 POP "
		"DUP1 DUP2 SSTORE "
		"POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_on_decl_assign_to_not_last_used)
{
	string in = R"({
		let x := 5
		let y := x // y should not reuse the stack slot of x, since x is still used below
		sstore(y, x)
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x5 "
		"DUP1 "
		"DUP2 DUP2 SSTORE "
		"POP POP "
	);
}

BOOST_AUTO_TEST_CASE(reuse_on_decl_assign_not_same_scope)
{
	string in = R"({
		let x := 5
		{
			let y := x // y should not reuse the stack slot of x, since x is not in the same scope
			sstore(y, y)
		}
	})";
	BOOST_CHECK_EQUAL(assemble(in),
		"PUSH1 0x5 "
		"DUP1 "
		"DUP1 DUP2 SSTORE "
		"POP POP "
	);
}


BOOST_AUTO_TEST_SUITE_END()

}
