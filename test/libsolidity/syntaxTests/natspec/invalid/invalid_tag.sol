/// @a&b test
contract C {
	/// @custom:x^y test2
	function f() public pure {}
}
// ----
// DocstringParsingError 2968: (0-14): Invalid character in tag @a&b.
// DocstringParsingError 6546: (0-14): Documentation tag @a&b not valid for contracts.
// DocstringParsingError 2968: (28-49): Invalid character in tag @custom:x^y.
