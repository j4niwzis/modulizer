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
// EntityExtraction suite
TEST(EntityExtraction, FindsPublicClass) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "Foo" && it.kind == EntityItem::kClass)
      found = true;
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, ClassDefinedByNoHeaderIsCrossModuleForwardDeclared) {
  // `Accessor` is declared by the library and defined by none of its headers:
  // whatever defines it does so outside the module — an implementation unit,
  // or a consumer, as with the friend accessors a test defines to reach a
  // class's private members. That definition lands in the global module, so
  // the declaration must be `extern "C++"` too; a module-attached one would be
  // a different entity and the friendship would not apply to the definition.
  std::vector<std::string> headers = {data_path("openfwd_lib/api.h")};
  std::vector<std::string> extra_args = {"-I", gDataDir};
  auto fqns = cross_module_fwd_declared_fqns(headers, extra_args);
  EXPECT_TRUE(std::find(fqns.begin(), fqns.end(), "openfwd_lib::Accessor") !=
              fqns.end())
      << "a class the library never defines must be treated as a shared "
         "global-module entity";
  EXPECT_TRUE(std::find(fqns.begin(), fqns.end(), "openfwd_lib::Widget") ==
              fqns.end())
      << "a class defined in the header that declares it is a module entity";
  EXPECT_TRUE(std::find(fqns.begin(), fqns.end(), "openfwd_lib::Plain") ==
              fqns.end())
      << "a class defined where it is declared is a module entity";
}

TEST(EntityExtraction, FunctionTemplateCompleteReflectsBody) {
  // fwtpl.h forward-declares the function template `foo` (no body) and defines
  // `bar` (has a body). The `complete` flag must reflect whether a body exists:
  // cross_module_fwd_declared_fqns relies on it.
  auto model = analyze_file(data_path("fwtpl.h"));
  bool foo_seen = false, bar_seen = false, foo_complete = true,
       bar_complete = false;
  for (auto &it : model.items) {
    if (it.name == "foo") {
      foo_seen = true;
      foo_complete = it.complete;
    }
    if (it.name == "bar") {
      bar_seen = true;
      bar_complete = it.complete;
    }
  }
  EXPECT_TRUE(foo_seen);
  EXPECT_TRUE(bar_seen);
  EXPECT_FALSE(foo_complete)
      << "a forward-declared function template (no body) must be complete=false";
  EXPECT_TRUE(bar_complete)
      << "a function template with a body must stay complete=true";
}

TEST(EntityExtraction, FindsPublicFunction) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "foo" && it.kind == EntityItem::kFunction)
      found = true;
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, FindsEnum) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "Baz" && it.kind == EntityItem::kEnum)
      found = true;
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, FindsUsingAlias) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "Size" && it.kind == EntityItem::kAlias)
      found = true;
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, FindsConstexprVariable) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "VERSION") { found = true; break; }
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, FindsDetailEntities) {
  auto model = analyze_file(data_path("macro_test.h"));
  bool found = false;
  for (auto &it : model.items)
    if (it.name == "Foo") { found = true; break; }
  EXPECT_TRUE(found);
}

TEST(EntityExtraction, DoesNotDuplicateRecords) {
  // Bug: VisitCXXRecordDecl pushed each class/struct twice (once bare,
  // once via add_item), so guarded entities appeared as unguarded dupes.
  auto model = analyze_file(data_path("macro_test.h"));
  std::set<std::string> seen;
  for (auto &it : model.items) {
    auto key = fqn_of(it.ns_path, it.name) + "#" + std::to_string(it.kind);
    EXPECT_FALSE(seen.count(key))
        << "entity " << key << " must appear exactly once";
    seen.insert(std::move(key));
  }
}

TEST(EntityExtraction, FindsMacros) {
  auto model = analyze_file(data_path("macro_test.h"));
  EXPECT_GT(model.macros.size(), 0u);
}

TEST(EntityExtraction, FindsGuardedEntities) {
  auto model = analyze_file(data_path("guarded.h"));
  bool found_windows = false, found_fallback = false;
  bool found_feature = false, found_legacy = false, found_always = false;
  for (auto &it : model.items) {
    if (it.name == "WindowsOnly") {
      found_windows = true;
      EXPECT_NE(it.guard_prefix.find("HAS_WINDOWS"), std::string::npos);
    }
    if (it.name == "UnixFallback") {
      found_fallback = true;
      if (!it.guard_prefix.empty())
        EXPECT_NE(it.guard_prefix.find("HAS_WINDOWS"), std::string::npos);
    }
    if (it.name == "feature_fn") found_feature = true;
    if (it.name == "Legacy") found_legacy = true;
    if (it.name == "AlwaysAvailable") found_always = true;
  }
  EXPECT_TRUE(found_windows) << "WindowsOnly not found";
  EXPECT_TRUE(found_fallback) << "UnixFallback not found";
  EXPECT_TRUE(found_feature) << "feature_fn not found";
  EXPECT_TRUE(found_legacy) << "Legacy not found";
  EXPECT_TRUE(found_always) << "AlwaysAvailable not found";
}

TEST(EntityExtraction, ConflictingFeatureGuards) {
  auto model = analyze_file(data_path("conflicting_features.h"));
  bool found_a = false, found_b = false;
  for (auto &it : model.items) {
    if (it.name == "a") {
      found_a = true;
      EXPECT_NE(it.guard_prefix.find("#ifdef FEATURE_A"), std::string::npos);
    }
    if (it.name == "b") {
      found_b = true;
      EXPECT_NE(it.guard_prefix.find("#ifdef FEATURE_B"), std::string::npos);
    }
  }
  EXPECT_TRUE(found_a) << "function a not found";
  EXPECT_TRUE(found_b) << "function b not found";
}

TEST(EntityExtraction, HardConflictNeedsPerFeaturePass) {
  auto model = analyze_file(data_path("hard_conflict.h"));
  bool found_fn_a = false, found_struct_a = false, found_b = false;
  for (auto &it : model.items) {
    if (it.name == "a" && it.kind == EntityItem::kStruct) found_struct_a = true;
    if (it.name == "a" && it.kind == EntityItem::kFunction) found_fn_a = true;
    if (it.name == "b") found_b = true;
  }
  EXPECT_TRUE(found_fn_a) << "function a not found";
  EXPECT_TRUE(found_struct_a) << "struct a not found";
  EXPECT_TRUE(found_b) << "function b not found";
}

TEST(EntityExtraction, InternalReachableViaPublicMacro) {
  auto model = analyze_files({data_path("lib_a.h"), data_path("lib_macro.h")});
  auto r = analyze_macro_reachability(model.macros, model,
                                       kDefaultInternalFilter);
  bool found = false;
  for (auto &fqn : r.reachable_fqns) {
    if (fqn.find("internal::foo") != std::string::npos) found = true;
  }
  EXPECT_TRUE(found) << "internal::foo should be reachable via MACRO";
}
