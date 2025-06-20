// © 2020 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: CC0-1.0

// Install build dependencies:
// $ dnf install llvm-devel clang-devel

// Build:
// $ g++ -std=c++17 -Wall genostream.cpp -o genostream -lclang-cpp -lLLVM

// Example usage:
// $ genostream -p build src/foo.cpp
// `-p build` should be a directory with `compile_commands.json` for
// `src/foo.cpp`

// If the tool fails to find `stddef.h` or similar headers move the binary to
// the same directory as clang or specify the clang resource dir:
// `--extra-arg="-resource-dir /usr/lib64/clang/10.0.1/"`. See
// <https://clang.llvm.org/docs/LibTooling.html#builtin-includes>.

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

constexpr static bool kUseUP = true;

auto EnumMatcher = enumDecl(isExpansionInMainFile()).bind("enum");

auto RecordMatcher =
    recordDecl(isExpansionInMainFile(), unless(isImplicit())).bind("record");

class Printer : public MatchFinder::MatchCallback
{
public:
    virtual void run(const MatchFinder::MatchResult &Result)
    {
        if (const auto *Enum = Result.Nodes.getNodeAs<EnumDecl>("enum"))
        {
            // Enum->dump();
            auto fullname = "::" + Enum->getQualifiedNameAsString();
            if (Enum->getName().empty())
            {
                outs() << "// skipping " << Enum->getName()
                       << ": name empty.s\n";
            }
            outs() << "inline std::ostream & operator<<(std::ostream &os, "
                      "[[maybe_unused]] "
                   << fullname << " e) {\n"
                   << "  switch (e) {\n";
            for (const EnumConstantDecl *EnumConstant : Enum->enumerators())
            {
                auto case_name =
                    "::" + EnumConstant->getQualifiedNameAsString();
                outs() << "  case " << case_name << ": os << \""
                       << EnumConstant->getName() << "\"; break;\n";
            }
            outs() << "  default: os << \"Unknown " << fullname << "("
                   << "\" "
                   << "<< (int) e << "
                   << "\")\"; \n";
            outs() << "  }\n  return os;\n}\n";
        }
        if (const auto *Record = Result.Nodes.getNodeAs<RecordDecl>("record"))
        {
            // Record->dump();
            // outs() << Record->getAccess() << "\n";
            auto fullname = "::" + Record->getQualifiedNameAsString();
            if (Record->isAnonymousStructOrUnion())
            {
                outs() << "// SKipping " << fullname
                       << ": anonymous struct or union \n\n";
                return;
            }
            if (Record->getName().empty())
            {
                outs() << "// Skipping " << fullname << ": empty name.\n\n";
                return;
            }
            outs() << "inline std::ostream& operator<<(std::ostream &os, "
                      "[[maybe_unused]] const "
                   << fullname << " &v) {\n"
                   << "  os << \"{" << Record->getName() << " \";\n";
            for (const FieldDecl *Field : Record->fields())
            {
                bool is_public =
                    Field->getAccess() == clang::AccessSpecifier::AS_public;
                if (!is_public)
                {
                    outs() << "  // Skipping " << Field->getName()
                           << ": not public.\n";
                    continue;
                }
                bool IsFirst = Field == *Record->field_begin();
                if constexpr (kUseUP)
                {
                    outs() << "  os << \"" << (IsFirst ? "" : ", ")
                           << Field->getName() << ": \" << "
                           << "util::pre("
                           << "v." << Field->getName() << ")"
                           << ";\n";
                }
                else
                {
                    outs() << "  os << \"" << (IsFirst ? "" : ", ")
                           << Field->getName() << ": \" << v."
                           << Field->getName() << ";\n";
                }
            }
            outs() << "  os << \"}\";\n  return os;\n}\n";
        }
    }
};

static cl::OptionCategory GenOstreamCategory("genostream options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

int main(int argc, const char **argv)
{
    CommonOptionsParser OptionsParser(argc, argv, GenOstreamCategory);
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());

    Printer Printer;
    MatchFinder Finder;
    Finder.addMatcher(EnumMatcher, &Printer);
    Finder.addMatcher(RecordMatcher, &Printer);

    outs() << "#include <iostream>\n";

    return Tool.run(newFrontendActionFactory(&Finder).get());
}