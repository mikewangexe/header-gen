//===- PrintFunctionNames.cpp ---------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Example clang plugin which simply prints the names of all the top-level decls
// in the input file.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
using namespace clang;

#include <cstdlib>
#include <cstdio>
#include <vector>
#include <sqlite3.h>

enum {
	TYPE_MACRO = 1,
	TYPE_TYPEDEF = 2,
	TYPE_STRUCT = 3,
	TYPE_FUNCTION = 4,
	TYPE_ENUM = 5,
	TYPE_UNION = 6,
	TYPE_VAR = 7
};

static const int BUF_SIZE = 40960;
std::map<std::string, int> explored;
#define errs outs

/// simplify path like aaa/xxx/../bbb to aaa/bbb
static std::string SimplifyPath(std::string path) {
	if (path.find("/../") == std::string::npos)
		return path;
	while (path.find("/../") != std::string::npos) {
		int pos_dot = path.find("/../");
		int pos_delete = path.rfind("/", pos_dot - 1);
		std::string left = path.substr(0, pos_delete + 1);
		std::string right = path.substr(pos_dot + 4, path.length() - pos_dot - 4);
		path = left + right;
	}
	return path;
}

/// PrintMacroDefinition - Print a macro definition in a form that will be
/// properly accepted back as a definition.
/// Copied from PrintPreprocessedOutput.cpp
static void PrintMacroDefinition(const IdentifierInfo &II, const MacroInfo &MI,
                                 Preprocessor &PP, raw_ostream &OS) {
	OS << "#define " << II.getName();

	if (MI.isFunctionLike()) {
		OS << '(';
		if (!MI.arg_empty()) {
			MacroInfo::arg_iterator AI = MI.arg_begin(), E = MI.arg_end();
			for (; AI+1 != E; ++AI) {
				OS << (*AI)->getName();
				OS << ',';
			}

			// Last argument.
			if ((*AI)->getName() == "__VA_ARGS__")
				OS << "...";
			else
				OS << (*AI)->getName();
		}

		if (MI.isGNUVarargs())
			OS << "...";  // #define foo(x...)

		OS << ')';
	}

	// GCC always emits a space, even if the macro body is empty.  However, do not
	// want to emit two spaces if the first token has a leading space.
	if (MI.tokens_empty() || !MI.tokens_begin()->hasLeadingSpace())
		OS << ' ';

	SmallString<128> SpellingBuffer;
	for (MacroInfo::tokens_iterator I = MI.tokens_begin(), E = MI.tokens_end();
		 I != E; ++I) {
		if (I->hasLeadingSpace())
			OS << ' ';

		OS << PP.getSpelling(*I, SpellingBuffer);
	}

	OS.flush();
}

std::string& replace_all(std::string& str, const std::string& old_value, const std::string& new_value)
{
	std::string::size_type pos(0);
    while ((pos = str.find(old_value, pos)) != std::string::npos) {
		str.replace(pos, old_value.length(), new_value);
		pos += new_value.length() - old_value.length() + 1;
	}
    return str;
}

class DumpMacrosCallbacks : public PPCallbacks {
	Preprocessor &PP;
	SourceManager& SM;

	sqlite3 *conn;
	char sqlbuf[BUF_SIZE];
	char *errmsg;

	std::string lastIncluded;
	std::vector<std::string> fileStack;

public:
	explicit DumpMacrosCallbacks(Preprocessor& pp, SourceManager& sm, sqlite3 *conn = NULL)
		: PP(pp), SM(sm), conn(conn) {}

	virtual void MacroDefined(const Token &MacroNameTok, const MacroDirective *MD) {
		std::string loc = MacroNameTok.getLocation().printToString(SM);
		std::string name, def;
		llvm::raw_string_ostream os(def);
		
//		llvm::errs() << "Enter MacroDefined, loc = " << loc << '\n';
		// Ignore builtin macros
		if (loc.find("<built-in>:") != std::string::npos)
			return;
		if (loc.find("<command line>:") != std::string::npos)
			return;
//		if (loc.find("linux/kconfig.h") != std::string::npos)
//			return;
//		if (loc.find("generated/autoconf.h") != std::string::npos)
//			return;

		const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
		const MacroInfo *MI = MD->getMacroInfo();
		name = II->getName();
		PrintMacroDefinition(*II, *MI, PP, os);
		
		if (conn) {
			std::size_t first = loc.find(':'), second = loc.find(':', first + 1);
			std::string file = loc.substr(0, first), linum = loc.substr(first + 1, second - first - 1);
		
			// remove /../
			file = SimplifyPath(file);			
	
			if (explored.find(loc) != explored.end()) {
//				llvm::errs() << "find file in explored, ignore it\n";
				return;
			} else
				RecordExploredFiles(loc + name);

			def = replace_all(def, "'", "''");
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
					 name.c_str(), TYPE_MACRO, file.c_str(), linum.c_str(), os.str().c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << "\n";
		} else {
			llvm::outs() << loc << ":\t" << os.str() << "\n";
		}
	}

	void InclusionDirective (SourceLocation HashLoc,
							 const Token & IncludeTok,
							 StringRef FileName,
							 bool isAngled,
							 CharSourceRange FilenameRange,
							 const FileEntry *File,
							 StringRef SearchPath,
							 StringRef RelativePath,
							 const Module *Imported) {
		std::string loc = HashLoc.printToString(SM);
		std::size_t first = loc.find(':'), second = loc.find(':', first + 1);
		std::string file = loc.substr(0, first), linum = loc.substr(first + 1, second - first - 1);

		if (loc.find("<built-in>:") != std::string::npos)
			return;
		if (loc.find("<command line>:") != std::string::npos)
			return;
		if (loc.find("linux/kconfig.h") != std::string::npos)
			return;
		if (loc.find("generated/autoconf.h") != std::string::npos)
			return;

		if (conn) {
			if (!fileStack.empty()) {
				snprintf(sqlbuf, BUF_SIZE, "INSERT INTO incdeps VALUES ('%s', %s, '%s')",
						 fileStack.back().c_str(), linum.c_str(), FileName.str().c_str());
				if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
					llvm::errs() << sqlbuf << ": " << errmsg << "\n";
			}
		} else {
			if (!fileStack.empty())
				llvm::outs() << "[" << fileStack.back() << "] ";
			llvm::outs() << loc << " => " << FileName << "\n";
		}
		lastIncluded = FileName.str();
	}

	/*
	 * function to record files that had been explored
	 */
	void RecordExploredFiles(std::string file) {
		if (conn && explored.find(file) == explored.end()) {
			explored.insert(std::pair<std::string, int>(file, 1));
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO explored VALUES ('%s')", file.c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << '\n';
		}
	}

	void FileChanged(SourceLocation Loc,
					 FileChangeReason Reason,
					 SrcMgr::CharacteristicKind FileType,
					 FileID PrevFID) {
		switch (Reason) {
		case EnterFile:
//			llvm::errs() << "enter FileChanged, file = " << SM.getFilename(Loc) << ", preFile = " << SM.getFilename(SM.getLocForEndOfFile(PrevFID));
//			llvm::errs() << ", change reason: EnterFile" << '\n';
			if (SM.getFileEntryForID(SM.getFileID(Loc)))
				fileStack.push_back(lastIncluded);
			break;
		case ExitFile:
//			llvm::errs() << "enter FileChanged, file = " << SM.getFilename(Loc) << ", preFile = " << SM.getFilename(SM.getLocForStartOfFile(PrevFID));
//			llvm::errs() << ", change reason: ExitFile" << '\n';
			if (SM.getFileEntryForID(PrevFID))
				fileStack.pop_back();
			break;
		default:
			break;
		}
	}

};

class DumpDeclsConsumer : public ASTConsumer {

	sqlite3 *conn;
	char sqlbuf[BUF_SIZE];
	char *errmsg;

	struct DefInfo {
		std::string def;
		int type;
	};
	std::map<std::string, DefInfo> defs;

	std::string getLocStart(const Decl *d) {
		std::string loc = d->getLocStart().printToString(d->getASTContext().getSourceManager());
		std::size_t pos = loc.rfind(':');
		return loc.substr(0, pos);
	}

	std::string getLocation(const Decl *d) {
		std::string loc = d->getLocEnd().printToString(d->getASTContext().getSourceManager());
		std::size_t pos = loc.rfind(':');
		return loc.substr(0, pos);
	}


	std::string nameAnonymous(std::string loc) {
		loc.erase(std::remove(loc.begin(), loc.end(), '.'), loc.end());
		replace(loc.begin(), loc.end(), '/', '_');
		replace(loc.begin(), loc.end(), ':', '_');
		replace(loc.begin(), loc.end(), '-', '_');
		return loc;
	}

	std::string getTypeString(int type) {
		switch(type) {
		case TYPE_ENUM:
			return "enum ";
		case TYPE_STRUCT:
			return "struct ";
		case TYPE_UNION:
			return "union ";
		}
		return "";
	}

	std::string getTypeString(const QualType qt) {
		if (qt->isEnumeralType())
			return "enum ";
		if (qt->isStructureType())
			return "struct ";
		if (qt->isUnionType())
			return "union ";
		return "";
	}

	std::string printNameWithType(std::string name, std::string type, bool addFormal = false) {
		/* Handle function pointers */
		std::size_t pos = type.find("(*)");
		if (pos != std::string::npos) {
			std::string acc = type.insert(pos + 2, name);
			if (addFormal) {
				char arg[3] = " a";
				pos = acc.find("(void)");
				if (pos == std::string::npos) {
					while ((pos = acc.find(",", pos)) != std::string::npos) {
						acc = acc.insert(pos, arg);
						arg[1] ++;
						pos += 3;
					}
					pos = acc.find("...");
					if (pos == std::string::npos) {
						pos = 0;
						while ((pos = acc.find("(*)", pos)) != std::string::npos)
							pos ++;
						pos = acc.rfind(")");
						acc = acc.insert(pos, arg);
					}
				}
			}
			return acc;
		}

		if (type.find("typeof") == std::string::npos) {
			pos = type.find("(");
			if (pos != std::string::npos) {
				std::string ingre = "(" + name + ")";
				std::string acc = type.insert(pos, ingre);
				return acc;
			}
		}

		/* Handle arrays */
		pos = type.find("[");
		if (pos != std::string::npos)
			return type.insert(pos, name);

		return type + " " + name;
	}

	void printFunction(const FunctionDecl *d) {
		std::string name = d->getNameAsString();
		std::string ret = d->getResultType().getAsString();
		std::vector<std::string> args;
		std::string location = getLocation(d);

			if (explored.find(location + name) != explored.end()) {
				return;
			}
			else
				RecordExploredFiles(location + name);

		for (FunctionDecl::param_const_iterator i = d->param_begin(), e = d->param_end();
			 i != e;
			 i++) {
			QualType type = (*i)->getOriginalType();
			args.push_back(type.getAsString());
		}

		std::size_t first = location.find(':'), second = location.find(':', first + 1);
		std::string file = location.substr(0, first), linum = location.substr(first + 1, second - first - 1);
		std::string def;
		llvm::raw_string_ostream os(def);

		// remove /../
		file = SimplifyPath(file);			
	
		os << ret << " " << name << "(";
		char pn[2] = "a";
		for (int i = 0, size = args.size(); i < size; i++) {
			// os << args[i] << " " << (char)('a' + i);
			pn[0] = 'a' + i;
			os << printNameWithType(pn, args[i]);
			if (i < size - 1)
				os << ", ";
		}
		if (d->isVariadic())
			os << ", ...";
		os << ")";

		if (conn) {
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
					 name.c_str(), TYPE_FUNCTION, file.c_str(), linum.c_str(), os.str().c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << "\n";
		} else {
			llvm::outs() << location << ":\t" << os.str() << "\n";
		}
	}

	void printRecord(const RecordDecl *d, bool recording = false, int linumBefore = -1) {
		std::string name = d->getNameAsString();
		std::vector<std::pair<std::string, std::string> > fields;

		std::string location = getLocation(d);
		std::size_t first = location.find(':'), second = location.find(':', first + 1);
		std::string file = location.substr(0, first);
		int linum = atoi(location.substr(first + 1, second - first - 1).c_str());
		bool anonymous = false;
	
			if (explored.find(location + name) != explored.end()) {
				return;
			}
			else
				RecordExploredFiles(location + name);

		if (d->getDefinition() && d->getDefinition() != d)
			return;

		if (name.empty()) {
			TypedefNameDecl *tnd = d->getTypedefNameForAnonDecl();
			if (tnd && !tnd->getNameAsString().empty())
				name = tnd->getNameAsString();
			else {
				name = nameAnonymous(location);
				anonymous = true;
			}
		}

		for (RecordDecl::decl_iterator i = d->decls_begin(), e = d->decls_end();
			 i != e;
			 i ++) {
			int before = linum;
			if (linumBefore >= 0 && linum >= linumBefore)
				before = linumBefore - 1;
			if (const RecordDecl *RD = dyn_cast<RecordDecl>(*i))
				printRecord(RD, true);
			if (const EnumDecl *ED = dyn_cast<EnumDecl>(*i))
				printEnum(ED, true);
		}

		for (RecordDecl::field_iterator i = d->field_begin(), e = d->field_end();
			 i != e;
			 i++) {
			std::string name = i->getNameAsString();
			const QualType qt = i->getType();
			std::string type;
			if (qt->hasUnnamedOrLocalType()) {
				if (name.empty())
					type = getTypeString(qt) + nameAnonymous(getLocStart(*i));
				else
					type = getTypeString(qt) + nameAnonymous(getLocation(*i));
			} else {
				type = qt.getAsString();
			}
			fields.push_back(std::make_pair(name, type));
		}

		std::string def;
		llvm::raw_string_ostream os(def);

		os << "{ ";
		char c[2] = "a";
		for (int i = 0, size = fields.size(); i < size; i++) {
			std::pair<std::string, std::string> field = fields[i];
			std::string decl;
			if (field.first == "") {
				decl = getTypeString(defs[field.second].type) + defs[field.second].def;
			} else {
				decl = printNameWithType(field.first, field.second);
			}
			if (conn) {
				std::string fname = field.first;
				if (fname == "") {
					fname = "anonymous_";
					fname.append(c);
					c[0] ++;
				}
				snprintf(sqlbuf, BUF_SIZE, "INSERT INTO record_fields VALUES ('%s %s', '%s', '%s')",
						 d->isUnion() ? "union" : "struct", name.c_str(),
						 fname.c_str(),
						 decl.c_str());
				if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
					llvm::errs() << sqlbuf << ": " << errmsg << "\n";
			}
			os << decl << "; ";
		}
		os << "}";

		if (recording) {
			std::string n = (d->isUnion() ? "union " : "struct ") +
				(anonymous ? nameAnonymous(getLocStart(d)) : name);
			defs[n].def = os.str();
			defs[n].type = d->isUnion() ? TYPE_UNION : TYPE_STRUCT;
		}

		if (linumBefore >= 0 && linum > linumBefore)
			linum = linumBefore;

			// remove /../
			file = SimplifyPath(file);			
	
		if (conn) {
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %d, '%s %s %s')",
					 /*d->isUnion() ? "union" : "struct",*/ name.c_str(),
					 TYPE_STRUCT,
					 file.c_str(),
					 linum,
					 d->isUnion() ? "union" : "struct", name.c_str(), os.str().c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << "\n";
		} else {
			llvm::outs() << location << ":\t" << (d->isUnion() ? "union " : "struct ")
						 << name.c_str() << " " << os.str() << "\n";
		}
	}

	void printTypedef(const TypedefDecl *d) {
		std::string name = d->getNameAsString();
		std::string type = d->getUnderlyingType().getAsString();
		std::string location = getLocation(d);

			if (explored.find(location + name) != explored.end()) {
				return;
			}
			else
				RecordExploredFiles(location + name);

		if (conn) {
			std::size_t first = location.find(':'), second = location.find(':', first + 1);
			std::string file = location.substr(0, first), linum = location.substr(first + 1, second - first - 1);
			std::string def;
			llvm::raw_string_ostream os(def);

			os << "typedef " << printNameWithType(name, type);

			// remove /../
			file = SimplifyPath(file);			
	
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
					 name.c_str(), TYPE_TYPEDEF, file.c_str(), linum.c_str(), os.str().c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << "\n";
		} else {
			llvm::outs() << location << ":\t"
						 << "typedef " << printNameWithType(name, type) << "\n";
		}
	}

	void printEnum(const EnumDecl *d, bool recording = false) {
		std::string name = d->getNameAsString();
		std::string location = getLocation(d);
		std::string def;
		llvm::raw_string_ostream os(def);
		bool anonymous = false;

			if (explored.find(location + name) != explored.end()) {
				return;
			}
			else
				RecordExploredFiles(location + name);

		if (name.empty()) {
			TypedefNameDecl *tnd = d->getTypedefNameForAnonDecl();
			if (tnd && !tnd->getNameAsString().empty())
				name = tnd->getNameAsString();
			else {
				name = nameAnonymous(location);
				anonymous = true;
			}
		}

		os << "enum " << name << " {";
		int entries = 0;
		for (EnumDecl::enumerator_iterator i = d->enumerator_begin(), e = d->enumerator_end();
			 i != e;
			 i ++) {
			os << i->getNameAsString() << " = " << i->getInitVal().toString(10) << ", ";
			entries ++;
		}
		os << "};";

		if (entries == 0)
			return;

		if (recording) {
			std::string n = "enum " + (anonymous ? nameAnonymous(getLocStart(d)) : name);
			defs[n].def = os.str();
			defs[n].type = d->isUnion() ? TYPE_UNION : TYPE_STRUCT;
		}

		if (conn) {
			std::size_t first = location.find(':'), second = location.find(':', first + 1);
			std::string file = location.substr(0, first), linum = location.substr(first + 1, second - first - 1);

			// remove /../
			file = SimplifyPath(file);			
	
			if (!name.empty()) {
				snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
						 name.c_str(), TYPE_ENUM, file.c_str(), linum.c_str(), os.str().c_str());
				if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
					llvm::errs() << sqlbuf << ": " << errmsg << "\n";
			}

			for (EnumDecl::enumerator_iterator i = d->enumerator_begin(), e = d->enumerator_end();
				 i != e;
				 i ++) {
				snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
						 i->getNameAsString().c_str(), TYPE_ENUM, file.c_str(), linum.c_str(),
						 os.str().c_str());
				if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
					llvm::errs() << sqlbuf << ": " << errmsg << "\n";
			}
		} else {
			llvm::outs() << location << ":\t" << os.str() << "\n";
		}
	}

	void printVar(const VarDecl *d) {
		std::string name = d->getNameAsString();
		std::string type = d->getType().getAsString();
		std::string location = getLocation(d);

		if (conn) {
			std::size_t first = location.find(':'), second = location.find(':', first + 1);
			std::string file = location.substr(0, first), linum = location.substr(first + 1, second - first - 1);
			std::string def;
			llvm::raw_string_ostream os(def);

			os << "extern " << printNameWithType(name, type);

			if (explored.find(location + name) != explored.end()) {
				return;
			}
			else
				RecordExploredFiles(location + name);

			// remove /../
			file = SimplifyPath(file);			
	
			snprintf(sqlbuf, BUF_SIZE, "INSERT INTO decls VALUES ('%s', %d, '%s', %s, '%s')",
					 name.c_str(), TYPE_VAR, file.c_str(), linum.c_str(), os.str().c_str());
			if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
				llvm::errs() << sqlbuf << ": " << errmsg << "\n";
		} else {
			llvm::outs() << location << ":\t"
						 << "extern " << printNameWithType(name, type) << "\n";
		}
	}

/*
 * function to record files that had been explored
 */
void RecordExploredFiles(std::string file) {
	if (conn) {
		explored.insert(std::pair<std::string, int>(file, 1));
		snprintf(sqlbuf, BUF_SIZE, "INSERT INTO explored VALUES ('%s')", file.c_str());
		if (sqlite3_exec(conn, sqlbuf, 0, 0, &errmsg) != SQLITE_OK)
			llvm::errs() << sqlbuf << ": " << errmsg << '\n';
	}
}

public:
	explicit DumpDeclsConsumer(sqlite3 *conn = NULL) : conn(conn) {}

	virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
//		Decl *d = *DG.begin();
//		llvm::errs() << "enter HandleTopLevelDecl, file = " << getLocation(d) << "\n";
//		std::string file = getLocation(d);
//		if (explored.find(file) != explored.end()) {
//			llvm::errs() << "find file in explored, ignore it\n";
//			return true;
//		}
//		else
//			RecordExploredFiles(file);	
				
		for (DeclGroupRef::iterator i = DG.begin(), e = DG.end(); i != e; i++) {
			const Decl *D = *i;
			if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
				printFunction(FD);
			else if (const RecordDecl *RD = dyn_cast<RecordDecl>(D))
				printRecord(RD);
			else if (const TypedefDecl *TD = dyn_cast<TypedefDecl>(D))
				printTypedef(TD);
			else if (const EnumDecl *ED = dyn_cast<EnumDecl>(D))
				printEnum(ED);
			else if (const VarDecl *VD = dyn_cast<VarDecl>(D))
				printVar(VD);
		}

		return true;
	}
};

class DumpDeclsAction : public PluginASTAction {
	sqlite3 *conn;

protected:
	ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) {
		return new DumpDeclsConsumer(conn);
	}

	bool ParseArgs(const CompilerInstance &CI,
			       const std::vector<std::string>& args) {
		conn = NULL;
		char sqlbuf[BUF_SIZE];
		char *errmsg;
		
		if (args.size()) {
			if (args[0] == "help") {
				PrintHelp(llvm::errs());
				return false;
			} else {
				std::string database = args[0];
				sqlite3_open(database.c_str(), &conn);
				sqlite3_exec(conn, "begin;", 0, 0, 0);

				snprintf(sqlbuf, BUF_SIZE, "SELECT * FROM explored");
				char **result;
				int nrow = 0, ncolumn = 0;
				if (sqlite3_get_table(conn, sqlbuf, &result, &nrow, &ncolumn, &errmsg) != SQLITE_OK)
					llvm::errs() << sqlbuf << ": " << errmsg << '\n';
				llvm::errs() << "Read " << nrow << " filenames from table explored\n";
				for (int i = 0; i < nrow; i++) {
					std::string file(result[i]);
					explored.insert(std::pair<std::string, int>(file, 1));
				}
				sqlite3_free_table(result);
			}
		}

		return true;
	}

	void PrintHelp(llvm::raw_ostream& ros) {
		ros << "Help for DumpDecls plugin goes here\n";
	}

	bool BeginSourceFileAction(CompilerInstance& CI, llvm::StringRef) {
		Preprocessor &PP = CI.getPreprocessor();
		PP.addPPCallbacks(new DumpMacrosCallbacks(PP, CI.getSourceManager(), conn));
		return true;
	}

public:
	virtual ~DumpDeclsAction() {
		if (conn) {
			sqlite3_exec(conn, "commit;", 0, 0, 0);
			sqlite3_close(conn);
		}
	}
};

static FrontendPluginRegistry::Add<DumpDeclsAction>
X("dump-decls", "dump macros and declarations");
