! <testinfo>
! test_generator="config/mercurium-fortran run"
! </testinfo>
PROGRAM MAIN
  USE, INTRINSIC :: ISO_C_BINDING
  IMPLICIT NONE

  ! SCALAR
  TYPE(C_PTR) :: C_PD, C_PD1
  INTEGER(KIND=C_INT), TARGET :: TAR
  INTEGER(KIND=C_INT), POINTER :: PTR

  ! ARRAY
  TYPE(C_PTR) :: C_PDA
  INTEGER(KIND=C_INT), TARGET :: ARR_TAR(5)
  INTEGER(KIND=C_INT), POINTER :: ARR_PTR(:)

  ! FUNCTION
  INTERFACE
      FUNCTION S(X) BIND(C, NAME="my_name_") RESULT(R)
          USE ISO_C_BINDING
          INTEGER(C_INT) :: X, R
      END FUNCTION S
  END INTERFACE
  TYPE(C_FUNPTR) :: C_PF

  C_PD = C_NULL_PTR
  C_PDA = C_NULL_PTR
  C_PF = C_NULL_FUNPTR

  ! DATA POINTERS TO SCALAR
  TAR = 12
  C_PD = C_LOC(TAR)
  C_PD1 = C_LOC(TAR)

  IF (.NOT. C_ASSOCIATED(C_PD, C_PD1)) STOP 1
  IF (.NOT. C_ASSOCIATED(C_PD)) STOP 2

  CALL C_F_POINTER(C_PD, PTR)
  IF (PTR /= 12) STOP 3
  PRINT *, PTR, TAR

  ! DATA POINTERS TO ARRAYS
  ARR_TAR(:) = 42
  PRINT *, ARR_TAR
  C_PDA = C_LOC(ARR_TAR)
  CALL C_F_POINTER(C_PDA, ARR_PTR, (/ 5 /) )
  IF (ANY(ARR_TAR /= ARR_PTR)) STOP 4
  PRINT *, ARR_PTR

  ! FUNCTION POINTER
  C_PF = C_FUNLOC(S)
  IF (.NOT. C_ASSOCIATED(C_PF)) STOP 5

  PRINT *, C_SIZEOF(X = TAR)
  PRINT *, C_SIZEOF(X = ARR_TAR)

END PROGRAM MAIN

SUBROUTINE my_name(x)
    USE, INTRINSIC :: ISO_C_BINDING
    IMPLICIT NONE
    INTEGER(C_INT) :: X

    PRINT *, X
END SUBROUTINE my_name
