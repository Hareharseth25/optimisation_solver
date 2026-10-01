NAME          STRUCTURED_MILP
* Choose one of three options (set partitioning: Y1 + Y2 + Y3 = 1). Only Y1
* lets X be positive (big-M link X <= 1000 Y1), and X must be at least 2.
* Y2 and Y3 are identical columns (one symmetric group). Optimum 12 at Y1 = 1, X = 2.
ROWS
 N  COST
 E  PICK
 L  LINK
 G  NEED
COLUMNS
    X         COST      1.0       LINK      1.0
    X         NEED      1.0
    MARKER                 'MARKER'                 'INTORG'
    Y1        COST      10.0      PICK      1.0
    Y1        LINK      -1000.0
    Y2        COST      20.0      PICK      1.0
    Y3        COST      20.0      PICK      1.0
    MARKER                 'MARKER'                 'INTEND'
RHS
    RHS       PICK      1.0       LINK      0.0
    RHS       NEED      2.0
BOUNDS
 BV BND       Y1
 BV BND       Y2
 BV BND       Y3
ENDATA
