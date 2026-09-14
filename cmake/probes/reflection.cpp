#include <cstddef>
#include <meta>
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
int main() { return 0; }
