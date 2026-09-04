// An implementation unit of A. It imports B, and B's closure re-enters A --
// which is not a direct self-import, and is what gcc refuses.
module A;

import B;

int a() { return 1; }
