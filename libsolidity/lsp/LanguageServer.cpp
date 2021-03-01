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
// SPDX-License-Identifier: GPL-3.0
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/interface/ReadFile.h>
#include <libsolidity/lsp/LanguageServer.h>
#include <libsolidity/lsp/ReferenceCollector.h>

#include <liblangutil/SourceReferenceExtractor.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <ostream>

#include <iostream>
#include <string>

using namespace std;
using namespace std::placeholders;

using namespace solidity::langutil;
using namespace solidity::frontend;

namespace solidity::lsp {

namespace // {{{ helpers
{
	string toFileURI(std::string const& _path)
	{
		return "file://" + _path;
	}

	// TODO: maybe use SimpleASTVisitor here, if that would be a simple free-fuunction :)
	class ASTNodeLocator : public ASTConstVisitor
	{
	public:
		static ASTNode const* locate(int _pos, SourceUnit const& _sourceUnit)
		{
			ASTNodeLocator locator{_pos};
			_sourceUnit.accept(locator);
			return locator.m_closestMatch;
		}

		bool visitNode(ASTNode const& _node) override
		{
			if (_node.location().start <= m_pos && m_pos <= _node.location().end)
			{
				m_closestMatch = &_node;
				return true;
			}
			return false;
		}

	private:
		explicit ASTNodeLocator(int _pos): m_pos{_pos} {}

		int m_pos = -1;
		ASTNode const* m_closestMatch = nullptr;
	};

	optional<string> extractPathFromFileURI(std::string const& _uri)
	{
		if (!boost::starts_with(_uri, "file://"))
			return nullopt;

		return _uri.substr(7);
	}

	void loadTextDocumentPosition(DocumentPosition& _params, Json::Value const& _json)
	{
		_params.path = extractPathFromFileURI(_json["textDocument"]["uri"].asString()).value();
		_params.position.line = _json["position"]["line"].asInt() + 1;
		_params.position.column = _json["position"]["character"].asInt() + 1;
	}

	DocumentPosition extractDocumentPosition(Json::Value const& _json)
	{
		DocumentPosition dpos{};
		dpos.path = extractPathFromFileURI(_json["textDocument"]["uri"].asString()).value();
		dpos.position.line = _json["position"]["line"].asInt() + 1;
		dpos.position.column = _json["position"]["character"].asInt() + 1;
		return dpos;
	}

	Json::Value toJsonRange(SourceLocation const& _location)
	{
		solAssert(_location.source.get() != nullptr, "");

		Json::Value json;

		auto const [startLine, startColumn] = _location.source->translatePositionToLineColumn(_location.start);
		json["start"]["line"] = max(startLine - 1, 0);
		json["start"]["character"] = max(startColumn - 1, 0);

		auto const [endLine, endColumn] = _location.source->translatePositionToLineColumn(_location.end);
		json["end"]["line"] = max(endLine - 1, 0);
		json["end"]["character"] = max(endColumn - 1, 0);

		return json;
	}

	Json::Value toJson(SourceLocation const& _location)
	{
		solAssert(_location.source.get() != nullptr, "");
		Json::Value item = Json::objectValue;
		item["uri"] = toFileURI(_location.source->name());
		item["range"] = toJsonRange(_location);
		return item;
	}

	Json::Value toJson(LineColumn _pos)
	{
		Json::Value json = Json::objectValue;
		json["line"] = max(_pos.line - 1, 0);
		json["character"] = max(_pos.column - 1, 0);

		return json;
	}
} // }}} end helpers

LanguageServer::LanguageServer(Transport& _client, Logger _logger):
	m_client{_client},
	m_handlers{
		{"cancelRequest", [](auto, auto) {/*don't do anything for now, as we're synchronous*/}},
		{"$/cancelRequest", [](auto, auto) {/*don't do anything for now, as we're synchronous*/}},
		{"initialize", bind(&LanguageServer::handle_initialize, this, _1, _2)},
		{"initialized", {} },
		{"shutdown", [this](auto, auto) { m_shutdownRequested = true; }},
		{"workspace/didChangeConfiguration", bind(&LanguageServer::handle_workspace_didChangeConfiguration, this, _1, _2)},
		{"textDocument/didOpen", bind(&LanguageServer::handle_textDocument_didOpen, this, _1, _2)},
		{"textDocument/didChange", bind(&LanguageServer::handle_textDocument_didChange, this, _1, _2)},
		{"textDocument/didClose", [this](auto, Json::Value const& _args) {
			documentClosed(extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value());
		}},
		{"textDocument/definition", bind(&LanguageServer::handle_textDocument_definition, this, _1, _2)},
		{"textDocument/documentHighlight", bind(&LanguageServer::handle_textDocument_highlight, this, _1, _2)},
		{"textDocument/references", bind(&LanguageServer::handle_textDocument_references, this, _1, _2)},
	},
	m_logger{std::move(_logger)},
	m_vfs()
{
}

void LanguageServer::changeConfiguration(Json::Value const& _settings)
{
	if (_settings["evm"].isString())
		if (auto const evmVersionOpt = EVMVersion::fromString(_settings["evm"].asString()); evmVersionOpt.has_value())
			m_evmVersion = evmVersionOpt.value();

	if (_settings["remapping"].isArray())
	{
		for (auto const& element: _settings["remapping"])
		{
			if (element.isString())
			{
				if (auto remappingOpt = CompilerStack::parseRemapping(element.asString()); remappingOpt.has_value())
					m_remappings.emplace_back(move(remappingOpt.value()));
				else
					trace("Failed to parse remapping: '"s + element.asString() + "'");
			}
		}
	}
}

void LanguageServer::documentContentUpdated(string const& _path, std::optional<int> _version, LineColumnRange _range, std::string const& _text)
{
	// TODO: all this info is actually unrelated to solidity/lsp specifically except knowing that
	// the file has updated, so we can  abstract that away and only do the re-validation here.
	auto file = m_vfs.find(_path);
	if (!file)
	{
		log("LanguageServer: File to be modified not opened \"" + _path + "\"");
		return;
	}

	if (_version.has_value())
		file->setVersion(_version.value());

	file->modify(_range, _text);
}

void LanguageServer::documentContentUpdated(string const& _path, optional<int> _version, string const& _fullContentChange)
{
	auto file = m_vfs.find(_path);
	if (!file)
	{
		log("LanguageServer: File to be modified not opened \"" + _path + "\"");
		return;
	}

	if (_version.has_value())
		file->setVersion(_version.value());

	file->replace(_fullContentChange);

	validate(*file);
}

void LanguageServer::documentClosed(string const& _path)
{
	log("LanguageServer: didClose: " + _path);
}

void LanguageServer::validateAll()
{
	for (reference_wrapper<vfs::File const> const& file: m_vfs.files())
		validate(file.get());
}

frontend::ReadCallback::Result LanguageServer::readFile(string const& _kind, string const& _path)
{
	return m_fileReader->readFile(_kind, _path);
}

constexpr DiagnosticSeverity toDiagnosticSeverity(Error::Type _errorType)
{
	using Type = Error::Type;
	using Severity = DiagnosticSeverity;
	switch (_errorType)
	{
		case Type::CodeGenerationError:
		case Type::DeclarationError:
		case Type::DocstringParsingError:
		case Type::ParserError:
		case Type::SyntaxError:
		case Type::TypeError:
			return Severity::Error;
		case Type::Warning:
			return Severity::Warning;
	}
	// Should never be reached.
	return Severity::Error;
}

void LanguageServer::compile(vfs::File const& _file)
{
	// TODO: optimize! do not recompile if nothing has changed (file(s) not flagged dirty).

	// always start fresh when compiling
	m_sourceCodes.clear();

	m_sourceCodes[_file.path()] = _file.contentString();

	m_fileReader = make_unique<FileReader>(m_basePath, m_allowedDirectories);

	m_compilerStack.reset();
	m_compilerStack = make_unique<CompilerStack>(bind(&FileReader::readFile, ref(*m_fileReader), _1, _2));

	// TODO: configure all compiler flags like in CommandLineInterface (TODO: refactor to share logic!)
	OptimiserSettings settings = OptimiserSettings::standard(); // TODO: get from config
	m_compilerStack->setOptimiserSettings(settings);
	m_compilerStack->setParserErrorRecovery(false);
	m_compilerStack->setRevertStringBehaviour(RevertStrings::Default); // TODO get from config
	m_compilerStack->setSources(m_sourceCodes);
	m_compilerStack->setRemappings(m_remappings);

	m_compilerStack->setEVMVersion(m_evmVersion);

	trace("compile: using EVM "s + m_evmVersion.name());

	m_compilerStack->compile();
}

void LanguageServer::validate(vfs::File const& _file)
{
	compile(_file);

	Json::Value params;
	params["uri"] = toFileURI(_file.path());
	if (_file.version())
		params["version"] = _file.version();

	params["diagnostics"] = Json::arrayValue;
	for (shared_ptr<Error const> const& error: m_compilerStack->errors())
	{
		// Don't show this warning. "This is a pre-release compiler version."
		if (error->errorId().error == 3805)
			continue;

		SourceReferenceExtractor::Message const message = SourceReferenceExtractor::extract(*error);

		Json::Value jsonDiag;
		jsonDiag["source"] = "solc";
		jsonDiag["severity"] = int(toDiagnosticSeverity(error->type()));
		jsonDiag["message"] = message.primary.message;

		if (message.errorId.has_value())
			jsonDiag["code"] = Json::UInt64{message.errorId.value().error};

		jsonDiag["range"]["start"] = toJson(LineColumn{message.primary.position.line, message.primary.startColumn});
		jsonDiag["range"]["end"] = toJson(LineColumn{message.primary.position.line, message.primary.endColumn});

		for (SourceReference const& secondary: message.secondary)
		{
			Json::Value jsonRelated;
			jsonRelated["message"] = secondary.message;
			jsonRelated["location"]["uri"] = toFileURI(secondary.sourceName);
			jsonRelated["location"]["range"]["start"] = toJson(LineColumn{secondary.position.line, secondary.startColumn});
			jsonRelated["location"]["range"]["end"] = toJson(LineColumn{secondary.position.line, secondary.endColumn});
			jsonDiag["relatedInformation"].append(jsonRelated);
		}

		params["diagnostics"].append(jsonDiag);
	}

	m_client.notify("textDocument/publishDiagnostics", params);
}

frontend::ASTNode const* LanguageServer::findASTNode(LineColumn _position, std::string const& _fileName)
{
	if (!m_compilerStack)
		return nullptr;

	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(_fileName);
	auto const sourcePos = sourceUnit.location().source->translateLineColumnToPosition(_position.line, _position.column);
	if (!sourcePos.has_value())
		return nullptr;

	ASTNode const* closestMatch = ASTNodeLocator::locate(sourcePos.value(), sourceUnit);

	if (!closestMatch)
		trace(
			"findASTNode not found for "s +
			to_string(sourcePos.value()) + ":" +
			to_string(_position.line) + ":" +
			to_string(_position.column)
		);
	else
		trace(
			"findASTNode found for "s +
			to_string(sourcePos.value()) + ":" +
			to_string(_position.line) + ":" +
			to_string(_position.column) + ": " +
			closestMatch->location().text() + " (" +
			typeid(*closestMatch).name() +
			")"s
		);


	return closestMatch;
}

optional<SourceLocation> LanguageServer::declarationPosition(frontend::Declaration const* _declaration)
{
	if (!_declaration)
		return nullopt;

	if (_declaration->nameLocation().isValid())
		return _declaration->nameLocation();

	if (_declaration->location().isValid())
		return _declaration->location();

	return nullopt;
}

void LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	string const& _sourceIdentifierName,
	frontend::SourceUnit const& _sourceUnit,
	std::vector<SourceLocation>& _output
)
{
	for (DocumentHighlight& highlight: ReferenceCollector::collect(_declaration, _sourceUnit, _sourceIdentifierName))
		_output.emplace_back(move(highlight.location));
}

vector<SourceLocation> LanguageServer::references(DocumentPosition _documentPosition)
{
	auto const file = m_vfs.find(_documentPosition.path);
	if (!file)
	{
		trace("File does not exist. " + _documentPosition.path);
		return {};
	}

	if (!m_compilerStack)
		compile(*file);

	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceName = file->path();

	auto const sourceNode = findASTNode(_documentPosition.position, sourceName);
	if (!sourceNode)
	{
		trace("AST node not found");
		return {};
	}

	auto output = vector<SourceLocation>{};
	if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

		if (auto decl = sourceIdentifier->annotation().referencedDeclaration)
			findAllReferences(decl, decl->name(), sourceUnit, output);

		for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
			findAllReferences(decl, decl->name(), sourceUnit, output);
	}
	else if (auto const decl = dynamic_cast<VariableDeclaration const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(decl, decl->name(), sourceUnit, output);
	}
	else if (auto const* functionDefinition = dynamic_cast<FunctionDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(functionDefinition, functionDefinition->name(), sourceUnit, output);
	}
	else if (auto const* enumDef = dynamic_cast<EnumDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(enumDef, enumDef->name(), sourceUnit, output);
	}
	else if (auto const memberAccess = dynamic_cast<MemberAccess const*>(sourceNode))
	{
		if (Declaration const* decl = memberAccess->annotation().referencedDeclaration)
		{
			auto const sourceName = _documentPosition.path;
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
			findAllReferences(decl, memberAccess->memberName(), sourceUnit, output);
		}
	}
	else if (auto const* importDef = dynamic_cast<ImportDirective const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(importDef, importDef->name(), sourceUnit, output);
	}
	else
		trace("references: not an identifier: "s + typeid(*sourceNode).name());

	return output;
}

vector<DocumentHighlight> LanguageServer::semanticHighlight(DocumentPosition _documentPosition)
{
	auto const file = m_vfs.find(_documentPosition.path);
	if (!file)
	{
		trace("semanticHighlight: Could not map document path to file.");
		return {};
	}

	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceName = file->path();
	auto const sourceNode = findASTNode(_documentPosition.position, sourceName);
	if (!sourceNode)
	{
		trace("semanticHighlight: AST node not found");
		return {};
	}

	trace(
		"semanticHighlight: Source Node("s + typeid(*sourceNode).name() + "): " +
		sourceNode->location().text()
	);

	auto output = vector<DocumentHighlight>{};

	// TODO: ImportDirective: hovering a symbol of an import directive should highlight all uses of that symbol.
	if (auto const* sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

		if (sourceIdentifier->annotation().referencedDeclaration)
			output += ReferenceCollector::collect(sourceIdentifier->annotation().referencedDeclaration, sourceUnit, sourceIdentifier->name());

		for (Declaration const* declaration: sourceIdentifier->annotation().candidateDeclarations)
			output += ReferenceCollector::collect(declaration, sourceUnit, sourceIdentifier->name());

		for (Declaration const* declaration: sourceIdentifier->annotation().overloadedDeclarations)
			output += ReferenceCollector::collect(declaration, sourceUnit, sourceIdentifier->name());
	}
	else if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output = ReferenceCollector::collect(varDecl, sourceUnit, varDecl->name());
	}
	else if (auto const* structDef = dynamic_cast<StructDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output = ReferenceCollector::collect(structDef, sourceUnit, structDef->name());
	}
	else if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(sourceNode))
	{
		TypePointer const type = memberAccess->expression().annotation().type;
		if (auto const ttype = dynamic_cast<TypeType const*>(type))
		{
			auto const memberName = memberAccess->memberName();
			auto const sourceName = _documentPosition.path;
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

			if (auto const* enumType = dynamic_cast<EnumType const*>(ttype->actualType()))
			{
				auto const& enumMembers = enumType->enumDefinition().members();
				if (enumMembers.empty())
					trace("enumType members are empty");

				// find the definition
				for (auto const& enumMember: enumMembers)
					if (enumMember->name() == memberName)
						output += ReferenceCollector::collect(enumMember.get(), sourceUnit, enumMember->name());

				// find uses of the enum value
			}
			else
				trace("semanticHighlight: not an EnumType");
		}
		else
		{
			// TODO: StructType, ...
			trace("semanticHighlight: member type is: "s + (type ? typeid(*type).name() : "NULL"));
		}

		// TODO: If the cursor os positioned on top of a type name, then all other symbols matching
		// this type should be highlighted (clangd does so, too).
		//
		// if (auto const tt = dynamic_cast<TypeType const*>(type))
		// {
		// 	auto const sourceName = _documentPosition.path;
		// 	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		// 	output = findAllReferences(declaration, sourceUnit);
		// }
	}
	else if (auto const* identifierPath = dynamic_cast<IdentifierPath const*>(sourceNode))
	{
		solAssert(!identifierPath->path().empty(), "");
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output += ReferenceCollector::collect(identifierPath->annotation().referencedDeclaration, sourceUnit, identifierPath->path().back());
	}
	else if (auto const* functionDefinition = dynamic_cast<FunctionDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output += ReferenceCollector::collect(functionDefinition, sourceUnit, functionDefinition->name());
	}
	else if (auto const* enumDef = dynamic_cast<EnumDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output += ReferenceCollector::collect(enumDef, sourceUnit, enumDef->name());
	}
	else if (auto const* importDef = dynamic_cast<ImportDirective const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output += ReferenceCollector::collect(importDef, sourceUnit, importDef->name());
	}
	else
		trace("semanticHighlight: not an identifier. "s + typeid(*sourceNode).name());

	return output;
}

// {{{ LSP internals
bool LanguageServer::run()
{
	while (!m_exitRequested && !m_client.closed())
	{
		// TODO: receive() must return a variant<> to also return on <Transport::TimeoutEvent>,
		// so that we can perform some idle tasks in the meantime, such as
		// - lazy validation runs
		// - check for results of asynchronous runs (in case we want to support threaded background jobs)
		// Also, EOF should be noted properly as a <Transport::ClosedEvent>.
		optional<Json::Value> const jsonMessage = m_client.receive();
		if (jsonMessage.has_value())
		{
			try
			{
				handleMessage(*jsonMessage);
			}
			catch (std::exception const& e)
			{
				log("Unhandled exception caught when handling message. "s + e.what());
			}
		}
		else
			log("Could not read RPC request.");
	}

	if (m_shutdownRequested)
		return true;
	else
		return false;
}

void LanguageServer::handle_initialize(MessageId _id, Json::Value const& _args)
{
	string rootPath;
	if (Json::Value uri = _args["rootUri"])
		rootPath = extractPathFromFileURI(uri.asString()).value();
	else if (Json::Value rootPath = _args["rootPath"]; rootPath)
		rootPath = rootPath.asString();

	if (Json::Value value = _args["trace"]; value)
	{
		string const name = value.asString();
		if (name == "messages")
			m_trace = Trace::Messages;
		else if (name == "verbose")
			m_trace = Trace::Verbose;
		else if (name == "off")
			m_trace = Trace::Off;
	}

#if 0 // Currently not used.
	// At least VScode supports more than one workspace.
	// This is the list of initial configured workspace folders
	struct WorkspaceFolder { std::string name; std::string path; };
	std::vector<WorkspaceFolder> workspaceFolders;
	if (Json::Value folders = _args["workspaceFolders"]; folders)
	{
		for (Json::Value folder: folders)
		{
			WorkspaceFolder wsFolder{};
			wsFolder.name = folder["name"].asString();
			wsFolder.path = extractPathFromFileURI(folder["uri"].asString()).value();
			workspaceFolders.emplace_back(move(wsFolder));
		}
	}
#endif

	// TODO: ClientCapabilities
	// ... Do we actually care? Not in the initial PR.

	auto const fspath = boost::filesystem::path(rootPath);

	m_basePath = fspath;
	m_allowedDirectories.push_back(fspath);

	if (_args["initializationOptions"].isObject())
		changeConfiguration(_args["initializationOptions"]);

	// {{{ encoding
	Json::Value replyArgs;

	replyArgs["serverInfo"]["name"] = "solc";
	replyArgs["serverInfo"]["version"] = string(solidity::frontend::VersionNumber);
	replyArgs["hoverProvider"] = true;
	replyArgs["capabilities"]["hoverProvider"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["openClose"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["change"] = 2; // 0=none, 1=full, 2=incremental
	replyArgs["capabilities"]["definitionProvider"] = true;
	replyArgs["capabilities"]["documentHighlightProvider"] = true;
	replyArgs["capabilities"]["referencesProvider"] = true;

	m_client.reply(_id, replyArgs);
	// }}}
}

void LanguageServer::handle_workspace_didChangeConfiguration(MessageId, Json::Value const& _args)
{
	if (_args["settings"].isObject())
		changeConfiguration(_args["settings"]);
}

void LanguageServer::handle_exit(MessageId _id, Json::Value const& /*_args*/)
{
	m_exitRequested = true;
	auto const exitCode = m_shutdownRequested ? 0 : 1;

	Json::Value replyArgs = Json::intValue;
	replyArgs = exitCode;

	m_client.reply(_id, replyArgs);
}

void LanguageServer::handle_textDocument_didOpen(MessageId /*_id*/, Json::Value const& _args)
{
	// decoding
	if (!_args["textDocument"])
		return;

	auto const path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();
	auto const languageId = _args["textDocument"]["languageId"].asString();
	auto const version = _args["textDocument"]["version"].asInt();
	auto const text = _args["textDocument"]["text"].asString();

	log("LanguageServer: Opening document: " + path);

	vfs::File const& file = m_vfs.insert(
		path,
		languageId,
		version,
		text
	);

	validate(file);

	// no encoding
}

void LanguageServer::handle_textDocument_didChange(MessageId /*_id*/, Json::Value const& _args)
{
	auto const version = _args["textDocument"]["version"].asInt();
	auto const path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();

	// TODO: in the longer run, I'd like to try moving the VFS handling into Server class, so
	// the specific Solidity LSP implementation doesn't need to care about that.

	auto const contentChanges = _args["contentChanges"];
	for (Json::Value jsonContentChange: contentChanges)
	{
		if (!jsonContentChange.isObject())
			// Protocol error, will only happen on broken clients, so silently ignore it.
			continue;

		auto const text = jsonContentChange["text"].asString();

		if (jsonContentChange["range"].isObject())
		{
			Json::Value jsonRange = jsonContentChange["range"];
			LineColumnRange range{};
			range.start.line = jsonRange["start"]["line"].asInt();
			range.start.column = jsonRange["start"]["character"].asInt();
			range.end.line = jsonRange["end"]["line"].asInt();
			range.end.column = jsonRange["end"]["character"].asInt();

			documentContentUpdated(path, version, range, text);
		}
		else
		{
			// full content update
			documentContentUpdated(path, version, text);
		}
	}

	if (!contentChanges.empty())
	{
		auto file = m_vfs.find(path);
		if (!file)
			log("LanguageServer: File to be modified not opened \"" + path + "\"");
		else
			validate(*file);
	}
}

void LanguageServer::handle_textDocument_definition(MessageId _id, Json::Value const& _args)
{
	DocumentPosition const dpos = extractDocumentPosition(_args);

	// source should be compiled already
	solAssert(m_compilerStack.get() != nullptr, "");

	auto const file = m_vfs.find(dpos.path);
	if (!file)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client.reply(_id, emptyResponse);
		return;
	}

	auto const sourceNode = findASTNode(dpos.position, file->path());
	if (!sourceNode)
	{
		trace("gotoDefinition: AST node not found for "s + to_string(dpos.position.line) + ":" + to_string(dpos.position.column));
		// Could not infer AST node from given source location.
		Json::Value emptyResponse = Json::arrayValue;
		m_client.reply(_id, emptyResponse);
		return;
	}

	vector<SourceLocation> locations;
	if (auto const importDirective = dynamic_cast<ImportDirective const*>(sourceNode))
	{
		// When cursor is on an import directive, then we want to jump to the actual file that
		// is being imported.
		auto const fpm = m_fileReader->fullPathMapping().find(importDirective->path());
		if (fpm != m_fileReader->fullPathMapping().end())
			locations.emplace_back(SourceLocation{0, 0, make_shared<CharStream>("", fpm->second)});
		else
			trace("gotoDefinition: (importDirective) full path mapping not found\n");
	}
	else if (auto const n = dynamic_cast<frontend::MemberAccess const*>(sourceNode))
	{
		// For scope members, jump to the naming symbol of the referencing declaration of this member.
		auto const declaration = n->annotation().referencedDeclaration;
		auto const location = declarationPosition(declaration);
		if (location.has_value())
			locations.emplace_back(location.value());
		else
			trace("gotoDefinition: declaration not found.");
	}
	else if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		// For identifiers, jump to the naming symbol of the definition of this identifier.
		if (Declaration const* decl = sourceIdentifier->annotation().referencedDeclaration)
			if (auto location = declarationPosition(decl); location.has_value())
				locations.emplace_back(move(location.value()));
		// if (auto location = declarationPosition(sourceIdentifier->annotation().referencedDeclaration); location.has_value())
		// 	locations.emplace_back(move(location.value()));

		for (auto const declaration: sourceIdentifier->annotation().candidateDeclarations)
			if (auto location = declarationPosition(declaration); location.has_value())
				locations.emplace_back(move(location.value()));
	}
	else
		trace("gotoDefinition: Symbol is not an identifier. "s + typeid(*sourceNode).name());

	Json::Value reply = Json::arrayValue;
	for (SourceLocation const& location: locations)
		reply.append(toJson(location));
	m_client.reply(_id, reply);
}

void LanguageServer::handle_textDocument_highlight(MessageId _id, Json::Value const& _args)
{
	DocumentPosition dpos{};
	loadTextDocumentPosition(dpos, _args);

	Json::Value jsonReply = Json::arrayValue;
	for (DocumentHighlight const& highlight: semanticHighlight(dpos))
	{
		Json::Value item = Json::objectValue;
		item["range"] = toJsonRange(highlight.location);
		if (highlight.kind != DocumentHighlightKind::Unspecified)
			item["kind"] = static_cast<int>(highlight.kind);

		jsonReply.append(item);
	}
	m_client.reply(_id, jsonReply);
}

void LanguageServer::handle_textDocument_references(MessageId _id, Json::Value const& _args)
{
	DocumentPosition dpos{};
	loadTextDocumentPosition(dpos, _args);

	trace(
		"find all references: " +
		dpos.path + ":" +
		to_string(dpos.position.line) + ":" +
		to_string(dpos.position.column)
	);

	vfs::File* const file = m_vfs.find(dpos.path);
	if (!file)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client.reply(_id, emptyResponse); // reply with "No references".
		return;
	}

	if (!m_compilerStack)
		compile(*file);
	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceNode = findASTNode(dpos.position, dpos.path);
	if (!sourceNode)
	{
		Json::Value emptyResponse = Json::arrayValue;
		m_client.reply(_id, emptyResponse); // reply with "No references".
		return;
	}

	auto locations = vector<SourceLocation>{};
	if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);

		if (auto decl = sourceIdentifier->annotation().referencedDeclaration)
			findAllReferences(decl, decl->name(), sourceUnit, locations);

		for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
			findAllReferences(decl, decl->name(), sourceUnit, locations);
	}
	else if (auto const decl = dynamic_cast<VariableDeclaration const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
		findAllReferences(decl, decl->name(), sourceUnit, locations);
	}
	else if (auto const* functionDefinition = dynamic_cast<FunctionDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
		findAllReferences(functionDefinition, functionDefinition->name(), sourceUnit, locations);
	}
	else if (auto const* enumDef = dynamic_cast<EnumDefinition const*>(sourceNode))
	{
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
		findAllReferences(enumDef, enumDef->name(), sourceUnit, locations);
	}
	else if (auto const memberAccess = dynamic_cast<MemberAccess const*>(sourceNode))
	{
		if (Declaration const* decl = memberAccess->annotation().referencedDeclaration)
		{
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(dpos.path);
			findAllReferences(decl, memberAccess->memberName(), sourceUnit, locations);
		}
	}
	else
		trace("references: not an identifier: "s + typeid(*sourceNode).name());

	Json::Value jsonReply = Json::arrayValue;
	for (SourceLocation const& location: locations)
		jsonReply.append(toJson(location));

	m_client.reply(_id, jsonReply);
}

void LanguageServer::log(string const& _message)
{
	if (m_trace < Trace::Messages)
		return;

	Json::Value json = Json::objectValue;
	json["type"] = static_cast<int>(Trace::Messages);
	json["message"] = _message;

	m_client.notify("window/logMessage", json);

	if (m_logger)
		m_logger(_message);
}

void LanguageServer::trace(string const& _message)
{
	if (m_trace < Trace::Verbose)
		return;

	Json::Value json = Json::objectValue;
	json["type"] = static_cast<int>(Trace::Verbose);
	json["message"] = _message;

	m_client.notify("window/logMessage", json);

	if (m_logger)
		m_logger(_message);
}

void LanguageServer::handleMessage(Json::Value const& _jsonMessage)
{
	string const methodName = _jsonMessage["method"].asString();

	MessageId const id = _jsonMessage["id"].isInt()
		? MessageId{_jsonMessage["id"].asInt()}
		: _jsonMessage["id"].isString()
			? MessageId{_jsonMessage["id"].asString()}
			: MessageId{};

	if (auto const handler = m_handlers.find(methodName); handler != m_handlers.end() && handler->second)
	{
		Json::Value const& jsonArgs = _jsonMessage["params"];
		handler->second(id, jsonArgs);
	}
	else
		m_client.error(id, ErrorCode::MethodNotFound, "Unknown method " + methodName);
}
// }}}

} // namespace solidity
