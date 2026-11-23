cmake_minimum_required(VERSION 3.16)
project(tcpp LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TCPP_BUILD_SHARED "Build shared library" ON)

set(TCPP_SOURCES
  src/lexer.cpp
  src/literal.cpp
  src/preprocessor.cpp
  src/parser.cpp
  src/codegen.cpp
)

add_library(tcpp_core STATIC ${TCPP_SOURCES})
target_include_directories(tcpp_core PUBLIC src)
if(MSVC)
  target_compile_options(tcpp_core PRIVATE /W3)
else()
  target_compile_options(tcpp_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(tcpp src/main.cpp)
target_link_libraries(tcpp PRIVATE tcpp_core)
target_include_directories(tcpp PRIVATE src)

if(TCPP_BUILD_SHARED)
  add_library(tcpp_shared SHARED ${TCPP_SOURCES})
  target_include_directories(tcpp_shared PUBLIC src)
  set_target_properties(tcpp_shared PROPERTIES OUTPUT_NAME tcpp)
  if(MSVC)
    target_compile_options(tcpp_shared PRIVATE /W3)
  else()
    target_compile_options(tcpp_shared PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  install(TARGETS tcpp_shared LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
endif()

install(TARGETS tcpp tcpp_core RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
