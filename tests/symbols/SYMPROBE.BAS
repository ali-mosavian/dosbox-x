' Symbol-loading probe: a global, two procedures with locals, fixed line numbers.
DECLARE SUB Bump (n AS INTEGER)
DECLARE FUNCTION Twice% (v AS INTEGER)
DIM SHARED counter AS INTEGER
counter = 1
PRINT "ready"
WHILE INKEY$ = "": WEND
Bump 5
Bump 7
PRINT "counter="; counter
END

SUB Bump (n AS INTEGER)
    DIM delta AS INTEGER
    delta = Twice%(n)
    counter = counter + delta
END SUB

FUNCTION Twice% (v AS INTEGER)
    Twice% = v * 2
END FUNCTION
