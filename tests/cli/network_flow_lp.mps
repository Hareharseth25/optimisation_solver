NAME          NETWORK_FLOW
* Ship 2 units from node 1 to node 3 over arcs 1-2, 2-3, 1-3. Every column has
* exactly one +1 and one -1 (a node-arc incidence matrix). Optimum 4 via node 2.
ROWS
 N  COST
 E  N1
 E  N2
 E  N3
COLUMNS
    X12       COST      1.0       N1        -1.0
    X12       N2        1.0
    X23       COST      1.0       N2        -1.0
    X23       N3        1.0
    X13       COST      3.0       N1        -1.0
    X13       N3        1.0
RHS
    RHS       N1        -2.0      N3        2.0
ENDATA
