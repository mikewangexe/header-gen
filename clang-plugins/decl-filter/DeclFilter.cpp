//===- DeclFilter.cpp -----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Filter out unused declarations in the input file.
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

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <list>
#include <map>
#include <sqlite3.h>

#define out llvm::outs() << ">>> "
static const int BUF_SIZE = 1024;

static sqlite3 *conn;
static char sqlbuf[BUF_SIZE];
static char *sqlerrmsg;

static StringRef currentFile, nextFile;

void executeSql(const char *format, ...) {
	if (!conn)
		return;

	va_list ap;
	va_start(ap, format);
	vsnprintf(sqlbuf, BUF_SIZE, format, ap);
	va_end(ap);
	sqlite3_exec(conn, sqlbuf, 0, 0, &sqlerrmsg);
	// if (sqlite3_exec(conn, sqlbuf, 0, 0, &sqlerrmsg) != SQLITE_OK)
	// 	out << sqlbuf << ": " << sqlerrmsg << "\n";
}

class DeclFilterCallbacks : public PPCallbacks {
	SourceManager& SM;

	void addMacro(const Token &MacroNameTok,
				  const MacroDirective *MD) {
		const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
		const MacroInfo *MI = MD->getMacroInfo();
		clang::SourceLocation start = MI->getDefinitionLoc(), end = MI->getDefinitionEndLoc();

		std::string name = II->getName();
		llvm::StringRef file = SM.getFilename(start);
		int startLine = SM.getExpansionLineNumber(start), startColumn = SM.getExpansionColumnNumber(start);
		int endLine = SM.getExpansionLineNumber(end), endColumn = SM.getExpansionColumnNumber(end);

		executeSql("INSERT INTO macros VALUES ('%s', '%s', %d, %d, %d, %d)",
				   file.str().c_str(), name.c_str(), startLine, startColumn, endLine, endColumn);
	}

	void removeMacro(const Token &MacroNameTok) {
		const IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
		clang::SourceLocation loc = MacroNameTok.getLocation();

		std::string name = II->getName();
		llvm::StringRef file = SM.getFilename(loc);
		int line = SM.getExpansionLineNumber(loc);

		executeSql("INSERT INTO macros VALUES ('%s', '%s', %d, %d, %d, %d)",
				   file.str().c_str(), name.c_str(), line, 1, line, 1);
	}

public:
	explicit DeclFilterCallbacks(SourceManager& sm)
		: SM(sm) {
		executeSql("CREATE TABLE IF NOT EXISTS deps (header TEXT NOT NULL, included TEXT NOT NULL, included_path TEXT NOT NULL, line INTEGER, force_keep INTEGER, PRIMARY KEY(header, included))");
		executeSql("CREATE TABLE IF NOT EXISTS macros (header TEXT NOT NULL, name TEXT NOT NULL, start_line INTEGER, start_column INTEGER, end_line INTEGER, end_column INTEGER, PRIMARY KEY(header, name, start_line))");
	}

	virtual void MacroUndefined(const Token &MacroNameTok, const MacroDirective *MD) {
		if (MD)
			removeMacro(MacroNameTok);
	}

	virtual void Defined(const Token &MacroNameTok,
						 const MacroDirective *MD) {
		if (MD)
			addMacro(MacroNameTok, MD);
	}

	virtual void Ifdef(SourceLocation Loc,
					   const Token &MacroNameTok,
					   const MacroDirective *MD) {
		if (MD)
			addMacro(MacroNameTok, MD);
	}

	virtual void Ifndef(SourceLocation Loc,
						const Token &MacroNameTok,
						const MacroDirective *MD) {
		if (MD)
			addMacro(MacroNameTok, MD);
	}

	virtual void MacroExpands(const Token &MacroNameTok,
							  const MacroDirective *MD,
							  SourceRange Range,
							  const MacroArgs *Args) {
		if (MD)
			addMacro(MacroNameTok, MD);
	}

	virtual void InclusionDirective(SourceLocation HashLoc,
									const Token & IncludeTok,
									StringRef FileName,
									bool isAngled,
									CharSourceRange FilenameRange,
									const FileEntry *File,
									StringRef SearchPath,
									StringRef RelativePath,
									const Module *Imported) {
		if (!File)
			return;

		llvm::StringRef file = SM.getFilename(HashLoc);
		int line = SM.getExpansionLineNumber(HashLoc);

		executeSql("INSERT INTO deps VALUES ('%s', '%s', '%s', %d, 0)", file.str().c_str(), FileName.str().c_str(), File->getName(), line);
	}

	virtual void FileChanged(SourceLocation Loc,
							 FileChangeReason Reason,
							 SrcMgr::CharacteristicKind FileType,
							 FileID PrevFID) {
		if (currentFile.empty())
			currentFile = SM.getFilename(Loc);
		else
			nextFile = SM.getFilename(Loc);

		switch (Reason) {
		case ExitFile:
			if (SM.getFileEntryForID(PrevFID)) {
				const char *included = SM.getFileEntryForID(PrevFID)->getName();
				executeSql("UPDATE deps SET force_keep = 1 WHERE header = '%s' and included_path = '%s'",
						   SM.getFilename(Loc).str().c_str(), included);
			}
			break;
		default:
			break;
		}
	}
};

class DeclFilterConsumer : public ASTConsumer {
	std::list<Decl *> _Ds;
	std::map<Decl *, llvm::StringRef> _locations;

	void markDeclReferenced(Decl *D) {
		if (D->isReferenced())
			return;
		D->setReferenced();
		_Ds.push_back(D);

		// Note: include forward declarations as well
		if (dyn_cast<RecordDecl>(D)) {
			for (Decl::redecl_iterator i = D->redecls_begin(), e = D->redecls_end(); i != e; i ++) {
				Decl *rd = *i;
				if (rd != D)
					_Ds.push_back(rd);
			}
		}
	}

	void markTypeReferenced(const QualType &QT) {
		const Type *T = QT.getTypePtr();

		if (dyn_cast<const BuiltinType>(T) || dyn_cast<const TypeOfExprType>(T))
			return;

		// Note: this check must be placed before RecordType checks as
		//       getAsXXXType() may strip off the typedef information
		if (const TypedefType *TT = dyn_cast<const TypedefType>(T)) {
			markDeclReferenced(TT->getDecl());
			return;
		}

		if (const RecordType *RT = T->getAsStructureType()) {
			markDeclReferenced(RT->getDecl());
			return;
		}

		if (const RecordType *RT = T->getAsUnionType()) {
			markDeclReferenced(RT->getDecl());
			return;
		}

		if (const EnumType *ET = dyn_cast<const EnumType>(T)) {
			markDeclReferenced(ET->getDecl());
			return;
		}

		if (const PointerType *PT = dyn_cast<const PointerType>(T)) {
			markTypeReferenced(PT->getPointeeType());
			return;
		}

		if (const ElaboratedType *ET = dyn_cast<const ElaboratedType>(T)) {
			markTypeReferenced(ET->getNamedType());
			return;
		}

		if (const ArrayType *AT = dyn_cast<const ArrayType>(T)) {
			markTypeReferenced(AT->getElementType());
			return;
		}

		if (const TypeOfType *TOT = dyn_cast<const TypeOfType>(T)) {
			markTypeReferenced(TOT->getUnderlyingType());
			return;
		}

		if (const FunctionProtoType *FPT = dyn_cast<const FunctionProtoType>(T)) {
			for (unsigned int i = 0; i < FPT->getNumArgs(); i ++)
				markTypeReferenced(FPT->getArgType(i));
			markTypeReferenced(FPT->getResultType());
			return;
		}

		if (const PointerType *PT = dyn_cast<const PointerType>(T)) {
			markTypeReferenced(PT->getPointeeType());
			return;
		}

		if (const ParenType *PT = dyn_cast<const ParenType>(T)) {
			markTypeReferenced(PT->getInnerType());
			return;
		}

		out << "not handled class(" << T->getTypeClass() << ") " << QT.getAsString() << "\n";
	}

	void markDependencies(Decl *D) {
		if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
			for (FunctionDecl::param_const_iterator i = FD->param_begin(), e = FD->param_end(); i != e; i++)
				markTypeReferenced((*i)->getOriginalType());
			markTypeReferenced(FD->getResultType());
		} else if (RecordDecl *RD = dyn_cast<RecordDecl>(D)) {
			for (RecordDecl::decl_iterator i = RD->decls_begin(), e = RD->decls_end(); i != e; i++) {
				// XXX: Is this correct?!
				markDeclReferenced(*i);
			}
			for (RecordDecl::field_iterator i = RD->field_begin(), e = RD->field_end(); i != e; i++)
				markTypeReferenced(i->getType());
		} else if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D)) {
			markTypeReferenced(TD->getUnderlyingType());
		} else if (dyn_cast<EnumDecl>(D)) {
			/* Enums consist of constants and have no more declarations in them */
		} else if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
			markTypeReferenced(VD->getType());
		} else if (FieldDecl *FD = dyn_cast<FieldDecl>(D)) {
			markTypeReferenced(FD->getType());
		} else if (IndirectFieldDecl *IFD = dyn_cast<IndirectFieldDecl>(D)) {
			markTypeReferenced(IFD->getType());
		} else if (dyn_cast<EmptyDecl>(D)) {
		} else {
			out << "Unhandled decl: " << D->getDeclKindName() << "\n";
		}
	}

	llvm::StringRef tryFindFile(Decl *d) {
		return _locations[d];
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

	void dumpFunction(const FunctionDecl *d, llvm::StringRef file) {
		std::string name = d->getNameAsString();
		std::string ret = d->getResultType().getAsString();
		std::vector<std::string> args;

		for (FunctionDecl::param_const_iterator i = d->param_begin(), e = d->param_end();
			 i != e;
			 i++) {
			QualType type = (*i)->getOriginalType();
			args.push_back(type.getAsString());
		}

		std::string def;
		llvm::raw_string_ostream os(def);

		os << ret << " " << name << "(";
		char pn[2] = "a";
		for (int i = 0, size = args.size(); i < size; i++) {
			pn[0] = 'a' + i;
			os << printNameWithType(pn, args[i]);
			if (i < size - 1)
				os << ", ";
		}
		if (d->isVariadic())
			os << ", ...";
		os << ")";

		executeSql("INSERT INTO prototypes VALUES ('%s', '%s', '%s', %d)", name.c_str(), os.str().c_str(), file.str().c_str(), 1);
	}

	void dumpVar(const VarDecl *d, llvm::StringRef file) {
		std::string name = d->getNameAsString();
		std::string type = d->getType().getAsString();
		std::string def;
		llvm::raw_string_ostream os(def);

		os << "extern " << printNameWithType(name, type);

		executeSql("INSERT INTO prototypes VALUES ('%s', '%s', '%s', %d)", name.c_str(), os.str().c_str(), file.str().c_str(), 0);
	}

public:
	explicit DeclFilterConsumer() {
		executeSql("CREATE TABLE IF NOT EXISTS prototypes (name TEXT NOT NULL, prototype TEXT, header TEXT, is_function INTEGER, PRIMARY KEY(name))");
		executeSql("CREATE TABLE IF NOT EXISTS decls (header TEXT NOT NULL, name TEXT NOT NULL, start_line INTEGER, start_column INTEGER, end_line INTEGER, end_column INTEGER, kind INTEGER, from_macro INTEGER, has_body INTEGER, PRIMARY KEY(header, name, start_line, kind))");
		executeSql("CREATE TABLE IF NOT EXISTS all_decls (header TEXT NOT NULL, ident TEXT NOT NULL, start_line INTEGER, start_column INTEGER, end_line INTEGER, end_column INTEGER, PRIMARY KEY(header, ident, start_line))");
	}

	virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
		for (DeclGroupRef::iterator i = DG.begin(), e = DG.end(); i != e; i++) {
			Decl *D = *i;

			// XXX: Reuse the TopLevelDeclInObjCContainer flag to mark this decl as toplevel
			D->setTopLevelDeclInObjCContainer();

			std::string name = "";
			if (const NamedDecl *ND = dyn_cast<const NamedDecl>(D))
				name = ND->getNameAsString();

			clang::SourceManager &SM = D->getASTContext().getSourceManager();
			clang::SourceLocation start = D->getLocStart(), end = D->getLocEnd();
			llvm::StringRef file = SM.getFilename(start);
			int startLine = SM.getExpansionLineNumber(start), startColumn = SM.getExpansionColumnNumber(start);
			int endLine = SM.getExpansionLineNumber(end), endColumn = SM.getExpansionColumnNumber(end);

			if (file.empty())
				_locations[D] = currentFile;

			// XXX Currently the last declaration is seen after file changes. So
			//   we need to record the next file in FileChanged and switch to it
			//   after we have seen the last declaration of the original file.
			if (!nextFile.empty()) {
				currentFile = nextFile;
				nextFile = "";
			}

			if (name != "")
				executeSql("INSERT INTO all_decls VALUES ('%s', '%s', %d, %d, %d, %d)",
						   file.str().c_str(), name.c_str(),
						   startLine, startColumn, endLine, endColumn);
			if (EnumDecl *ED = dyn_cast<EnumDecl>(D)) {
				for (EnumDecl::enumerator_iterator i = ED->enumerator_begin(), e = ED->enumerator_end();
					 i != e;
					 i ++)
					executeSql("INSERT INTO all_decls VALUES ('%s', '%s', %d, %d, %d, %d)",
							   file.str().c_str(), i->getNameAsString().c_str(),
							   startLine, startColumn, endLine, endColumn);
			}

			_Ds.push_back(D);
		}
		return true;
	}

	virtual void PrintStats() {
		// 1. Remove unreferenced decls
		//    Only decls used in the main file are marked referenced currently.
		std::list<Decl *>::iterator i = _Ds.begin();
		while (i != _Ds.end()) {
			Decl *D = *i;

			std::string name = "";
			if (const NamedDecl *ND = dyn_cast<const NamedDecl>(D))
				name = ND->getNameAsString();

			// Note: Cannot ignore declarations in the main source file
			clang::SourceManager &SM = D->getASTContext().getSourceManager();
			clang::SourceLocation start = D->getLocStart();
			llvm::StringRef file = SM.getFilename(start);
			if (file.empty())
				file = tryFindFile(D);

			if (!D->isReferenced() && !file.endswith(".c"))
				_Ds.erase(i++);
			else
				i++;
		}

		// 2. Iterate the decl list until it is empty
		i = _Ds.begin();
		while (!_Ds.empty()) {
			Decl *D = *i;
			int from_macro = 0;

			std::string name = "";
			if (const NamedDecl *ND = dyn_cast<const NamedDecl>(D))
				name = ND->getNameAsString();
			if (name.empty()) {
				const RecordDecl *RD = dyn_cast<const RecordDecl>(D);
				if (RD) {
					TypedefNameDecl *TND = RD->getTypedefNameForAnonDecl();
					if (TND && !TND->getNameAsString().empty())
						name = TND->getNameAsString();
				}
			}

			clang::SourceManager &SM = D->getASTContext().getSourceManager();
			clang::SourceLocation loc = D->getLocStart();
			llvm::StringRef file = SM.getFilename(loc);

			if (file.empty()) {
				// Note: declarations expanded from macros are located in a
				//       'scratch space', which leads us to an empty @file. Here
				//       we try to find where this declaration resides by
				//       looking at the other declarations following.
				from_macro = 1;
				file = tryFindFile(D);
			}
			if (file.endswith(".c")) {
				markDependencies(D);
				_Ds.erase(i++);
				continue;
			}

			markDependencies(D);

			clang::SourceLocation start = D->getLocStart(), end = D->getLocEnd();
			int startLine = SM.getExpansionLineNumber(start), startColumn = SM.getExpansionColumnNumber(start);
			int endLine = SM.getExpansionLineNumber(end), endColumn = SM.getExpansionColumnNumber(end);

			// Note: Only mark top level decls as nested decls will be automatically included
			if (D->isTopLevelDeclInObjCContainer()) {
				executeSql("INSERT INTO decls VALUES ('%s', '%s', %d, %d, %d, %d, %d, %d, %d)",
						   file.str().c_str(), name.c_str(), startLine, startColumn, endLine, endColumn,
						   D->getKind(), from_macro, D->hasBody() ? 1 : 0);
				if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
					dumpFunction(FD, file);
				else if (VarDecl *VD = dyn_cast<VarDecl>(D))
					dumpVar(VD, file);
			}
			_Ds.erase(i++);
		}
	}
};

class DeclFilterAction : public PluginASTAction {
protected:
	ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) {
		return new DeclFilterConsumer();
	}

	bool ParseArgs(const CompilerInstance &CI,
			       const std::vector<std::string>& args) {
		conn = NULL;

		if (args.size() > 0) {
			std::string database = args[0];
			sqlite3_open(database.c_str(), &conn);
			sqlite3_exec(conn, "begin;", 0, 0, 0);
		}

		return true;
	}

	bool BeginSourceFileAction(CompilerInstance& CI, llvm::StringRef) {
		Preprocessor &PP = CI.getPreprocessor();
		PP.addPPCallbacks(new DeclFilterCallbacks(CI.getSourceManager()));
		return true;
	}

public:
	virtual ~DeclFilterAction() {
		if (conn) {
			sqlite3_exec(conn, "commit;", 0, 0, 0);
			sqlite3_close(conn);
		}
		out << "========== done ==========\n";
	}
};

static FrontendPluginRegistry::Add<DeclFilterAction>
X("decl-filter", "filters out unused declarations");
