! <testinfo>
! test_generator=config/mercurium-ompss
! </testinfo>

MODULE M
IMPLICIT NONE
CONTAINS

SUBROUTINE FOO()
        IMPLICIT NONE
        INTEGER, ALLOCATABLE ::V (:)
        INTEGER :: I

        ALLOCATE(V(10))
        V = -1
        !$OMP DO PRIVATE(V)
        DO I=1, 10
        END DO
END SUBROUTINE FOO


SUBROUTINE FII()
        IMPLICIT NONE
        INTEGER, ALLOCATABLE ::V (:)
        INTEGER :: I

        ALLOCATE(V(10))
        V = -1
        !$OMP DO PRIVATE(V)
        DO I=1, 10
        END DO
END SUBROUTINE FOO
END MODULE M


PROGRAM P
    USE M, ONLY: FOO, FII
    IMPLICIT NONE
    CALL FOO()
    CALL FII()
END PROGRAM P
