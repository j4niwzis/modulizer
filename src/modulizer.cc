module;

export module modulizer;

// The umbrella module: `import modulizer;` re-exports every sub-module, so
// consumers never need to name them individually. All implementation lives in
// the modulizer.* modules (including the CLI entry points in
// modulizer.cli); this module only aggregates them.
export import modulizer.analyzer;
export import modulizer.cli;
export import modulizer.consumer_rewriter;
export import modulizer.consumer_trace;
export import modulizer.cross_module;
export import modulizer.header_rewriter;
export import modulizer.include_analysis;
export import modulizer.macro_analyzer;
export import modulizer.naming;
export import modulizer.rewrite_export;
export import modulizer.rewrite_impls;
export import modulizer.rewrite_includes;
export import modulizer.rewrite_inject;
export import modulizer.rewrite_macros;
export import modulizer.rewrite_orchestration;
export import modulizer.rewrite_util;
export import modulizer.rewrite_visitors;
export import modulizer.trace_visitors;
export import modulizer.util;
export import modulizer.wrapper_gen;
import libtooling;
import std;
