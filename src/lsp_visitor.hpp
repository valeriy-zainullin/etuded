#pragma once

#include <vector>
#include <string>
#include <optional>
#include <cstddef>
#include <string_view>

// LibLsp.
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/utils.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

struct SymbolUsage {
  lsRange range;

  struct SymbolDeclDefInfo {
    // Function, type or variable was imported, if it is from another module.
    //   But then it was declared in that module we import it from
    //   Declaration and definition always reside in the same module.

    std::string path;
    lsPosition decl_position;
    lsPosition def_position;

    bool operator==(const SymbolDeclDefInfo& other) const {
      if (lsp::NormalizePath(path, false) != lsp::NormalizePath(other.path, false)) {
        return false;
      }

      if (decl_position != other.decl_position) {
        return false;
      }

      if (def_position != other.def_position) {
        return false;
      }

      return true;
    }
  } declared_at;

  bool is_decl = false;
  bool is_def = false;
};

inline lsPosition LsPositionFromLexLocation(const lex::Location& location) {
  // Не думаю, что кто-то будет открывать файл размером в 4 гигабайта.
  //   IDE с большой вероятностью будет сильно тормозить.
  assert(token.location.lineno   >= 0);
  assert(token.location.columnno >= 1); // Позиция после последнего символа. Храним правую границу полуинтервала [start, end).
  assert(token.location.lineno   <= std::numeric_limits<int>::max());
  assert(token.location.columnno <= std::numeric_limits<int>::max());

  return {static_cast<int>(location.lineno), static_cast<int>(location.columnno)};
}

inline lsRange LsRangeFromLexToken(const lex::Token& token) {
  // Позиция токена -- номер строки и столбца сразу после него.
  //   Все токены однострочные, перевод строки разделяет токены.
  assert(token.location.columnno >= token.length());

  int line = static_cast<int>(token.location.lineno);
  int col  = static_cast<int>(token.location.columnno);

  lsPosition end = LsPositionFromLexLocation(token.location);
  lsPosition start = end;
  start.character -= static_cast<int>(token.length());

  fmt::println(stderr, "TokenToLsRange ({}, {})-({}, {})", start.line, start.character, end.line, end.character);

  // Exclusive like range in editor.
  // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#range
  return lsRange{std::move(start), std::move(end)};
}


class LSPVisitor: public Visitor {
public:
  LSPVisitor(
    const std::string file_path,
    std::vector<lsDocumentSymbol>* symbols,
    std::vector<SymbolUsage>* usages
  )
    : file_path_(file_path)
    , symbols_(symbols)
    , usages_(usages) {
      assert(symbols_ != nullptr);
  }

  virtual ~LSPVisitor() = default;

  LSPVisitor(LSPVisitor&&) = default;
  LSPVisitor(const LSPVisitor&) = delete;

  LSPVisitor& operator=(LSPVisitor&&) = default;
  LSPVisitor& operator=(const LSPVisitor&) = delete;

  static lsRange TokenToLsRange(const lex::Token& token);

  // Statements

  void VisitYield(YieldStatement* node) override;
  void VisitReturn(ReturnStatement* node) override;
  void VisitAssignment(AssignmentStatement* node) override;
  void VisitExprStatement(ExprStatement* node) override;

  // Declarations

  void VisitTypeDecl(TypeDeclStatement* node) override;
  void VisitVarDecl(VarDeclStatement* node) override;
  void VisitFunDecl(FunDeclStatement* node) override;
  void VisitTraitDecl(TraitDeclaration* node) override {}
  void VisitImplDecl(ImplDeclaration* node) override {}

  // Patterns

  void VisitBindingPat(BindingPattern* node) override {}
  void VisitDiscardingPat(DiscardingPattern* node) override {}
  void VisitLiteralPat(LiteralPattern* node) override {}
  void VisitStructPat(StructPattern* node) override {}
  void VisitVariantPat(VariantPattern* node) override {}

  // Expressions

  void VisitComparison(ComparisonExpression* node) override {}
  void VisitBinary(BinaryExpression* node) override;
  void VisitUnary(UnaryExpression* node) override;
  void VisitDeref(DereferenceExpression* node) override {}
  void VisitAddressof(AddressofExpression* node) override {}
  void VisitIf(IfExpression* node) override;
  void VisitMatch(MatchExpression* node) override {}
  void VisitNew(NewExpression* node) override {}
  void VisitBlock(BlockExpression* node) override;
  void VisitFnCall(FnCallExpression* node) override;
  void VisitIntrinsic(IntrinsicCall* node) override {}
  void VisitCompoundInitalizer(CompoundInitializerExpr* node) override {}
  void VisitFieldAccess(FieldAccessExpression* node) override;
  void VisitVarAccess(VarAccessExpression* node) override;
  void VisitLiteral(LiteralExpression* node) override {}
  void VisitTypecast(TypecastExpression* node) override {}

private:
  const std::string file_path_;
  std::vector<lsDocumentSymbol>* symbols_;
  std::vector<SymbolUsage>* usages_;
};
