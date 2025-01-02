#include <atomic>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <variant>

// LibLsp.
#include "LibLsp/lsp/AbsolutePath.h"
#include "LibLsp/lsp/ProtocolJsonHandler.h"
#include "LibLsp/JsonRpc/stream.h"
#include "LibLsp/JsonRpc/RemoteEndPoint.h"
#include "LibLsp/JsonRpc/Endpoint.h"
#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/lsTextDocumentIdentifier.h" // Missing include inside of did_close.h, okay...
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/did_close.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

#include "logger.hpp"

class LSPVisitor: public Visitor {
public:
  LSPVisitor(const lsDocumentUri& file_uri, std::vector<lsDocumentSymbol>* symbols)
    : file_uri(file_uri)
    , symbols_(symbols) {
      assert(symbols_ != nullptr);
  }

  LSPVisitor(LSPVisitor&&) = default;
  LSPVisitor(const LSPVisitor&) = delete;

  LSPVisitor& operator=(LSPVisitor&&) = default;
  LSPVisitor& operator=(const LSPVisitor&) = delete;

  static lsRange TokenToLsRange(const lex::Token& token) {
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

  void VisitYield(YieldStatement* node) override {}

  void VisitReturn(ReturnStatement* node) override {
    symbols_->push_back(lsDocumentSymbol{
      name: "return",
      kind: SymbolKind::Operator,
      range: TokenToLsRange(node->return_token_),
    });

    node->return_value_->Accept(this);
  }

  void VisitAssignment(AssignmentStatement* node) override {
    node->target_->Accept(this);
    node->value_->Accept(this);

    symbols_->push_back(lsDocumentSymbol{
      name: "assign",
      kind: lsSymbolKind::Operator,
      range: TokenToLsRange(node->assign_),
    });
  }

  void VisitExprStatement(ExprStatement* node) override {
    node->expr_->Accept(this);
  }

  // Declarations

  void VisitTypeDecl(TypeDeclStatement* node) override {
    // TODO.
  }

  void VisitVarDecl(VarDeclStatement* node) override {
    // TODO.
  }

  void VisitFunDecl(FunDeclStatement* node) override {
    // TODO: store fun token in AST for editor
    //   integration purposes (coloring). 
    // symbols_->push_back(lsSymbol{
    //   name: "fun",
    //   kind: SymbolKind::Operator,
    //   location: lsLocation{
    //     file_uri,
    //     TokenToLsRange(node->)
    //   }
    // });

    symbols_->push_back(lsDocumentSymbol{
      name: "function name",
      kind: lsSymbolKind::Function,
      range: TokenToLsRange(node->name_),
      selectionRange: TokenToLsRange(node->name_),
    });

    for (const lex::Token& formal: node->formals_) {
      symbols_->push_back(lsDocumentSymbol{
        name: "function parameter",
        kind: lsSymbolKind::Parameter,
        range: TokenToLsRange(formal),
        selectionRange: TokenToLsRange(formal),
      });
    }
  }

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

  void VisitBinary(BinaryExpression* node) override {}

  void VisitUnary(UnaryExpression* node) override {}

  void VisitDeref(DereferenceExpression* node) override {}

  void VisitAddressof(AddressofExpression* node) override {}

  void VisitIf(IfExpression* node) override {}

  void VisitMatch(MatchExpression* node) override {}

  void VisitNew(NewExpression* node) override {}

  void VisitBlock(BlockExpression* node) override {}

  void VisitFnCall(FnCallExpression* node) override {}

  void VisitIntrinsic(IntrinsicCall* node) override {}

  void VisitCompoundInitalizer(CompoundInitializerExpr* node) override {}

  void VisitFieldAccess(FieldAccessExpression* node) override {}

  void VisitVarAccess(VarAccessExpression* node) override {}

  void VisitLiteral(LiteralExpression* node) override {
    fmt::println(stderr, "literal visited!");
    symbols_->push_back(lsDocumentSymbol{
      name: "literal",
      kind: std::visit(
        [](auto& value) -> lsSymbolKind {
          if constexpr (std::is_same_v<decltype(value), std::string_view&>) {
            return lsSymbolKind::String;
          } else if constexpr (std::is_same_v<decltype(value), int&>) {
            return lsSymbolKind::Number;
          } else {
            return lsSymbolKind::Unknown;
          }
        },
        node->token_.sem_info
      ),
      range: TokenToLsRange(node->token_),
    });
  };

  void VisitTypecast(TypecastExpression* node) override {}

private:
  const lsDocumentUri& file_uri;
  std::vector<lsDocumentSymbol>* symbols_;
};

namespace fs = std::filesystem;
class ViewedFile {
public:
  ViewedFile(lsDocumentUri uri)
    : uri_(std::move(uri)), path_(uri_.GetAbsolutePath().path) {
      assert(path_.is_absolute());

      Invalidate();
  }

  void Invalidate() {
      // Компилятор на данный момент ищет файлы в рабочей директории.
      //   В том числе, все импортируемые. Кроме стандартной библиотеки,
      //   которую он найдет и так, если мы укажем переменную окружения.
      //   Потому сменим рабочую директорию. Другие части нашего кода от
      //   этого не зависят.

      // https://stackoverflow.com/a/57096619
      fs::current_path(path_.parent_path());

      std::string module_name = GetModuleName();
      // Важно, чтобы module_name существовал все время выполнения
      //   этой функции, потому что compilation driver
      //   принимает эту строку как std::string_view.
      CompilationDriver driver(module_name);
      driver.PrepareForTooling();

      symbols.clear();

      LSPVisitor visitor(uri_, &symbols);
      driver.RunVisitor(&visitor);
  }
private:
  std::string GetModuleName() {
      // Module.et -> Module
      return path_.filename().replace_extension();
  }
public:
  lsDocumentUri uri_;
  fs::path path_;

  std::vector<lsDocumentSymbol> symbols;
};

std::unordered_map<std::string, ViewedFile> file_cache;

int main(int argc, char** argv) {
  if (argc < 1 || argv[0] == nullptr) {
    std::cerr << "Invalid usage, missing executable path in argv.";
    return -1;
  }

  fs::path exec_path = fs::absolute(fs::path(argv[0]));
  fs::path exec_dir  = exec_path.parent_path();
  fs::path compiler_path  = exec_dir / "etude" / "123";
  // TODO: figure out compiler path!

  std::atomic<bool> initialized = false;
  std::atomic<bool> exiting = false;

  Logger logger;

  auto server_endpoint = std::make_shared<GenericEndpoint>(logger);
  // TODO: handlers.

  auto json_handler = std::make_shared<lsp::ProtocolJsonHandler>();
  RemoteEndPoint client_endpoint(json_handler, server_endpoint, logger);

  // https://github.com/kuafuwang/LspCpp/blob/e0b443d42e7d23638d727ac8ef6839b9e527bf0a/examples/StdIOServerExample.cpp#L57
  client_endpoint.registerHandler([&](const td_initialize::request& request) {
    td_initialize::response response;
    
    response.id = request.id;
    response.result.capabilities = lsServerCapabilities {
        .documentSymbolProvider = {{true, {}}}
    };

    return response;
  });

  auto find_file = [&](const lsDocumentUri& uri) -> ViewedFile& {
    auto file_it = file_cache.find(uri.raw_uri_);
    if (file_it == file_cache.end()) {
      // Здесь произойдет разбор файла с путем doc_path.
      //   Внутри конструктора будет вызов Invalidate(), он
      //   разбирает файл и собарет информацию, которую
      //   отображает редактор.
      // TODO: в дальнейшем может понадобиться комплиировать
      //   буффер текста, а не файл с диска. Если он еще не был
      //   сохранен в редакторе.
      // NOTE: пока не понятно, как давать подсказки по дополнению.
      //   Нужно разобраться в алгоритме, как это работает в других
      //   случаях. Т.к. если код не дописан, будут ошибки со стороны
      //   парсера.
      auto file = ViewedFile(uri);

      auto result = file_cache.insert({uri.raw_uri_, std::move(file)});
      assert(result.second);
      file_it = std::move(result.first);
    }

    return file_it->second;
  };

  client_endpoint.registerHandler([&](const td_symbol::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_symbol::response response;
    response.id = request.id;
    response.result = file.symbols;
    return response;
  });

  client_endpoint.registerHandler([&](Notify_InitializedNotification::notify& notify) {
    initialized.store(true);
  });

  client_endpoint.registerHandler([&](Notify_Exit::notify& notify) {
    client_endpoint.stop();
    exiting.store(true);
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidOpen::notify& notify) {
    if (!initialized) {
        return;
    }
    // Т.е. перед нами может быть даже не локальный файл, а на удаленном компьютере!
    logger.log(lsp::Log::Level::INFO, "opened file with uri " + notify.params.textDocument.uri.raw_uri_);
  });

  auto input  = std::static_pointer_cast<lsp::istream>(std::make_shared<istream<decltype(std::cin)>>(std::cin));
  auto output = std::static_pointer_cast<lsp::ostream>(std::make_shared<ostream<decltype(std::cout)>>(std::cout));
  client_endpoint.startProcessingMessages(input, output);

  // cppreference: "These functions are guaranteed to return only if
  //   value has changed, even if underlying implementation unblocks
  //   spuriously."
  // https://en.cppreference.com/w/cpp/atomic/atomic/wait
  exiting.wait(false);

  return 0;
}