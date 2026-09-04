export module dep;
#ifdef EXTRA
// Not exported and never referenced; it only perturbs the contents of the BMI
// so that the two builds of this module produce *different* module files.
inline int hidden() { return 42; }
#endif
export struct S { int v = 1; };
