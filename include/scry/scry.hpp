#pragma once

/**
 * @file scry.hpp
 * @brief Convenience umbrella header for Scry's stable C++23 API.
 *
 * Include this header when compile-time dependency minimization is not important. It
 * gathers the complete reflection-independent public surface: configuration,
 * conversations, errors, callback values, Harness, explicit tools, turns, JSON, and
 * version information.
 *
 * The experimental C++26 reflection component is intentionally absent. Consumers of
 * that optional package component must link `scry::reflection` and include
 * `<scry/reflection.hpp>` explicitly, preserving a clean C++23 core dependency graph.
 */

#include <scry/config.hpp>
#include <scry/conversation.hpp>
#include <scry/error.hpp>
#include <scry/events.hpp>
#include <scry/harness.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <scry/turn.hpp>
#include <scry/turn_id.hpp>
#include <scry/unique_function.hpp>
#include <scry/version.hpp>
