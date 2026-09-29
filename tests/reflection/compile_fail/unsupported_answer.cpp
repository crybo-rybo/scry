#include <scry/harness.hpp>

struct Reading {
  double value{};
  char unit{};
};

scry::Result<scry::Answered<Reading>> read(scry::Harness& harness,
                                           scry::Conversation& conversation) {
  return harness.ask<Reading>(conversation, "Must not compile");
}
