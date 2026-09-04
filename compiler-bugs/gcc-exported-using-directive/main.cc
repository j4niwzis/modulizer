import m;

// Reached through the exported using-directive.
int main() { foo::Thing t{1}; return t.v - 1; }
