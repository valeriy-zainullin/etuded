#include <atomic>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <condition_variable>
#include <memory>

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
#include "LibLsp/lsp/textDocument/documentColor.h"
#include "LibLsp/lsp/lsTextDocumentIdentifier.h" // Missing include inside of did_close.h, okay...
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/did_close.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

#include "logger.hpp"

namespace colors {
  using Color = TextDocument::Color;

  // Должны быть похожи на цвета какого-нибудь другого языка,
  //   для простоты. Чтобы расцветка была уже знакома пользователю.
  Color func_name = {1, 0, 0};
}

class ColoringVisitor: public Visitor {
public:
  ColoringVisitor(std::vector<ColorInformation>& colorings)
    : colorings_(colorings) {}

  // Statements

  void VisitYield(YieldStatement* node) override = 0;

  void VisitReturn(ReturnStatement* node) override = 0;

  void VisitAssignment(AssignmentStatement* node) override = 0;

  void VisitExprStatement(ExprStatement* node) override = 0;

  // Declarations

  void VisitTypeDecl(TypeDeclStatement* node) override = 0;

  void VisitVarDecl(VarDeclStatement* node) override = 0;

  void VisitFunDecl(FunDeclStatement* node) override {
    colorings_.emplace_back(Col);
  }

  void VisitTraitDecl(TraitDeclaration* node) override = 0;

  void VisitImplDecl(ImplDeclaration* node) override = 0;

  // Patterns

  void VisitBindingPat(BindingPattern* node) override = 0;

  void VisitDiscardingPat(DiscardingPattern* node) override = 0;

  void VisitLiteralPat(LiteralPattern* node) override = 0;

  void VisitStructPat(StructPattern* node) override = 0;

  void VisitVariantPat(VariantPattern* node) override = 0;

  // Expressions

  void VisitComparison(ComparisonExpression* node) override = 0;

  void VisitBinary(BinaryExpression* node) override = 0;

  void VisitUnary(UnaryExpression* node) override = 0;

  void VisitDeref(DereferenceExpression* node) override = 0;

  void VisitAddressof(AddressofExpression* node) override = 0;

  void VisitIf(IfExpression* node) override = 0;

  void VisitMatch(MatchExpression* node) override = 0;

  void VisitNew(NewExpression* node) override = 0;

  void VisitBlock(BlockExpression* node) override = 0;

  void VisitFnCall(FnCallExpression* node) override = 0;

  void VisitIntrinsic(IntrinsicCall* node) override = 0;

  void VisitCompoundInitalizer(CompoundInitializerExpr* node) override = 0;

  void VisitFieldAccess(FieldAccessExpression* node) override = 0;

  void VisitVarAccess(VarAccessExpression* node) override = 0;

  void VisitLiteral(LiteralExpression* node) override = 0;

  void VisitTypecast(TypecastExpression* node) override = 0;

private:
  std::vector<ColorInformation>& colorings_;
};

namespace fs = std::filesystem;
class ViewedFile {
public:
  ViewedFile(std::string doc_path)
    : path(doc_path) {
      assert(path.is_absolute());

      Invalidate();
  }

  void Invalidate() {
      // Компилятор на данный момент ищет файлы в рабочей директории.
      //   В том числе, все импортируемые. Кроме стандартной библиотеки,
      //   которую он найдет и так, если мы укажем переменную окружения.
      //   Потому сменим рабочую директорию. Другие части нашего кода от
      //   этого не зависят.

      // https://stackoverflow.com/a/57096619
      fs::current_path(path.parent_path());

      CompilationDriver driver(GetModuleName());
      driver.PrepareForTooling();

      coloring.clear();
      driver.RunVisitor(ColoringVisitor(&coloring));
  }
private:
  std::string GetModuleName() {
      // Module.et -> Module
      return path.filename().replace_extension();
  }
public:
  fs::path path;
  std::vector<ColorInformation> coloring;
};

std::map<std::string, ViewedFile> doc_path_to_file;

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
        .colorProvider = {{true, {}}}
    };

    return response;
  });

  std::map<std::string, Module> ast_cache;

  client_endpoint.registerHandler([&](const td_documentColor::request& request) {
    std::string doc_path = request.params.textDocument.uri.GetAbsolutePath().path;
    std::vector<ColorInformation> coloring;

    decltype(ast_cache)::iterator ast_it = ast_cache.find(doc_path);
    if (ast_it != ast_cache.end()) {
        Module& module = ast_it->second;
        // module.
    } else {
    }

    td_documentColor::response response;
    response.id = request.id;
    response.result = std::move(coloring);
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