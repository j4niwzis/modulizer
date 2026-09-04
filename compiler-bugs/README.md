# Compiler bugs

One directory per defect found while converting real libraries, each reduced to
something that stands on its own. A conversion that trips over one of these
looks like a modulizer bug until the language shape is pulled out of it, so the
reduction is kept.

Each directory holds:

- `ISSUE.md` — the report: what the standard says, what each compiler does,
  the versions it was seen on, and where it bites a conversion.
- the sources, named `foo`/`bar`/`Thing` and the like. Never the names from the
  library it was found in: what is kept here is the language shape, and a name
  carried over from the original only makes it look like someone else's code.
- `repro.sh` — runnable, prints the compiler versions first.

A workaround for one of these belongs behind an option naming the compiler that
needs it, not applied to everything. The other compiler is not wrong, and the
workaround stops being wanted the day the defect is fixed.

| directory | defect | reduced |
| --- | --- | --- |
| `gcc-exported-using-directive` | g++ does not carry an exported `using namespace` to an importer | yes |
| `gcc-import-cycle-through-own-module` | g++ refuses an implementation unit importing a module whose closure re-enters its own module | yes |
| `clang-mismatched-bmi` | clang segfaults instead of diagnosing when a TU is handed a different BMI for a transitively imported module | yes |
| `gcc-bmi-bad-file-data` | g++ fails to read a BMI cluster ("Bad file data"); separately, `<ostream>` lookup fails when the standard library is textual | **no** |

A reduced entry has a `repro.sh` that exits non-zero on the defect. An
unreduced one is a field note: what was observed, what was already tried and did
not reproduce, and what to try next. It stays here rather than in a scratch
directory so the next attempt starts from what is known.
