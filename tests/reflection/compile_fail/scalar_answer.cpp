#include <scry/harness.hpp>

scry::Result<scry::Turn> count(scry::Harness& harness,
                               scry::Conversation& conversation) {
  return harness.send<int>(conversation, "Must not compile");
}
