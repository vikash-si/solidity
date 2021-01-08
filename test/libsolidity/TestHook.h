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

#pragma once

#include <test/libsolidity/util/SoltestErrors.h>

#include <memory>
#include <vector>

namespace solidity::frontend::test
{

class TestFunctionCall;

class TestHook
{
public:
	TestHook() = default;
	virtual ~TestHook() = default;

	virtual void beginTestCase() = 0;
	virtual void beforeFunctionCall(TestFunctionCall const& _call)  = 0;
	virtual void afterFunctionCall(TestFunctionCall const& _call) = 0;
	virtual void endTestCase() = 0;

	virtual bool verifyFunctionCall(TestFunctionCall const& _call) = 0;

	virtual std::string formatFunctionCall(
		const TestFunctionCall& _call,
		ErrorReporter& _errorReporter,
		std::string const& _linePrefix,
		bool const _renderResult,
		bool const _highlight
	) const = 0;
};

using TestHooks = std::vector<std::shared_ptr<TestHook>>;

} // namespace solidity::frontend::test
