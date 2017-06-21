#ifndef __DMRG_HPP__
#define __DMRG_HPP__

/** @defgroup DMRG

    @brief Implementation of the DMRGBlock and iDMRG classes

 */


#include <slepceps.h>
#include <petscmat.h>
#include "kron.hpp"
#include "matrix_tools.hpp"


#define     DMRGBLOCK_DEFAULT_LENGTH        1
#define     DMRGBLOCK_DEFAULT_BASIS_SIZE    2
#define     DMRG_DEFAULT_MPI_COMM           PETSC_COMM_WORLD

/** @defgroup DMRG

 */
class DMRGBlock
{
    Mat             H_;     // Hamiltonian of entire block
    Mat             Sz_;    // Spin operator of rightmost/leftmost
    Mat             Sp_;

    PetscInt        length_;
    PetscInt        basis_size_;

    MPI_Comm        comm_;

public:

    DMRGBlock() :   length_(DMRGBLOCK_DEFAULT_LENGTH),
                    basis_size_(DMRGBLOCK_DEFAULT_BASIS_SIZE),
                    comm_(DMRG_DEFAULT_MPI_COMM)
                    {}  // constructor with spin 1/2 defaults

    ~DMRGBlock(){}

    // PetscErrorCode  init(); // explicit initializer
    PetscErrorCode  init(MPI_Comm, PetscInt, PetscInt); // explicit initializer
    PetscErrorCode  destroy(); // explicit destructor

    const Mat  H()        {return H_;}
    const Mat  Sz()       {return Sz_;}
    const Mat  Sp()       {return Sp_;}

    PetscErrorCode  update_operators(Mat H_new, Mat Sz_new, Mat Sp_new);

    PetscErrorCode  update_H(Mat H_new);
    PetscErrorCode  update_Sz(Mat Sz_new);
    PetscErrorCode  update_Sp(Mat Sp_new);


    PetscInt        length(){return length_;}
    PetscInt        basis_size(){return basis_size_;}

};


PetscErrorCode DMRGBlock::init( MPI_Comm comm = DMRG_DEFAULT_MPI_COMM,
                                PetscInt length = DMRGBLOCK_DEFAULT_LENGTH,
                                PetscInt basis_size = DMRGBLOCK_DEFAULT_BASIS_SIZE)
{
    PetscErrorCode  ierr = 0;
    comm_ = comm;
    length_ = length;
    basis_size_ = basis_size;

    PetscInt sqmatrixdim = pow(basis_size_,length_);

    // initialize the matrices
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

    // Operators are constructed explicitly in this section
    // For the simple infinite-system DMRG, the calculations begin with a
    // block of 2x2 matrices explictly constructed in 1-2 processor implementations

    // fill the operator values
    // matrix assembly assumes block length = 1, basis_size = 2
    // TODO: generalize!
    if(!(length_==1 && basis_size==2)) SETERRQ(comm,1,"Matrix assembly assumes block length = 1, basis_size = 2\n");
    ierr = MatSetValue(Sz_, 0, 0, +0.5, INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatSetValue(Sz_, 1, 1, -0.5, INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatSetValue(Sp_, 0, 1, +1.0, INSERT_VALUES); CHKERRQ(ierr);

    // #define __PEEK__
    #ifdef __PEEK__
        // Peek into values
        PetscViewer fd = nullptr;
    #define PEEK(mat) \
        ierr = MatAssemblyBegin(mat, MAT_FLUSH_ASSEMBLY); CHKERRQ(ierr); \
        ierr = MatAssemblyEnd(mat, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr); \
        ierr = MatView(mat, fd); CHKERRQ(ierr);

        PetscPrintf(comm, "Sz\n");
        PEEK(Sz_)
        PetscPrintf(comm, "Sp\n");
        PEEK(Sp_)
    #undef PEEK
    #endif


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

    return ierr;
}

PetscErrorCode DMRGBlock::update_operators(Mat H_new, Mat Sz_new, Mat Sp_new)
{
    PetscErrorCode  ierr = 0;
    ierr = update_H(H_new); CHKERRQ(ierr);
    ierr = update_Sz(Sz_new); CHKERRQ(ierr);
    ierr = update_Sp(Sp_new); CHKERRQ(ierr);
    return ierr;
}

#define UPDATE_OPERATOR(MATRIX) \
PetscErrorCode DMRGBlock::update_ ## MATRIX(Mat MATRIX ## _new)         \
{                                                                       \
    PetscErrorCode  ierr = 0;                                           \
    if (MATRIX ## _ == MATRIX ## _new) /* Check whether same matrix */  \
        return ierr;                                                    \
    Mat MATRIX ## _temp = MATRIX ## _;                                  \
    MATRIX ## _ = MATRIX ## _new;                                       \
    ierr = MatDestroy(&MATRIX ## _temp); CHKERRQ(ierr);                 \
    return ierr;                                                        \
}

UPDATE_OPERATOR(H)
UPDATE_OPERATOR(Sz)
UPDATE_OPERATOR(Sp)

#undef UPDATE_OPERATOR


/*--------------------------------------------------------------------------------*/


class iDMRG
{

    DMRGBlock LeftBlock_;
    DMRGBlock RightBlock_;

    MPI_Comm    comm_;

public:

    PetscErrorCode init(MPI_Comm);
    PetscErrorCode destroy();

};


PetscErrorCode iDMRG::init(MPI_Comm comm=DMRG_DEFAULT_MPI_COMM)
{
    PetscErrorCode  ierr = 0;
    comm_ = comm;
    ierr = LeftBlock_.init(comm_); CHKERRQ(ierr);
    ierr = RightBlock_.init(comm_); CHKERRQ(ierr);

    return ierr;
}


PetscErrorCode iDMRG::destroy()
{
    PetscErrorCode  ierr = 0;

    ierr = LeftBlock_.destroy(); CHKERRQ(ierr);
    ierr = RightBlock_.destroy(); CHKERRQ(ierr);

    return ierr;
}


#endif // __DMRG_HPP__
