#pragma once

#include <string>

#include "ast/ast.hpp"
#include "tooling/source_text.hpp"

namespace a::tooling {

// Shared pretty-printers for type expressions and declaration signatures. The
// formatter, documentation generator, and language server all use these so a
// signature renders the same everywhere.
std::string typeText(const TypeExpr* type);
std::string paramText(const ParamDecl& param);
std::string paramsText(const std::vector<ParamDecl>& params);
std::string typeParamsText(const std::vector<std::string>& typeParams);
std::string signatureText(const FnDecl& decl);

// Recovers the declaration keyword that the AST normalizes away (`var`,
// `const`, `pvt`, `class`, `struct`, `metaclass`). `declaration` starts the
// statement and `name` starts the declared name.
std::string declarationKeyword(const SourceText& source, const Span& declaration,
                               const Span& name);

}  // namespace a::tooling
