! <testinfo>
! test_generator=config/mercurium-ompss
! </testinfo>
MODULE MOO
    INTEGER :: Z
END MODULE MOO

PROGRAM P
    USE MOO, ONLY : Z
    IMPLICIT NONE
    TYPE  MY_TYPE
        INTEGER, ALLOCATABLE :: ARR(:)
    END TYPE  MY_TYPE

    INTERFACE
        !$OMP TASK INOUT(ARR)
        SUBROUTINE FOO(ARR)
            IMPLICIT NONE
            INTEGER, ALLOCATABLE :: ARR(:)
        END SUBROUTINE FOO
    END INTERFACE

    TYPE(MY_TYPE) :: V
    Z = 0

    ALLOCATE(V % ARR(100))
    !$OMP TASK INOUT(V % ARR(:))
        PRINT *, "BEFORE 1"
        CALL SLEEP(1)
        Z = 1
        !$OMP FLUSH
        PRINT *, "AFTER 1"
    !$OMP END TASK

    CALL FOO(V % ARR)

    !$OMP TASKWAIT

    PRINT *, "END"
END PROGRAM P

SUBROUTINE FOO(ARR)
    USE MOO, ONLY : Z
    IMPLICIT NONE
    INTEGER, ALLOCATABLE :: ARR(:)

    PRINT *, "BEFORE 2"
    IF (Z /= 1) THEN
        PRINT *, "INVALID"
        CALL ABORT()
    END IF
    PRINT *, "AFTER 2"
END SUBROUTINE
