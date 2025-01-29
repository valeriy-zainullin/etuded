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
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitAssignment called.");
  #endif

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

  // TODO: also store symbols for type usages, so that it's
  //   possible to jump to their definitinos.
}


void LSPVisitor::VisitVarDecl(VarDeclStatement* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitVarDecl called.");
  #endif

  node->value_->Accept(this);

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

    for (auto& param : node->formals_) {
      symbols_->push_back(lsDocumentSymbol{
        name: std::string(param.GetName()),
        kind: lsSymbolKind::Variable,
        range: LsRangeFromLexToken(param),
        selectionRange: LsRangeFromLexToken(param),
      });

      usages_->push_back(SymbolUsage{
        range: LsRangeFromLexToken(param),
        declared_at: {
          path: file_path_,
          decl_position: LsPositionFromLexLocation(param.location),
          def_position: LsPositionFromLexLocation(param.location),
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

// Должно быть понятно тем, кто знаком с функциональными языками. Etude тоже
//   ими вдохновлялся.

// Maybe является суммой типов .some A и .none.
//   Т.е. у нас могли бы быть тип .some, который хранит в себе один объект.
//   И тип .none, который ничего не хранит, просто std::monostate.
// У нас просто std::variant<A, std::monostate>. Да, можно переводить это в
//   std::optional (Maybe) и обратно.
// В match maybe: .some x ... .some x будет variant pattern, а внутри его binding pattern, 
//   который создает переменную x, распаковав из maybe. Каждый случай match создает область видимости
//   (достаточно посмотреть ContextBuilder::VisitMatch), можем в нее смотреть.
// Discarding pattern - это `| _: ...`, т.е. не важно, какое было значение. Достаточно было посмотреть
//   в parse_pat.cpp, чтобы это выяснить.
// Literal pattern, судя по parse_pat.cpp, это сравнение с какой-то константой.
//   Проверил с помощью файла span.et из стандартной библиотеки Etude. Там вызывается VisitLiteralPat.
//   Что может содержать LiteralExpression? Перейдем к определению в том же файле parse_pat.cpp.
//   Попадем в espressions.hpp. Увидим, что это просто токен. Если не брать во внимание std::monostate
//   в токене, то это просто непосредственные число или строка. И то std::monostate указан первым в
//   данных, которые хранит токен (SemInfo) только потому, что хотели, видимо, чтобы Token был
//   default constructible с разумным значением (надо проверить, всегда ли std::monostate не
//   используется в токене). У нас есть конструкция Token() = default, потому я так решил.
// Но в целом в исходниках программ литералами называются непосредственные числа, строки (строковые
//   литералы) или символьные константы.

// Structure pattern, видимо, какой-то нереализованый функционал. В parse_pat.cpp его нет. А
//   поиск ссылок по всему проекту не выдает ничего, кроме этого файла и объявления. Т.е. у класса
//   даже нет определения, потому по факту в AST его не будет.

void LSPVisitor::VisitBindingPat(BindingPattern* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitBindingPat called.");
  #endif

  // Все то же самое, как в variable decl. Ситуация похожая:
  //   добавился символ и все.
  
  symbols_->push_back(lsDocumentSymbol{
    name: std::string(node->name_.GetName()),
    kind: lsSymbolKind::Variable,
    range: LsRangeFromLexToken(node->name_),
    selectionRange: LsRangeFromLexToken(node->name_),
  });

  usages_->push_back(SymbolUsage{
    range: LsRangeFromLexToken(node->name_),
    declared_at: {
      path: file_path_,
      decl_position: LsPositionFromLexLocation(node->name_.location),
      def_position: LsPositionFromLexLocation(node->name_.location),
    }
  });
}

void LSPVisitor::VisitDiscardingPat(DiscardingPattern* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitDiscardingPat called.");
  #endif

  // Ничего делать не надо, символов не добавилось.
}

void LSPVisitor::VisitLiteralPat(LiteralPattern* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitLiteralPat called.");
  #endif

  // Ничего делать не надо, литерал не добавляет символы.
}

void LSPVisitor::VisitStructPat(StructPattern* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitStructPat called.");
  #endif
}

void LSPVisitor::VisitVariantPat(VariantPattern* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitVariantPat called.");
  #endif

  // Необходимо зайти внутрь, там может быть еще variant pattern или binding pattern.
  //   Возможно, какие-то еще образцы. Если есть, конечно.
  //   Может быть лишь `| .none:`, пример
  if (node->inner_pat_ != nullptr) {
    node->inner_pat_->Accept(this);
  }

  // Хорошо бы найти определение этого элемента суммы типов.
  //   Можно сделать через типы.

  types::Type* type = TypeStorage(node->GetType());

  // TODO.
  //fmt::println("");

  // Все как у обращения к полю структуры.
  for (auto& member: type->as_sum.first) {
    if (member.field == node->name_.GetName()) {
      usages_->push_back(SymbolUsage{
        range: LsRangeFromLexToken(node->name_),
        declared_at: {
          path: member.name.location.unit->GetAbsPath(),
          decl_position: LsPositionFromLexLocation(member.name.location),
          def_position: LsPositionFromLexLocation(member.name.location),
        }
      });
    }
  }

}


// Expressions

void LSPVisitor::VisitBinary(BinaryExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitBinary called.");
  #endif

  node->left_->Accept(this);
  node->right_->Accept(this);
}

void LSPVisitor::VisitUnary(UnaryExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitUnary called.");
  #endif

  node->operand_->Accept(this);
}

void LSPVisitor::VisitDeref(DereferenceExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitDeref called.");
  #endif

  node->operand_->Accept(this);
}

void LSPVisitor::VisitAddressof(AddressofExpression* node) {
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

void LSPVisitor::VisitMatch(MatchExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitMatch called.");
  #endif

  node->against_->Accept(this);

  for (auto& [pat, expr]: node->patterns_) {
    pat->Accept(this);
    expr->Accept(this);
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
  node->callable_->Accept(this);

  for (auto& arg: node->arguments_) {
    arg->Accept(this);
  }

  fmt::println(stderr, "FnCall(.fn_name_ = {}, .callable = {})", node->fn_name_, reinterpret_cast<void*>(node->callable_));
}

void LSPVisitor::VisitCompoundInitalizer(CompoundInitializerExpr* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitCompoundInitalizer called.");
  #endif

  types::Type* type = TypeStorage(node->GetType());

  std::vector<types::Member>* members = nullptr;
  if (type->tag == types::TypeTag::TY_STRUCT) {
    members = &type->as_struct.first;
  } else if (type->tag == types::TypeTag::TY_SUM) {
    members = &type->as_sum.first;
  } else {
    #if TRACE_VISITOR
      fmt::println(stderr, "DEBUG: LSPVisitor::VisitCompoundInitalizer haven't found members to compound initialize..");
    #endif
  }

  for (CompoundInitializerExpr::Member& initializer: node->initializers_) {
    if (members != nullptr) {      
      for (types::Member& member: *members) {
        if (member.field == initializer.field) {
          usages_->push_back(SymbolUsage{
            range: LsRangeFromLexToken(initializer.name),
            declared_at: {
              path: member.name.location.unit->GetAbsPath(),
              decl_position: LsPositionFromLexLocation(member.name.location),
              def_position: LsPositionFromLexLocation(member.name.location),
            }
          });

          break;
        }
      }
    }

    // May be missing. See parse_expr.cpp, ParseSignleFieldCompound function.
    if (initializer.init != nullptr) {
      initializer.init->Accept(this);
    }
  }

}


void LSPVisitor::VisitFieldAccess(FieldAccessExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitFieldAccess called.");
  #endif

  node->struct_expression_->Accept(this);

  types::Type* struct_type = TypeStorage(node->struct_expression_->GetType());

  for (const types::Member& member: struct_type->as_struct.first) {
    // fmt::println(stderr, "member name={}, expected {}", member.field, node->field_name_.GetName());
    if (member.field == node->field_name_.GetName()) {
      usages_->push_back(SymbolUsage{
        range: LsRangeFromLexToken(node->field_name_),
        declared_at: {
          path: member.name.location.unit->GetAbsPath(),
          decl_position: LsPositionFromLexLocation(member.name.location),
          def_position: LsPositionFromLexLocation(member.name.location),
        }
      });

      break;
    }
  }
}

void LSPVisitor::VisitVarAccess(VarAccessExpression* node) {
  #if TRACE_VISITOR
    fmt::println(stderr, "TRACE: LSPVisitor::VisitVarAccess called.");
  #endif

  assert(node->layer_ != nullptr && "context builder is expected to have finished it's job");
  ast::scope::Symbol* symbol = node->layer_->FindDeclForUsage(
    node->GetName(),
    node->name_.location
  );
  if (symbol != nullptr) {
    usages_->push_back(SymbolUsage{
      range: LsRangeFromLexToken(node->name_),
      declared_at: {
        path: symbol->declared_at.unit.GetAbsPath(),
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
