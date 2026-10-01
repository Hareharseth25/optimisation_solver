NAME          KNAPSACK
* max 3a + 4b + 5c  s.t. 2a + 3b + 4c <= 5, binary. Optimum 7 at a = b = 1.
OBJSENSE
    MAX
ROWS
 N  OBJ
 L  CAP
COLUMNS
    MARKER                 'MARKER'                 'INTORG'
    A         OBJ       3.0   CAP       2.0
    B         OBJ       4.0   CAP       3.0
    C         OBJ       5.0   CAP       4.0
    MARKER                 'MARKER'                 'INTEND'
RHS
    RHS1      CAP       5.0
BOUNDS
 BV BND       A
 BV BND       B
 BV BND       C
ENDATA
