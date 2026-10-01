NAME          ENGINE_INFEASIBLE
* x + y >= 4, y + z >= 4, x + z >= 4, x + y + z <= 5, all >= 0.
* Summing the first three rows needs x + y + z >= 6, so no point exists --
* but no single row or bound shows it, so presolve passes it to an engine.
ROWS
 N  OBJ
 G  XY
 G  YZ
 G  XZ
 L  SUM
COLUMNS
    X         OBJ       1.0   XY        1.0
    X         XZ        1.0   SUM       1.0
    Y         OBJ       1.0   XY        1.0
    Y         YZ        1.0   SUM       1.0
    Z         OBJ       1.0   YZ        1.0
    Z         XZ        1.0   SUM       1.0
RHS
    RHS       XY        4.0   YZ        4.0
    RHS       XZ        4.0   SUM       5.0
ENDATA
