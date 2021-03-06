! <testinfo>
! test_generator=config/mercurium-ompss
! </testinfo>

!! Fortran's version of success_taskwait_depend_03.c, take the description from there!

SUBROUTINE FOO()
    IMPLICIT NONE
    INTEGER :: X, Y

    X = 0
    Y = 2

    !$OMP TASK DEPEND(INOUT: X) SHARED(X)
        X = X + 1
    !$OMP END TASK

    !$OMP TASK DEPEND(IN: X) DEPEND(INOUT: Y) SHARED(X, Y)
        Y = Y - X
    !$OMP END TASK

    !$OMP TASKWAIT DEPEND(INOUT: X)
    IF (X /= 1) STOP -1
    IF (X /= Y) STOP -2
END SUBROUTINE FOO

PROGRAM P
    IMPLICIT NONE
    CALL FOO()
END PROGRAM P
