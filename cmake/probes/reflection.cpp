#include <cstddef>
#include <meta>
#include <string_view>
#if !defined(__cpp_impl_reflection)
#error P2996 feature macro is missing
#endif

template <std::size_t Size> struct probe_description {
  char text[Size]{};
  consteval probe_description(const char (&value)[Size]) {
    for (std::size_t index = 0; index < Size; ++index) {
      text[index] = value[index];
    }
  }
};

struct probe_arguments {
  [[= probe_description{"probe"}]] int value{};
};

consteval bool has_required_annotation_queries() {
  constexpr auto members =
      std::define_static_array(std::meta::nonstatic_data_members_of(
          ^^probe_arguments, std::meta::access_context::unchecked()));
  constexpr auto annotations =
      std::define_static_array(std::meta::annotations_of(members[0]));
  if constexpr (annotations.size() != 1) {
    return false;
  }
  constexpr auto annotation = annotations[0];
  if (!std::meta::is_annotation(annotation)) {
    return false;
  }
  const auto type = std::meta::type_of(annotation);
  if (!std::meta::has_template_arguments(type) ||
      std::meta::template_of(type) != ^^probe_description) {
    return false;
  }
  using Annotation = [:std::meta::type_of(annotation):];
  constexpr auto payload = std::meta::extract<Annotation>(annotation);
  return payload.text[0] == 'p';
}

static_assert(has_required_annotation_queries());

// The codec reads annotations on class definitions, recovers fixed-string text
// through a substituted variable template, catches std::meta::exception while
// classifying types, and prints generated static_assert messages.
struct[[= probe_description{"tagged"}]] probe_tagged {};

struct probe_text {
  const char* data;
  std::size_t size;
};

template <auto Annotation>
inline constexpr probe_text probe_text_v{Annotation.text, sizeof(Annotation.text) - 1};

struct probe_incomplete;

consteval bool is_probe_complete(const std::meta::info type) {
  try {
    static_cast<void>(std::meta::size_of(type));
  } catch (const std::meta::exception&) {
    return false;
  }
  return true;
}

consteval bool has_required_codec_queries() {
  const auto annotations = std::meta::annotations_of(^^probe_tagged);
  if (annotations.size() != 1) {
    return false;
  }
  const auto text = std::meta::extract<probe_text>(std::meta::substitute(
      ^^probe_text_v, {
                          std::meta::constant_of(annotations[0])}));
  return text.size == 6 && text.data[0] == 't' && is_probe_complete(^^probe_tagged) &&
         !is_probe_complete(^^probe_incomplete);
}

static_assert(has_required_codec_queries());

constexpr const char* probe_message = std::define_static_string("generated message");
static_assert(has_required_codec_queries(),
              std::string_view{probe_message, sizeof("generated message") - 1});

int main() { return 0; }
