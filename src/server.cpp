#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <variant>
#include <sstream>
#include <unordered_map>

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
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/lsTextDocumentIdentifier.h" // Missing include inside of did_close.h, okay...
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/did_close.h"
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_save.h"
#include "LibLsp/lsp/textDocument/highlight.h"
#include "LibLsp/lsp/textDocument/prepareRename.h"
#include "LibLsp/lsp/textDocument/rename.h"
#include "LibLsp/lsp/textDocument/hover.h"
#include "LibLsp/lsp/utils.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

#include "logger.hpp"
#include "lsp_visitor.hpp"

namespace fs = std::filesystem;

#define TRACE_CONTENT_HOLDER 1
#define TRACE_INVALIDATION 1

struct EditedFile {
  std::string content;
  std::vector<size_t> line_starts;

  // line is [line_starts[i], line_starts[i + 1] or file size, if eof is the boundary) bytes. That means
  //   it includes line feed, bacause lines are adjacent, without gaps. Lines end with '\n', except the
  //   last one (which could end with '\n', but not necessarily).
  //   We could consider lines as text separated by '\n', but then we'd either store ends
  //   as well or compute ends by subtracting one from the next line start, which seems
  //   hacky.
  // The last line does not necessarily end with a '\n'. If so, end-of-file ends active
  //   line, without producing any more of them. Tbis is handled by the fact that
  //   we add new lines upon '\n', but only if a new line will actually start. So
  //   if the last line ends with '\n', we won't start a new line as a special case.

  void set_content(std::string new_content) {
    content = std::move(new_content);
    line_starts.clear();
    line_starts.push_back(0); // Первая строка начинается с первого байта.
    find_line_starts(0);

    #if TRACE_CONTENT_HOLDER
      fmt::println(stderr, "content.size() = {}", content.size());
      fmt::print(stderr, "line_starts = [");
      for (const auto& item: line_starts) {
        fmt::print(stderr, "{},", item);
      }
      fmt::println(stderr, "]");
    #endif
  }

  static size_t length(size_t start, size_t end) {
    return end - start;
  }

  void update_content(lsRange range, std::string replacement) {
    assert(range.start.line < line_starts.size());
    assert(range.end.line < line_starts.size());

    #if TRACE_CONTENT_HOLDER
      fmt::println(stderr, "content.size() = {}", content.size());
      fmt::print(stderr, "line_starts = [");
      for (const auto& item: line_starts) {
        fmt::print(stderr, "{},", item);
      }
      fmt::println(stderr, "]");

      fmt::println(
        stderr,
        "range.start.line = {}, range.end.line = {}, line_starts.size() = {}",
        range.start.line,
        range.end.line,
        line_starts.size()
      );
    #endif
    
    size_t edited_start = line_starts[range.start.line] + range.start.character;
    size_t edited_end = line_starts[range.end.line] + range.end.character;

    assert(edited_start < content.size());
    assert(edited_end < content.size());

    if (length(edited_start, edited_end) < replacement.size()) {
      // Чтобы не инвалидировать итераторы, расширим до их взятия.
      size_t underflow = replacement.size() - length(edited_start, edited_end);
      std::fill_n(std::back_inserter(content), underflow, '\0');
    }

    // Хотим перезаписать интервал новыми данными.

    // Если интервал уменьшился, то переместим данные после него и сократим тем самым строку.
    //   Данные интервала затем перезапишем новыми.
    auto edited_start_it = content.begin() + edited_start;
    auto edited_end_it   = content.begin() + edited_end;
    #if TRACE_CONTENT_HOLDER
      fmt::println(stderr, "length(edited_start, edited_end) = {}", length(edited_start, edited_end));
    #endif
    if (length(edited_start, edited_end) > replacement.size()) {
      size_t excess = length(edited_start, edited_end) - replacement.size();
      
      // [ a | b c d e_1 e_2 e_3 | f g h i] -> [ a | b c d | f g h i]
      // Сократим интервал.
      std::move(edited_end_it, content.end(), edited_end_it - excess);

      // Заменим данные.
      std::move(replacement.begin(), replacement.end(), edited_start_it);

      // Удаляем лишнее в конце, данные оттуда уже переместили.
      content.erase(content.end() - excess, content.end());
    } else if (length(edited_start, edited_end) < replacement.size()) {
      // Интервал расширился. Место создали ранее, надо записать новое содержимое.
      size_t underflow = replacement.size() - length(edited_start, edited_end);

      std::move_backward(edited_end_it, content.end() - underflow, content.end());

      // Заменим данные.
      std::move(replacement.begin(), replacement.end(), edited_start_it);
    } else {
      std::move(replacement.begin(), replacement.end(), edited_start_it);
    }

    find_line_starts(range.start.line);
  }

  // Считая, что начала строк остались правильными до line_valid_until включительно
  //  (всегда можно указать нулевую, первую в 1-индексации, строку, уж ее начало-то
  //  правильное, она всегда с 0-го байта начинается). пересчитать начала строк.
  void find_line_starts(size_t line_valid_until) {
    size_t pos = line_starts[line_valid_until];
    line_starts.erase(line_starts.begin() + line_valid_until + 1, line_starts.end());

    for (; pos < content.size(); ++pos) {
      if (content[pos] == '\n' && pos + 1 < content.size()) {
        line_starts.push_back(pos + 1);
      }
    }
  }
};

class LSPCompilationDriver final : public CompilationDriver {
  using CompilationDriver::CompilationDriver;

  virtual lex::InputFile OpenFile(std::string_view name) override;

public:
  void PrepareForTooling() {
    ParseAllModules();
    RegisterSymbols();

    // Those in the beginning have the least dependencies (see TopSort(...))
    for (size_t i = 0; i < modules_.size(); i += 1) {
      ProcessModule(modules_[i].get());
    }

    for (auto& m : modules_) {
      m->InferTypes(solver_);
    }

    if (test_build) {
      FMT_ASSERT(modules_.back()->GetName() == main_module_,
                  "Last module should be the main one");
      return;
    }
  }
  
  void RunVisitor(Visitor* visitor) {
    // Модуль, который был основным, находится в конце списка модулей
    //   после тополнической сортировки. Т.к. в него все ребра входили,
    //   но никакие не выходили: если кто-то его импортирует, мы об этом
    //   не знаем.

    modules_.back()->RunTooling(visitor);
  }
};

class ViewedFile {
public:
  ViewedFile(lsDocumentUri uri)
    : uri_(std::move(uri)), abs_path_(uri_.GetAbsolutePath().path) {
      assert(abs_path_.is_absolute());

      std::ifstream file(abs_path_);
      auto content = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      editor_content.set_content(content);

      Recompile();
  }

  ViewedFile(const ViewedFile& other) = delete;
  ViewedFile(ViewedFile&& other) = default;

  void Recompile() {
      // Компилятор на данный момент ищет файлы в рабочей директории.
      //   В том числе, все импортируемые. Кроме стандартной библиотеки,
      //   которую он найдет и так, если мы укажем переменную окружения.
      //   Потому сменим рабочую директорию. Другие части нашего кода от
      //   этого не зависят.

      // Это и упрощение логики являются причинами однопоточного подхода.
      //   Его производительности хватает, а сложности, которые он
      //   создаст, в алгоритма и внутри компилятора (там есть 
      //   глобальные переменные) перевешивают необходимость.

      // https://stackoverflow.com/a/57096619
      fs::current_path(abs_path_.parent_path());

      std::string module_name = GetModuleName();
      diagnostic.reset();

      try {
        // Важно, чтобы module_name существовал все время выполнения
        //   этой функции, потому что compilation driver
        //   принимает эту строку как std::string_view.
        auto driver = std::make_unique<LSPCompilationDriver>(module_name);

        driver->PrepareForTooling();

        std::vector<lsDocumentSymbol> new_symbols;
        std::vector<SymbolUsage> new_usages;
        LSPVisitor visitor(std::string(abs_path_), &new_symbols, &new_usages);

        driver->RunVisitor(&visitor);

        last_driver = std::move(driver);
        symbols = std::move(new_symbols);
        usages = std::move(new_usages);
      } catch (const ErrorAtLocation& err) {
        diagnostic = lsDiagnostic{
          range: lsRange{
            LsPositionFromLexLocation(err.where()),
            LsPositionFromLexLocation(err.where())
          },
          severity: lsDiagnosticSeverity::Error,
          message: std::string(err.what()),
        };
        return;
      } catch (const std::exception& exc) {
        diagnostic = lsDiagnostic{
          range: lsRange{lsPosition{0, 0}, lsPosition{0, 0}},
          severity: lsDiagnosticSeverity::Error,
          message: std::string(exc.what()),
        };
        return;
      }
  }

  void RecompileOnLookup() {
    recompile_on_lookup = true;
  }

  void Lookup() {
    if (recompile_on_lookup) {
      Recompile();
      recompile_on_lookup = false;
    }
  }

  // Delete information about symbols after position (there was
  //   a change starting from this position). Symbols not touched
  //   by modification are kept. With assumption that their accessible
  //   scope symbols and definitions aren't changed by the modification.
  //   Because it can only reference what is before, so it doesn't reference
  //   anything modified.
  void InvalidateAfterPosition(lsPosition position) {
    #if TRACE_INVALIDATION
      fmt::println(
        stderr,
        "Before InvalidateAfterPosition symbols.size() = {}, usages.size() = {}",
        symbols.size(),
        usages.size()
    );
    #endif
    
    std::erase_if(symbols, [&](const lsDocumentSymbol& symbol) {
      const lsPosition& end = symbol.range.start;
      return end.line > position.line || (end.line == position.line && end.character >= position.character);
    });

    std::erase_if(usages, [&](const SymbolUsage& usage) {
      const lsPosition& end = usage.range.end;
      if (end.line > position.line || (end.line == position.line && end.character >= position.character)) {
        return true;
      }

      lsPosition def_end = LsPositionFromLexLocation(usage.decl_def.def_position);
      if (def_end.line > position.line || (def_end.line == position.line && def_end.character >= position.character)) {
        return true;
      }

      lsPosition decl_end = LsPositionFromLexLocation(usage.decl_def.decl_position);
      if (decl_end.line > position.line || (decl_end.line == position.line && decl_end.character >= position.character)) {
        return true;
      }

      return false;
    });

    #if TRACE_INVALIDATION
      fmt::println(
        stderr,
        "After InvalidateAfterPosition symbols.size() = {}, usages.size() = {}",
        symbols.size(),
        usages.size()
      );
    #endif
    
  }
private:
  std::string GetModuleName() {
    // Module.et -> Module
    return abs_path_.filename().replace_extension();
  }
public:
  lsDocumentUri uri_;
  fs::path abs_path_;

  std::optional<lsDiagnostic> diagnostic;
  std::vector<lsDocumentSymbol> symbols;
  std::vector<SymbolUsage> usages;

  // Last driver is stored for the module pointers to be up to date.
  //   Otherwise module pointers are freed upon compilation driver
  //   destruction.
  std::unique_ptr<LSPCompilationDriver> last_driver;

  // Previosly we'd store std::string here with the full contents.
  //   But vscode doesn't tell the changed position, if we use
  //   full synchronization.
  // 
  EditedFile editor_content;

  bool recompile_on_lookup = false;
};



std::unordered_map<std::string, ViewedFile> file_cache;

lex::InputFile LSPCompilationDriver::OpenFile(std::string_view name) {
  auto rel_path = std::string(name) + ".et";

  // The file we're asked to open is always in cwd. We cd there before
  //   opening, because etude compiler expect that (or it'll search
  //   in stdlib, but we get absolute paths from language protocol
  //   client anyway and then set module name to be just the filename
  //   without .et).

  // Also forcing lowercase on windows. Because default fs there (ntfs)
  //   is not case-sensitive.
  // TODO: check vscode extension works on windows.
  std::string abs_path = lsp::NormalizePath(rel_path, false);
  auto it = file_cache.find(abs_path);
  if (it != file_cache.end()) {
    auto& file = it->second;

    return lex::InputFile{std::stringstream(file.editor_content.content), std::move(abs_path)};
  } 
  
  return CompilationDriver::OpenFile(name);
}

int main(int argc, char** argv) {
  if (argc < 1 || argv[0] == nullptr) {
    std::cerr << "Invalid usage, missing executable path in argv.";
    return -1;
  }

  fs::path exec_path = fs::absolute(fs::path(argv[0]));
  fs::path exec_dir  = exec_path.parent_path();
  fs::path stdlib_path  = exec_dir / "etude_stdlib";
  // Makes a copy of strings pointed by name and value.
  setenv("ETUDE_STDLIB", std::string(stdlib_path).c_str(), true);

  std::atomic<bool> initialized = false;
  std::atomic<bool> exiting = false;

  Logger logger;

  auto server_endpoint = std::make_shared<GenericEndpoint>(logger);
  // TODO: handlers.

  auto json_handler = std::make_shared<lsp::ProtocolJsonHandler>();
  RemoteEndPoint client_endpoint(json_handler, server_endpoint, logger, lsp::Standard, 1);

  // https://github.com/kuafuwang/LspCpp/blob/e0b443d42e7d23638d727ac8ef6839b9e527bf0a/examples/StdIOServerExample.cpp#L57
  client_endpoint.registerHandler([&](const td_initialize::request& request) {
    td_initialize::response response;
    
    response.id = request.id;
    response.result.capabilities = lsServerCapabilities {
        .textDocumentSync = {{{}, lsTextDocumentSyncOptions{
          .openClose = true,
          .change = lsTextDocumentSyncKind::Incremental,
          .save = lsSaveOptions {
            .includeText = false
          },
        }}},
        .hoverProvider = {true},
        .definitionProvider = {{true, {}}},
        .documentHighlightProvider = {{true, {}}},
        .documentSymbolProvider = {{true, {}}},
        .renameProvider = {{{}, RenameOptions{true}}},
        .documentLinkProvider = lsDocumentLinkOptions {},
    };

    return response;
  });

  auto update_diagnostics = [&](const ViewedFile& file) {
    Notify_TextDocumentPublishDiagnostics::notify notify;
    notify.params.uri = file.uri_;
    if (file.diagnostic.has_value()) {
      notify.params.diagnostics.push_back(file.diagnostic.value());
    }

    client_endpoint.sendNotification(notify);
  };

  auto find_file = [&](const lsDocumentUri& uri) -> ViewedFile& {
    auto file_it = file_cache.find(uri.GetAbsolutePath().path);
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

      // Нам нужен путь, т.к. при открытии файла мы смотрим
      //   в этот кеш, удобнее оперировать путями, чем uri.
      //   Возможно, именно мне сейчас..
      // На windows все приводится к нижнему регистру,
      //   внутри GetAbsolutePath() есть вызов
      //   lsp::NormalizePath, там есть параметр
      //   force_lower_on_windows. чтобы
      //   не хранить один файл дважды. Ведь еще есть
      //   чтение кеша при импортировании модулей внутри
      //   etude. А там модули могут быть и большими,
      //   и маленькими быть написаны. И на винде будет
      //   компилироваться.. Тогда на винде просто везде
      //   сделаем маленькими.
      // На самом деле, это свойство файловой системы,
      //   она может быть чувствительна или нет к регистру.
      // Но все эти ухищрения только для случая, где
      //   у кого-то неправильный регистр.

      auto result = file_cache.insert({uri.GetAbsolutePath().path, ViewedFile(std::move(file))});
      assert(result.second);
      file_it = std::move(result.first);
    }

    ViewedFile& file = file_it->second;

    file.Lookup();
    update_diagnostics(file); // Cheap, can do on each request or notification.

    return file;
  };

  auto close_file = [&](const lsDocumentUri& uri) {
    file_cache.erase(uri.GetAbsolutePath().path);
  };

  client_endpoint.registerHandler([&](const td_symbol::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_symbol::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    response.result = file.symbols;

    return response;
  });

  client_endpoint.registerHandler([&](const td_definition::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_definition::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }
    }

    std::vector<LocationLink> locations; 
    if (usage != nullptr) {
      // Distinguish decl and def positions like done in cquery:
      //    https://github.com/jacobdufault/cquery/blob/9b80917cbf7d26b78ec62b409442ecf96f72daf9/src/messages/text_document_definition.cc#L96
      locations.push_back(LocationLink {
        targetUri: lsDocumentUri::FromPath(usage->decl_def.decl_position.unit->GetAbsPath().string()),
        targetRange: lsRange{
          LsPositionFromLexLocation(usage->decl_def.decl_position),
          LsPositionFromLexLocation(usage->decl_def.decl_position),
        },
        targetSelectionRange: lsRange{
          LsPositionFromLexLocation(usage->decl_def.decl_position),
          LsPositionFromLexLocation(usage->decl_def.decl_position),
        },
      });
    }

    response.result = {{}, locations};

    return response;
  });

  client_endpoint.registerHandler([&](const td_highlight::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_highlight::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }
    }

    std::vector<lsDocumentHighlight> highlights; 
    if (usage != nullptr) {
      for (auto& usage_item: file.usages) {
        if (usage_item.decl_def == usage->decl_def) {
          highlights.push_back(lsDocumentHighlight{usage_item.range});          
        }
      }
    }

    response.result = std::move(highlights);

    return response;
  });

  client_endpoint.registerHandler([&](const td_hover::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_hover::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }

      if (usage != nullptr && usage->type_name.has_value()) {
        response.result.contents = {TextDocumentHover::Left{{{"of " + usage->type_name.value(), {}}}}, {}};
        response.result.range = usage->range;
      }
    }

    return response;
  });

  client_endpoint.registerHandler([&](const td_prepareRename::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_prepareRename::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }
    }

    if (usage == nullptr) {
      return response;
    }

    EditedFile& content = file.editor_content;
    const char* old_name = content.content.c_str() + content.line_starts[usage->range.start.line] + usage->range.start.character;

    // There is no multiline tokens in Etude as of now.
    size_t len = usage->range.end.character - usage->range.start.character + 1;
    
    if (file.last_driver->GetModuleOf(std::string_view(old_name, len)) != nullptr) {
      // Cannot rename across modules for now! Need buildsystem integration to get all files to rename.
      return response;
    }

    response.result.first = usage->range;


    return response;
  });

  client_endpoint.registerHandler([&](const td_rename::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_rename::response response;
    response.id = request.id;

    if (!initialized) {
      return response;
    }

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }
    }

    if (usage == nullptr) {
      return response;
    }

    EditedFile& content = file.editor_content;
    const char* old_name = content.content.c_str() + content.line_starts[usage->range.start.line] + usage->range.start.character;

    // There is no multiline tokens in Etude as of now.
    size_t len = usage->range.end.character - usage->range.start.character + 1;
    
    if (file.last_driver->GetModuleOf(std::string_view(old_name, len)) != nullptr) {
      // Cannot rename across modules for now! Need buildsystem integration to get all files to rename.
      return response;
    }

    response.result.changes = decltype(response.result.changes)::value_type();

    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.decl_def != usage->decl_def) {
        continue;
      }

      auto uri = request.params.textDocument.uri.raw_uri_;

      response.result.changes.value()[uri].push_back(lsTextEdit{usage_item.range, request.params.newName});
    }


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
    
    auto& file_uri = notify.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    logger.log(lsp::Log::Level::INFO, "opened file with uri " + notify.params.textDocument.uri.raw_uri_);
  });


  client_endpoint.registerHandler([&](Notify_TextDocumentDidChange::notify& notify) {
    if (!initialized) {
        return;
    }

    if (notify.params.contentChanges.empty()) {
      logger.warning("didChange event without contentChanges for file " + notify.params.textDocument.uri.GetAbsolutePath().path);
      return;      
    }

    auto& file_uri = notify.params.textDocument.uri;
    ViewedFile& target_file = find_file(file_uri);

    for (const lsTextDocumentContentChangeEvent& event: notify.params.contentChanges) {
      assert(event.range.has_value()); // Значение отсутствует только для обновлений в формате "весь файл сразу".
      target_file.editor_content.update_content(event.range.value(), event.text);
      target_file.InvalidateAfterPosition(event.range->end);
    }

    target_file.Recompile();
    update_diagnostics(target_file);

    for (auto& [_, file]: file_cache) {
      if (file.abs_path_ == target_file.abs_path_) {
        // Already re-evaluated, avoid unnecessary work.
        continue;
      }

      file.RecompileOnLookup();
    }
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidSave::notify& notify) {
    if (!initialized) {
        return;
    }
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidClose::notify& notify) {
    if (!initialized) {
        return;
    }
    logger.log(lsp::Log::Level::INFO, "closing file with uri " + notify.params.textDocument.uri.raw_uri_);
    // Editor asks what to save pending changes in the file before closing.
    close_file(notify.params.textDocument.uri);
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