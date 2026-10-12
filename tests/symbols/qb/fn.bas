DEFINT A-Z
DECLARE FUNCTION Add (a, b)
DIM SHARED total
DO
LOOP UNTIL INKEY$ <> ""
total = Add(3, 4)
DO
LOOP UNTIL INKEY$ <> ""
END

FUNCTION Add (a, b)
  DIM sum
  sum = a + b
  Add = sum
END FUNCTION
