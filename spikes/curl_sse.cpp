#include <cstddef>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace {

std::size_t append_bytes(char* data, std::size_t size, std::size_t count,
                         void* destination) noexcept {
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    return 0;
  }

  const auto byte_count = size * count;
  static_cast<std::string*>(destination)->append(data, byte_count);
  return byte_count;
}

} // namespace

int main() {
  const auto* version = curl_version_info(CURLVERSION_NOW);
  if (version == nullptr || version->version_num < CURL_VERSION_BITS(7, 84, 0) ||
      (version->features & CURL_VERSION_THREADSAFE) == 0) {
    return 1;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    return 2;
  }

  int result = 0;
  {
    const auto easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>{
        curl_easy_init(), &curl_easy_cleanup};
    std::string received;
    if (!easy ||
        curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, append_bytes) != CURLE_OK ||
        curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &received) != CURLE_OK) {
      result = 3;
    } else {
      auto first = std::string{"data: hel"};
      auto second = std::string{"lo\n\n"};
      append_bytes(first.data(), 1, first.size(), &received);
      append_bytes(second.data(), 1, second.size(), &received);
      result = received == std::string_view{"data: hello\n\n"} ? 0 : 4;
    }
  }

  curl_global_cleanup();
  return result;
}
