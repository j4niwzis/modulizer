export module B;

import A;

// B's purview reaches back into A. Nothing here imports B.
export inline int b() { return a(); }
