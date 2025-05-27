cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(etude-lsp-server)
set(CMAKE_CXX_STANDARD 20)
if(NOT "$ENV{BUILD_SANITIZERS}" STREQUAL "1")
    set(BUILD_SHARED_LIBS OFF) # So that it is portable across linuxes.
    if(WIN32 AND MSVC)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    # If not compiling for windows with msvc (mingw will work with -static)
    #   and not compiling for APPLE (necessary libraries should be static,
    #   so we should depend on system libraries only).
    # On apple it's not possible to build fully static.
    #   https://stackoverflow.com/questions/3801011/ld-library-not-found-for-lcrt0-o-on-osx-10-6-with-gcc-clang-static-flag
    #   https://stackoverflow.com/a/54890434
    if (NOT (WIN32 AND MSVC) AND NOT APPLE)
        add_compile_options(-static)
        add_link_options(-static)
    endif()
else()
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Fix too many sections error.
#   https://stackoverflow.com/a/62281145
#   https://stackoverflow.com/a/64287929
if(WIN32)
  add_compile_options(-Wa,-mbig-obj -Wl,--oformat,pe-bigobj-x86-64)
  add_link_options(-Wa,-mbig-obj -Wl,--oformat,pe-bigobj-x86-64)
endif()

# add_compile_options(-fsanitize=thread)
# add_link_options(-fsanitize=thread)

include(cmake/CPM.cmake)

add_executable(server src/server.cpp src/lsp_visitor.cpp)

CPMAddPackage("gh:valeriy-zainullin/LspCpp-tmp-fork#master")
# Так вот почему санитайзеры ругаются. Комплиятор уже при сборке говорит о том,
#   что могут быть проблемы. Если их добавить в сборку, то выйдут ошибки компиляции
#   на эту библиотеку. Эту ошибку в runtime можно избежать, если знать, как заставить эту
#   сетевую библиотеку перезаписать неинициализированные поля. Постараемся это
#   сделать.. Баги, баги, никто не собирал с санитайзерами.
target_compile_options(network-uri PRIVATE "-Wno-error=maybe-uninitialized")
target_link_libraries(server PRIVATE lspcpp)

add_subdirectory(etude)
# CPMAddPackage("gh:otakubeam/etude#effcece34dacc2e20d5db81c276b0e22ae984814")
target_link_libraries(server PRIVATE compiler)

target_compile_definitions(server PRIVATE TRACE_VISITOR=0)

