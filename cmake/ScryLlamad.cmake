# The optional llamad backend: ProviderDialect::llamad, gRPC over a Unix socket.
# Include after add_library(scry ...) and ScryDeveloperTools.cmake, only when
# SCRY_WITH_LLAMAD is on.
#
# Scry consumes llamad's wire contract and nothing else: the one file
# proto/llamad/v1/llamad.proto, from a pinned fetch or from SCRY_LLAMAD_SOURCE_DIR.
# llamad's own CMake project is never added, because it requires C++26
# reflection at configure time and Protobuf's CONFIG package, and the generated
# stubs need neither. src/backend/ uses the generated stub directly, never
# llamad's reflected client library.

if(SCRY_CLANG_TOOLING)
  message(
    FATAL_ERROR
    "SCRY_WITH_LLAMAD is not available in the SCRY_CLANG_TOOLING build"
  )
endif()

# Protobuf's own CONFIG package wins where it exists, because newer releases
# need the Abseil dependencies it declares; distributions that ship only CMake's
# FindProtobuf module (Ubuntu 24.04) fall back to module mode. The installed
# package config repeats whichever mode found it here.
find_package(Protobuf CONFIG QUIET)
if(Protobuf_FOUND)
  set(SCRY_LLAMAD_PROTOBUF_FIND_ARGS "CONFIG")
else()
  find_package(Protobuf REQUIRED)
  set(SCRY_LLAMAD_PROTOBUF_FIND_ARGS "")
endif()
find_package(gRPC CONFIG REQUIRED)

if(SCRY_LLAMAD_SOURCE_DIR)
  set(SCRY_LLAMAD_ROOT "${SCRY_LLAMAD_SOURCE_DIR}")
else()
  # SOURCE_SUBDIR names a directory without a CMakeLists.txt, so
  # FetchContent_MakeAvailable populates the checkout and adds nothing to the
  # build. The empty GIT_SUBMODULES skips llamad's llama.cpp submodule.
  FetchContent_Declare(
    llamad_proto
    GIT_REPOSITORY https://github.com/crybo-rybo/llamad.git
    GIT_TAG 80deb601e6e7bdba72feb7613b9833a3d1ca81fc
    GIT_SUBMODULES ""
    SOURCE_SUBDIR proto
  )
  FetchContent_MakeAvailable(llamad_proto)
  set(SCRY_LLAMAD_ROOT "${llamad_proto_SOURCE_DIR}")
endif()

set(SCRY_LLAMAD_PROTO "${SCRY_LLAMAD_ROOT}/proto/llamad/v1/llamad.proto")
if(NOT EXISTS "${SCRY_LLAMAD_PROTO}")
  message(
    FATAL_ERROR
    "llamad's wire contract was not found at ${SCRY_LLAMAD_PROTO}; point \
SCRY_LLAMAD_SOURCE_DIR at a llamad checkout or clear it to fetch the pinned one"
  )
endif()

set(SCRY_LLAMAD_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated/llamad")
file(MAKE_DIRECTORY "${SCRY_LLAMAD_GENERATED_DIR}")
protobuf_generate(
  LANGUAGE cpp
  PROTOS "${SCRY_LLAMAD_PROTO}"
  IMPORT_DIRS "${SCRY_LLAMAD_ROOT}/proto"
  PROTOC_OUT_DIR "${SCRY_LLAMAD_GENERATED_DIR}"
  OUT_VAR SCRY_LLAMAD_MESSAGE_SOURCES
)
protobuf_generate(
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  PROTOS "${SCRY_LLAMAD_PROTO}"
  IMPORT_DIRS "${SCRY_LLAMAD_ROOT}/proto"
  PROTOC_OUT_DIR "${SCRY_LLAMAD_GENERATED_DIR}"
  OUT_VAR SCRY_LLAMAD_SERVICE_SOURCES
)
set(
  SCRY_LLAMAD_GENERATED_SOURCES
  ${SCRY_LLAMAD_MESSAGE_SOURCES}
  ${SCRY_LLAMAD_SERVICE_SOURCES}
)

# The generated code compiles into scry itself, so the installed static archive
# carries it and scryTargets exports no extra target. It is not Scry's code:
# -w keeps protoc's output out of the warnings-as-errors set, and clang-tidy
# skips it.
set_source_files_properties(
  ${SCRY_LLAMAD_GENERATED_SOURCES}
  PROPERTIES
    GENERATED ON
    COMPILE_OPTIONS "-w"
    SKIP_LINTING ON
)
# Under GCC 16 and C++26, Abseil's options.h (reached from <grpcpp/...>)
# includes the removed <ciso646>, whose #warning fires even from a system
# header. Only the translation units that include gRPC headers opt out of it.
set(SCRY_LLAMAD_GRPC_COMPILE_OPTIONS "-Wno-cpp")
set_source_files_properties(
  src/backend/llamad_backend.cpp
  PROPERTIES COMPILE_OPTIONS "${SCRY_LLAMAD_GRPC_COMPILE_OPTIONS}"
)

target_sources(
  scry
  PRIVATE
    src/backend/llamad_backend.cpp
    src/backend/llamad_wire.cpp
    ${SCRY_LLAMAD_GENERATED_SOURCES}
)
target_include_directories(scry SYSTEM PRIVATE "${SCRY_LLAMAD_GENERATED_DIR}")
target_link_libraries(scry PRIVATE gRPC::grpc++ protobuf::libprotobuf)
