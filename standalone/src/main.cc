import modulizer;
import std;

int main(int argc, const char **argv) {
  enum { kDefault, kWrapper, kHeaders, kFull, kConsumers } mode = kDefault;
  std::vector<const char *> filtered;
  filtered.reserve(argc);
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    auto a = std::string_view(argv[i]);
    if (a == "--wrapper") {
      mode = kWrapper;
    } else if (a == "--headers") {
      mode = kHeaders;
    } else if (a == "--full") {
      mode = kFull;
    } else if (a == "--consumers") {
      mode = kConsumers;
    } else {
      filtered.push_back(argv[i]);
    }
  }

  switch (mode) {
  case kWrapper:   return modulizer::run_wrapper(filtered.size(), filtered.data());
  case kHeaders:   return modulizer::run_headers_rewrite(filtered.size(), filtered.data());
  case kFull:      return modulizer::run_full_rewrite(filtered.size(), filtered.data());
  case kConsumers: return modulizer::run_consumers_rewrite(filtered.size(), filtered.data());
  default:         return modulizer::run(filtered.size(), filtered.data());
  }
}
