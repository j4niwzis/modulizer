#include "gtest/gtest-macros.h"
import gtest;
import libtooling;
import modulizer;
import std;

namespace {

std::string gDataDir = TEST_DATA_DIR;

std::string data_path(llvm::StringRef fname) {
  return std::format("{}/{}", gDataDir, fname.str());
}

} // namespace
// ModulizerTest suite
TEST(ModulizerTest, ModuleLinks) {
  GTEST_SUCCEED();
}

TEST(ModulizerTest, ListFunctionsConsumerInstantiation) {
  modulizer::ListFunctionsAction action;
  GTEST_SUCCEED();
}
