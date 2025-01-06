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

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

struct SymbolUsage {
  lsRange range;

  struct {
    // Function, type or variable was imported, if it is from another module.
    //   But then it was declared in that module we import it from
    //   Declaration and definition always reside in the same module.

    std::string path;
    lsPosition decl_position;
    lsPosition def_position;
  } declared_at;

  bool is_decl = false;
  bool is_def = false;
};


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
  void VisitFieldAccess(FieldAccessExpression* node) override {}
  void VisitVarAccess(VarAccessExpression* node) override;
  void VisitLiteral(LiteralExpression* node) override {}
  void VisitTypecast(TypecastExpression* node) override {}

private:
  const std::string file_path_;
  std::vector<lsDocumentSymbol>* symbols_;
  std::vector<SymbolUsage>* usages_;
};
