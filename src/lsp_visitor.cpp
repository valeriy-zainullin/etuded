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
    range: LsRangeFromLexToken(node->name_),
    declared_at: {
      path: file_path_,
      decl_position: LsPositionFromLexLocation(node->name_.location),
      def_position: LsPositionFromLexLocation(node->name_.location),
    }
  });

  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->name_.GetName()),
    kind: lsSymbolKind::TypeAlias,
    range: LsRangeFromLexToken(node->name_),
    selectionRange: LsRangeFromLexToken(node->name_),
  });

  // TODO: also check what type variant it really is and
  //   store it's symbol declarations in the symbol vector
  //   (not in the symboltable).
}


void LSPVisitor::VisitVarDecl(VarDeclStatement* node) {
  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->lvalue_->GetName()),
    kind: lsSymbolKind::Variable,
    range: LsRangeFromLexToken(node->lvalue_->name_),
    selectionRange: LsRangeFromLexToken(node->lvalue_->name_),
  });

  usages_->push_back(SymbolUsage{
    range: LsRangeFromLexToken(node->lvalue_->name_),
    declared_at: {
      path: file_path_,
      decl_position: LsPositionFromLexLocation(node->lvalue_->name_.location),
      def_position: LsPositionFromLexLocation(node->lvalue_->name_.location),
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
      range: LsRangeFromLexToken(node->name_),
      declared_at: {
        path: file_path_,
        decl_position: LsPositionFromLexLocation(node->name_.location),
        def_position: LsPositionFromLexLocation(node->name_.location),
      }
    });
  }

  if (node->body_) {
    // TODO: store fun token inside of fun decl, include it into the symbol.
    symbols_->push_back(lsDocumentSymbol{
      name: std::string(node->name_.GetName()),
      kind: lsSymbolKind::Variable,
      range: lsRange(LsRangeFromLexToken(node->name_).start, LsPositionFromLexLocation(node->body_->GetLocation())),
      selectionRange: LsRangeFromLexToken(node->name_),
    });

    // auto symbol = current_context_->RetrieveSymbol(node->GetName());
    // symbol->as_fn_sym.def = node;

    // node->layer_ = current_context_;

    // Bring parameters into the scope (their very special one)

    for (auto& param : node->formals_) {
      symbols_->push_back(lsDocumentSymbol{
        name: std::string(param.GetName()),
        kind: lsSymbolKind::Variable,
        range: LsRangeFromLexToken(param),
        selectionRange: LsRangeFromLexToken(param),
      });

      int line = static_cast<int>(param.location.lineno);
      int col = static_cast<int>(param.location.columnno);
      usages_->push_back(SymbolUsage{
        range: LsRangeFromLexToken(param),
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
      range: LsRangeFromLexToken(node->name_),
      selectionRange: LsRangeFromLexToken(node->name_),
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
  fmt::println(stderr, "FnCall(.fn_name_ = {}, .callable = {})", node->fn_name_, reinterpret_cast<void*>(node->callable_));
}

void LSPVisitor::VisitFieldAccess(FieldAccessExpression* node) {
  node->struct_expression_->Accept(this);

  types::Type* struct_type = node->struct_expression_->GetType();

  for (const types::Member& member: struct_type->as_struct.first) {
    if (member.field == node->field_name_) {
      usages_->push_back(SymbolUsage{
        range: LsRangeFromLexToken(member.name),
        declared_at: {
          path: symbol->declared_at.unit.GetPath(),
          decl_position: LsPositionFromLexLocation(symbol->declared_at.position),
          def_position: LsPositionFromLexLocation(symbol->declared_at.position),
        }
      });

      break;
    }
  }

  node->field_name_
}

void LSPVisitor::VisitVarAccess(VarAccessExpression* node) {
  assert(node->layer_ != nullptr && "context builder is expected to have finished it's job");
  ast::scope::Symbol* symbol = node->layer_->FindDeclForUsage(
    node->GetName(),
    node->name_.location
  );
  if (symbol != nullptr) {
    usages_->push_back(SymbolUsage{
      range: LsRangeFromLexToken(node->name_),
      declared_at: {
        path: symbol->declared_at.unit.GetPath(),
        decl_position: LsPositionFromLexLocation(symbol->declared_at.position),
        def_position: LsPositionFromLexLocation(symbol->declared_at.position),
      }
    });
  }

  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->name_.GetName()),
    kind: lsSymbolKind::Variable,
    range: LsRangeFromLexToken(node->name_),
    selectionRange: LsRangeFromLexToken(node->name_),
  });
}

