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

#include <test/tools/ossfuzz/yulProto.pb.h>
#include <test/tools/ossfuzz/protoToYul.h>

#include <test/tools/ossfuzz/SolidityEvmoneInterface.h>

#include <libyul/AssemblyStack.h>
#include <libyul/Exceptions.h>

#include <libyul/backends/evm/EVMDialect.h>

#include <libevmasm/Instruction.h>

#include <liblangutil/EVMVersion.h>

#include <src/libfuzzer/libfuzzer_macro.h>

#include <fstream>

using namespace solidity;
using namespace solidity::test;
using namespace solidity::test::fuzzer;
using namespace solidity::yul;
using namespace solidity::yul::test;
using namespace solidity::yul::test::yul_fuzzer;
using namespace solidity::langutil;
using namespace std;

static evmc::VM evmone = evmc::VM{evmc_create_evmone()};

DEFINE_PROTO_FUZZER(Program const& _input)
{
	ProtoConverter converter;
	langutil::EVMVersion version;
	EVMHost hostContext(version, evmone);
	string yul_source = converter.programToString(_input);

	if (const char* dump_path = getenv("PROTO_FUZZER_DUMP_PATH"))
	{
		ofstream of(dump_path);
		of.write(yul_source.data(), static_cast<streamsize>(yul_source.size()));
	}

	if (yul_source.size() > 1200)
		return;

	YulStringRepository::reset();

	solidity::frontend::OptimiserSettings settings = solidity::frontend::OptimiserSettings::full();
	settings.runYulOptimiser = false;
	settings.optimizeStackAllocation = true;

	AssemblyStack stack(
		version,
		AssemblyStack::Language::StrictAssembly,
		settings
	);

	// Parse protobuf mutated YUL code
	if (
		!stack.parseAndAnalyze("source", yul_source) ||
		!stack.parserResult()->code ||
		!stack.parserResult()->analysisInfo ||
		!Error::containsOnlyWarnings(stack.errors())
	)
		yulAssert(false, "Proto fuzzer generated malformed program");

	bytes unoptimisedByteCode = stack.assemble(AssemblyStack::Machine::EVM).bytecode->bytecode;
	// Zero initialize all message fields
	evmc_message msg = {};
	// Gas available (value of type int64_t) is set to its maximum
	// value.
	msg.gas = std::numeric_limits<int64_t>::max();
	msg.input_data = unoptimisedByteCode.data();
	msg.input_size = unoptimisedByteCode.size();
	msg.kind = EVMC_CREATE;
	evmc::result deployResult = hostContext.call(msg);
	solAssert(
		deployResult.status_code == EVMC_SUCCESS,
		"Evmone: Contract creation failed"
	);
	evmc_message call = {};
	call.gas = std::numeric_limits<int64_t>::max();
	call.destination = deployResult.create_address;
	call.kind = EVMC_CALL;
	evmc::result callResult = hostContext.call(call);
	// We don't care about EVM One failures other than EVMC_REVERT
	solAssert(
		callResult.status_code != EVMC_REVERT,
		"SolidityEvmoneInterface: EVM One reverted"
	);
	ostringstream os;
	hostContext.print_all_storage(os);
}

