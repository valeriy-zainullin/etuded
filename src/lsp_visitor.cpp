#include "lsp_visitor.hpp"

#include <cassert>

#include "LibLsp/lsp/lsRange.h"

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

// Patterns

// Expressions
