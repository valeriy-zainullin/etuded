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

lsRange LSPVisitor::TokenToLsRange(const lex::Token& token) {
  // Не думаю, что кто-то будет открывать файл размером в 4 гигабайта.
  //   IDE с большой вероятностью будет сильно тормозить.
  assert(token.location.lineno   >= 0);
  assert(token.location.columnno >= 1); // Позиция после последнего символа. Храним правую границу полуинтервала [start, end).
  assert(token.location.lineno   <= std::numeric_limits<int>::max());
  assert(token.location.columnno <= std::numeric_limits<int>::max());

  // Позиция токена -- номер строки и столбца сразу после него.
  //   Все токены однострочные, перевод строки разделяет токены.
  assert(token.location.columnno >= token.length());

  int line = static_cast<int>(token.location.lineno);
  int col  = static_cast<int>(token.location.columnno);

  lsPosition end(line, col);
  lsPosition start(end.line, col - static_cast<int>(token.length()));

  fmt::println(stderr, "TokenToLsRange ({}, {})-({}, {})", start.line, start.character, end.line, end.character);

  // Exclusive like range in editor.
  // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#range
  return lsRange{std::move(start), std::move(end)};
}

// Statements

void LSPVisitor::VisitReturn(ReturnStatement* node) {
  node->return_value_->Accept(this);
}

void LSPVisitor::VisitAssignment(AssignmentStatement* node) {
  node->target_->Accept(this);
  node->value_->Accept(this);

  symbols_->push_back(lsDocumentSymbol{
    name: "assign",
    kind: lsSymbolKind::Operator,
    range: TokenToLsRange(node->assign_),
  });
}

void LSPVisitor::VisitExprStatement(ExprStatement* node) {
  node->expr_->Accept(this);
}

// Declarations

void LSPVisitor::VisitVarDecl(VarDeclStatement* node) {
  symbols_->push_back(lsDocumentSymbol{
    kind: lsSymbolKind::Variable,
    range: TokenToLsRange(node->lvalue_->name_),
  });

  int line = static_cast<int>(node->lvalue_->name_.location.lineno);
  int col = static_cast<int>(node->lvalue_->name_.location.columnno);
  usages_->push_back(Usage{
    range: TokenToLsRange(node->lvalue_->name_),
    declared_at: {
      path: file_path_,
      position: lsPosition(line, col),
    }
  });
}


// Patterns

// Expressions
