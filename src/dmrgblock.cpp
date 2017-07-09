#include "dmrgblock.hpp"


PetscErrorCode DMRGBlock::init( MPI_Comm comm, PetscInt length, PetscInt basis_size)
{
    PetscErrorCode  ierr = 0;
    comm_ = comm;
    length_ = length;
    basis_size_ = basis_size;

    PetscInt sqmatrixdim = pow(basis_size_,length_);

    /*
        initialize the matrices
    */
    #define INIT_AND_ZERO(mat) \
        ierr = MatCreate(comm_, &mat); CHKERRQ(ierr); \
        ierr = MatSetSizes(mat, PETSC_DECIDE, PETSC_DECIDE, sqmatrixdim, sqmatrixdim); CHKERRQ(ierr); \
        ierr = MatSetFromOptions(mat); CHKERRQ(ierr); \
        ierr = MatSetUp(mat); CHKERRQ(ierr); \
        ierr = MatZeroEntries(mat); CHKERRQ(ierr);

        INIT_AND_ZERO(H_)
        INIT_AND_ZERO(Sz_)
        INIT_AND_ZERO(Sp_)
    #undef INIT_AND_ZERO

    /*
        Operators are constructed explicitly in this section
        For the simple infinite-system DMRG, the calculations begin with a
        block of 2x2 matrices explictly constructed in 1-2 processor implementations
    */

    /*
        fill the operator values
        matrix assembly assumes block length = 1, basis_size = 2
        TODO: generalize!
    */
    if(!(length_==1 && basis_size==2)) SETERRQ(comm,1,"Matrix assembly assumes block length = 1, basis_size = 2\n");
    ierr = MatSetValue(Sz_, 0, 0, +0.5, INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatSetValue(Sz_, 1, 1, -0.5, INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatSetValue(Sp_, 0, 1, +1.0, INSERT_VALUES); CHKERRQ(ierr);

    return ierr;
}


PetscErrorCode DMRGBlock::destroy()
{
    PetscErrorCode  ierr = 0;

    // All matrices created in init() must be destroyed here
    ierr = MatDestroy(&H_); CHKERRQ(ierr);
    ierr = MatDestroy(&Sz_); CHKERRQ(ierr);
    ierr = MatDestroy(&Sp_); CHKERRQ(ierr);

    length_ = 0;
    basis_size_ = 0;

    H_ = NULL;
    Sz_ = NULL;
    Sp_ = NULL;

    return ierr;
}


PetscErrorCode DMRGBlock::update_operators(const Mat& H_new, const Mat& Sz_new, const Mat& Sp_new)
{
    PetscErrorCode  ierr = 0;
    ierr = update_H(H_new); CHKERRQ(ierr);
    ierr = update_Sz(Sz_new); CHKERRQ(ierr);
    ierr = update_Sp(Sp_new); CHKERRQ(ierr);
    return ierr;
}


// PetscErrorCode DMRGBlock::update_MATRIX(Mat MATRIX_new)
// {
//     PetscErrorCode  ierr = 0;
//     if (MATRIX_ == MATRIX_new)
//         return ierr;
//     Mat MATRIX_temp = MATRIX_;
//     MATRIX_ = MATRIX_new;
//     ierr = MatDestroy(&MATRIX_temp); CHKERRQ(ierr);
//     return ierr;
// }


PetscErrorCode DMRGBlock::update_H(const Mat& H_new)
{
    PetscErrorCode  ierr = 0;
    if (H_ == H_new)
        return ierr;
    Mat H_temp = H_;
    H_ = H_new;
    ierr = MatDestroy(&H_temp); CHKERRQ(ierr);
    return ierr;
}


PetscErrorCode DMRGBlock::update_Sz(const Mat& Sz_new)
{
    PetscErrorCode  ierr = 0;
    if (Sz_ == Sz_new)
        return ierr;
    Mat Sz_temp = Sz_;
    Sz_ = Sz_new;
    ierr = MatDestroy(&Sz_temp); CHKERRQ(ierr);
    return ierr;
}


PetscErrorCode DMRGBlock::update_Sp(const Mat& Sp_new)
{
    PetscErrorCode  ierr = 0;
    if (Sp_ == Sp_new)
        return ierr;
    Mat Sp_temp = Sp_;
    Sp_ = Sp_new;
    ierr = MatDestroy(&Sp_temp); CHKERRQ(ierr);
    return ierr;
}






