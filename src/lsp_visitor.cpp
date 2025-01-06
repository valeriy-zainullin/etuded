#include "lsp_visitor.hpp"

#include <cassert>
#include <cstddef>

// LibLsp.
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/lsPosition.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

static lsPosition PositionFromLexLocation(const lex::Location& location) {
  // Не думаю, что кто-то будет открывать файл размером в 4 гигабайта.
  //   IDE с большой вероятностью будет сильно тормозить.
  assert(token.location.lineno   >= 0);
  assert(token.location.columnno >= 1); // Позиция после последнего символа. Храним правую границу полуинтервала [start, end).
  assert(token.location.lineno   <= std::numeric_limits<int>::max());
  assert(token.location.columnno <= std::numeric_limits<int>::max());

  return {static_cast<int>(location.lineno), static_cast<int>(location.columnno)};
}


lsRange LSPVisitor::TokenToLsRange(const lex::Token& token) {
  // Позиция токена -- номер строки и столбца сразу после него.
  //   Все токены однострочные, перевод строки разделяет токены.
  assert(token.location.columnno >= token.length());

  int line = static_cast<int>(token.location.lineno);
  int col  = static_cast<int>(token.location.columnno);

  lsPosition end = PositionFromLexLocation(token.location);
  lsPosition start = end;
  start.character -= static_cast<int>(token.length());

  fmt::println(stderr, "TokenToLsRange ({}, {})-({}, {})", start.line, start.character, end.line, end.character);

  // Exclusive like range in editor.
  // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#range
  return lsRange{std::move(start), std::move(end)};
}

// Statements

void LSPVisitor::VisitYield(YieldStatement* node) {
  node->yield_value_->Accept(this);
}

void LSPVisitor::VisitReturn(ReturnStatement* node) {
  node->return_value_->Accept(this);
}

void LSPVisitor::VisitAssignment(AssignmentStatement* node) {
  node->target_->Accept(this);
  node->value_->Accept(this);
}

void LSPVisitor::VisitExprStatement(ExprStatement* node) {
  node->expr_->Accept(this);
}

// Declarations

void LSPVisitor::VisitTypeDecl(TypeDeclStatement* node) {
  usages_->push_back(SymbolUsage{
    range: TokenToLsRange(node->name_),
    declared_at: {
      path: file_path_,
      decl_position: PositionFromLexLocation(node->name_.location),
      def_position: PositionFromLexLocation(node->name_.location),
    }
  });

  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->name_.GetName()),
    kind: lsSymbolKind::TypeAlias,
    range: TokenToLsRange(node->name_),
    selectionRange: TokenToLsRange(node->name_),
  });
}


void LSPVisitor::VisitVarDecl(VarDeclStatement* node) {
  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->lvalue_->GetName()),
    kind: lsSymbolKind::Variable,
    range: TokenToLsRange(node->lvalue_->name_),
    selectionRange: TokenToLsRange(node->lvalue_->name_),
  });

  usages_->push_back(SymbolUsage{
    range: TokenToLsRange(node->lvalue_->name_),
    declared_at: {
      path: file_path_,
      decl_position: PositionFromLexLocation(node->lvalue_->name_.location),
      def_position: PositionFromLexLocation(node->lvalue_->name_.location),
    }
  });
}

void LSPVisitor::VisitFunDecl(FunDeclStatement* node) {
  auto fun_ty = types::HintedOrNew(node->type_);

  // Handle cases where parts of the signature are known
  // e.g. Vec(_) -> Maybe(_)

  if (!node->trait_method_) {
    // TODO: mark as definition if it is in fact definition, not only declaration.
    usages_->push_back(SymbolUsage{
      range: TokenToLsRange(node->name_),
      declared_at: {
        path: file_path_,
        decl_position: PositionFromLexLocation(node->name_.location),
        def_position: PositionFromLexLocation(node->name_.location),
      }
    });
  }

  if (node->body_) {
    // TODO: store fun token inside of fun decl, include it into the symbol.
    symbols_->push_back(lsDocumentSymbol{
      name: std::string(node->name_.GetName()),
      kind: lsSymbolKind::Variable,
      range: lsRange(TokenToLsRange(node->name_).start, PositionFromLexLocation(node->body_->GetLocation())),
      selectionRange: TokenToLsRange(node->name_),
    });

    // auto symbol = current_context_->RetrieveSymbol(node->GetName());
    // symbol->as_fn_sym.def = node;

    // node->layer_ = current_context_;

    // Bring parameters into the scope (their very special one)

    for (auto& param : node->formals_) {
      symbols_->push_back(lsDocumentSymbol{
        name: std::string(param.GetName()),
        kind: lsSymbolKind::Variable,
        range: TokenToLsRange(param),
        selectionRange: TokenToLsRange(param),
      });

      int line = static_cast<int>(param.location.lineno);
      int col = static_cast<int>(param.location.columnno);
      usages_->push_back(SymbolUsage{
        range: TokenToLsRange(param),
        declared_at: {
          path: file_path_,
          decl_position: lsPosition(line, col),
          def_position: lsPosition(line, col)
        }
      });
    }

    node->body_->Accept(this);
  } else {
      // TODO: store fun token inside of fun decl, include it into the symbol.
    symbols_->push_back(lsDocumentSymbol{
      name: std::string(node->name_.GetName()),
      kind: lsSymbolKind::Variable,
      range: TokenToLsRange(node->name_),
      selectionRange: TokenToLsRange(node->name_),
    });
  }
}


// Patterns

// Expressions

void LSPVisitor::VisitBinary(BinaryExpression* node) {
  node->left_->Accept(this);
  node->right_->Accept(this);
}

void LSPVisitor::VisitUnary(UnaryExpression* node) {
  node->operand_->Accept(this);
}


void LSPVisitor::VisitIf(IfExpression* node) {
  assert(node->true_branch_ != nullptr);

  node->condition_->Accept(this);
  node->true_branch_->Accept(this);

  if (node->false_branch_ != nullptr) {
    node->false_branch_->Accept(this);
  }
}


void LSPVisitor::VisitBlock(BlockExpression* node) {
  for (auto stmt : node->stmts_) {
    stmt->Accept(this);
  }

  if (node->final_) {
    node->final_->Accept(this);
  }
}

void LSPVisitor::VisitFnCall(FnCallExpression* node) {
  fmt::println("FnCall(.fn_name_ = {}, .callable = {})", node->fn_name_, reinterpret_cast<void*>(node->callable_));
}


void LSPVisitor::VisitVarAccess(VarAccessExpression* node) {
  assert(node->layer_ != nullptr && "context builder is expected to have finished it's job");
  ast::scope::Symbol* symbol = node->layer_->FindDeclForUsage(
    node->GetName(),
    node->name_.location
  );
  if (symbol != nullptr) {
    usages_->push_back(SymbolUsage{
      range: TokenToLsRange(node->name_),
      declared_at: {
        path: symbol->declared_at.unit.GetPath(),
        decl_position: PositionFromLexLocation(symbol->declared_at.position),
        def_position: PositionFromLexLocation(symbol->declared_at.position),
      }
    });
  }

  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->name_.GetName()),
    kind: lsSymbolKind::Variable,
    range: TokenToLsRange(node->name_),
    selectionRange: TokenToLsRange(node->name_),
  });
}

