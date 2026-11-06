# Prefer a compatible installed package or targets supplied by a parent project.
# The fallback is pinned so a fresh checkout builds reproducibly.
set(SCRY_SCRYDANTIC_MIN_VERSION 0.2.0)
option(SCRY_FETCH_SCRYDANTIC "Fetch Scrydantic when no package is available" ON)

if(NOT TARGET scrydantic::scrydantic)
  find_package(scrydantic ${SCRY_SCRYDANTIC_MIN_VERSION} CONFIG QUIET)
endif()

if(NOT TARGET scrydantic::scrydantic)
  if(NOT SCRY_FETCH_SCRYDANTIC)
    message(
      FATAL_ERROR
      "Scrydantic ${SCRY_SCRYDANTIC_MIN_VERSION} is required. Set CMAKE_PREFIX_PATH or scrydantic_DIR to its installation, or enable SCRY_FETCH_SCRYDANTIC."
    )
  endif()

  block()
    # Dependency tests and examples belong to Scrydantic's own repository.
    set(SCRYDANTIC_BUILD_TESTS OFF)
    set(SCRYDANTIC_BUILD_EXAMPLES OFF)
    set(SCRYDANTIC_SANITIZER ${SCRY_SANITIZER})
    FetchContent_Declare(
      scrydantic
      GIT_REPOSITORY https://github.com/crybo-rybo/scrydantic.git
      GIT_TAG 33ec842414b8ab10b43efd16bb1a8a82a6bbde1e # v0.2.0
      SYSTEM
    )
    FetchContent_MakeAvailable(scrydantic)
  endblock()
endif()

if(NOT TARGET scrydantic::json)
  message(FATAL_ERROR "Scrydantic must provide scrydantic::scrydantic and scrydantic::json")
endif()
