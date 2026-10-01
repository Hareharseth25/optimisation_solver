NAME          NONCONVEX_QP
* min -x^2 + y  s.t. x + y <= 1: concave in x, so no convex engine applies.
ROWS
 N  OBJ
 L  C1
COLUMNS
    X         C1        1.0
    Y         OBJ       1.0   C1        1.0
RHS
    RHS1      C1        1.0
BOUNDS
 UP BND       X         1.0
QUADOBJ
    X         X        -2.0
ENDATA
