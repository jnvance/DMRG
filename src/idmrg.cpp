#include "idmrg.hpp"
#include "linalg_tools.hpp"


#undef __FUNCT__
#define __FUNCT__ "iDMRG::init"
PetscErrorCode iDMRG::init(MPI_Comm comm, PetscInt nsites, PetscInt mstates)
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);

    comm_ = comm;
    mstates_ = mstates;
    final_nsites_ = nsites;

    /* Initialize block objects */
    ierr = BlockLeft_.init(comm_); CHKERRQ(ierr);
    ierr = BlockRight_.init(comm_); CHKERRQ(ierr);

    /* Initialize single-site operators */
    ierr = MatEyeCreate(comm, eye1_, 2); CHKERRQ(ierr);
    ierr = MatSzCreate(comm, Sz1_); CHKERRQ(ierr);
    ierr = MatSpCreate(comm, Sp1_); CHKERRQ(ierr);
    ierr = MatTranspose(Sp1_, MAT_INITIAL_MATRIX, &Sm1_); CHKERRQ(ierr);

    /*
        Initialize single-site sectors
        TODO: Transfer definition to spin-dependent class
    */
    single_site_sectors = {0.5, -0.5};
    BlockLeft_.basis_sector_array = single_site_sectors;
    BlockRight_.basis_sector_array = single_site_sectors;

    sector_indices = {};

    #ifdef __TESTING
        #define PRINT_VEC(stdvectorpetscscalar) \
            for (std::vector<PetscScalar>::const_iterator i = stdvectorpetscscalar.begin(); \
                i != stdvectorpetscscalar.end(); ++i) printf("%f\n",PetscRealPart(*i)); \
                printf("\n");
    #else
        #define PRINT_VEC(stdvectorpetscscalar)
    #endif

    // PRINT_VEC(single_site_sectors)
    // PRINT_VEC(BlockLeft_.basis_sector_array)
    // PRINT_VEC(BlockRight_.basis_sector_array)

    #undef PRINT_VEC


    /* Initialize log file for timings */
    #ifdef __TIMINGS
        ierr = PetscFOpen(PETSC_COMM_WORLD, "timings.dat", "w", &fp_timings); CHKERRQ(ierr);
    #endif

    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}

#undef __FUNCT__
#define __FUNCT__ "iDMRG::destroy"
PetscErrorCode iDMRG::destroy()
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);
    /*
     * Destroy block objects
     */
    ierr = BlockLeft_.destroy(); CHKERRQ(ierr);
    ierr = BlockRight_.destroy(); CHKERRQ(ierr);
    /*
     * Destroy single-site operators
     */
    LINALG_TOOLS__MATDESTROY(eye1_);
    LINALG_TOOLS__MATDESTROY(Sz1_);
    LINALG_TOOLS__MATDESTROY(Sp1_);
    LINALG_TOOLS__MATDESTROY(Sm1_);
    LINALG_TOOLS__MATDESTROY(superblock_H_);
    /*
     * Close log files after ending timings otherwise,
     * this causes a segmentation fault
     */
    DMRG_TIMINGS_END(__FUNCT__);

    #ifdef __TIMINGS
        ierr = PetscFClose(PETSC_COMM_WORLD, fp_timings); CHKERRQ(ierr);
    #endif

    return ierr;
}

PetscErrorCode iDMRG::SetTargetSz(PetscReal Sz_in, PetscBool do_target_Sz_in)
{
    PetscErrorCode ierr = 0;
    if(target_Sz_set==PETSC_TRUE)
        SETERRQ(comm_,1,"Target Sz has been set.");
    target_Sz = Sz_in;
    do_target_Sz = do_target_Sz_in;
    target_Sz_set=PETSC_TRUE;
    return ierr;
}

#undef __FUNCT__
#define __FUNCT__ "iDMRG::CheckSetParameters"
PetscErrorCode iDMRG::CheckSetParameters()
{
    PetscErrorCode  ierr = 0;

    if (parameters_set == PETSC_FALSE)
    {
        SETERRQ(comm_, 1, "Parameters not yet set.");
    }

    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::SolveGroundState"
PetscErrorCode iDMRG::SolveGroundState(PetscReal& gse_r, PetscReal& gse_i, PetscReal& error)
{
    PetscErrorCode ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);
    DMRG_SUB_TIMINGS_START(__FUNCT__);

    /*
        Checkpoint whether superblock Hamiltonian has been set and assembled
    */
    if (superblock_set_ == PETSC_FALSE)
        SETERRQ(comm_, 1, "Superblock Hamiltonian has not been set with BuildSuperBlock().");

    PetscBool assembled;
    LINALG_TOOLS__MATASSEMBLY_FINAL(superblock_H_);

    /*
        Solve the eigensystem using SLEPC EPS
    */
    EPS eps;

    ierr = EPSCreate(comm_, &eps); CHKERRQ(ierr);
    ierr = EPSSetOperators(eps, superblock_H_, nullptr); CHKERRQ(ierr);
    ierr = EPSSetProblemType(eps, EPS_HEP); CHKERRQ(ierr);
    ierr = EPSSetWhichEigenpairs(eps, EPS_SMALLEST_REAL); CHKERRQ(ierr);
    ierr = EPSSetType(eps, EPSKRYLOVSCHUR); CHKERRQ(ierr);
    ierr = EPSSetDimensions(eps, 1, PETSC_DECIDE, PETSC_DECIDE); CHKERRQ(ierr);

    /*
        If compatible, use previously solved ground state vector as initial guess
     */
    if (gsv_r_ && superblock_H_ && ntruncations_ > 1)
    {
        PetscInt gsv_size, superblock_H_size;

        ierr = VecGetSize(gsv_r_, &gsv_size); CHKERRQ(ierr);
        ierr = MatGetSize(superblock_H_, nullptr, &superblock_H_size); CHKERRQ(ierr);

        if(gsv_size==superblock_H_size){
            ierr = EPSSetInitialSpace(eps, 1, &gsv_r_); CHKERRQ(ierr);
        }
    }

    ierr = EPSSetFromOptions(eps); CHKERRQ(ierr);

    #define __EPS_SOLVE__ "    EPSSolve"
    DMRG_SUB_TIMINGS_START(__EPS_SOLVE__)
    ierr = EPSSolve(eps); CHKERRQ(ierr);
    DMRG_SUB_TIMINGS_END(__EPS_SOLVE__)

    PetscInt nconv;

    ierr = EPSGetConverged(eps,&nconv);CHKERRQ(ierr);
    if (nconv < 1)
        SETERRQ(comm_,1, "EPS did not converge.");

    if (gsv_r_){ ierr = VecDestroy(&gsv_r_); CHKERRQ(ierr); }
    if (gsv_i_){ ierr = VecDestroy(&gsv_i_); CHKERRQ(ierr); }

    ierr = MatCreateVecs(superblock_H_, &gsv_r_, nullptr); CHKERRQ(ierr);

    /* TODO: Verify that this works */
    #if defined(PETSC_USE_COMPLEX)
        gsv_i_ = nullptr;
    #else
        ierr = MatCreateVecs(superblock_H_,&gsv_i_,nullptr); CHKERRQ(ierr);
    #endif

    PetscScalar kr, ki;

    if (nconv>0)
    {
        /*
            Get converged eigenpairs: 0-th eigenvalue is stored in gse_r (real part) and
            gse_i (imaginary part)

            Note on EPSGetEigenpair():

            If the eigenvalue is real, then eigi and Vi are set to zero. If PETSc is configured
            with complex scalars the eigenvalue is stored directly in eigr (eigi is set to zero)
            and the eigenvector in Vr (Vi is set to zero).
        */

        #if defined(PETSC_USE_COMPLEX)
            ierr = EPSGetEigenpair(eps, 0, &kr, &ki, gsv_r_, nullptr); CHKERRQ(ierr);
            gse_r = PetscRealPart(kr);
            gse_i = PetscImaginaryPart(kr);
        #else
            ierr = EPSGetEigenpair(eps, 0, &kr, &ki, gsv_r_, gsv_i_); CHKERRQ(ierr);
            gse_r = kr;
            gse_i = ki;
        #endif

        ierr = EPSComputeError(eps, 0, EPS_ERROR_RELATIVE, &error); CHKERRQ(ierr);
        groundstate_solved_ = PETSC_TRUE;
        // superblock_set_ = PETSC_FALSE; // See note below
    }
    else
    {
        ierr = PetscPrintf(PETSC_COMM_WORLD,"Warning: EPS did not converge."); CHKERRQ(ierr);
    }

    #ifdef __TESTING
        #define __SAVE_SUPERBLOCK
    #endif

    #ifdef __SAVE_SUPERBLOCK
        char filename[PETSC_MAX_PATH_LEN];
        sprintf(filename,"data/superblock_H_%06d.dat",iter());
        ierr = MatWrite(superblock_H_,filename); CHKERRQ(ierr);
        sprintf(filename,"data/gsv_r_%06d.dat",iter());
        ierr = VecWrite(gsv_r_,filename); CHKERRQ(ierr);
        #ifndef PETSC_USE_COMPLEX
            sprintf(filename,"data/gsv_i_%06d.dat",iter());
            ierr = VecWrite(gsv_i_,filename); CHKERRQ(ierr);
        #endif
    #endif // __SAVE_SUPERBLOCK

    /*
        Retain superblock_H_ matrix
        Destroy it only when it is needed to be rebuilt
        Destroy EPS object
    */
    ierr = EPSDestroy(&eps); CHKERRQ(ierr);

    DMRG_SUB_TIMINGS_END(__FUNCT__);
    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::BuildReducedDensityMatrices"
PetscErrorCode iDMRG::BuildReducedDensityMatrices()
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);

    /*
        Determine whether ground state has been solved with SolveGroundState()
     */
    if(groundstate_solved_ == PETSC_FALSE)
        SETERRQ(comm_, 1, "Ground state not yet solved.");

    if (do_target_Sz) {

        /* Clear rho_block_dict for both blocks */
        if(BlockLeft_.rho_block_dict.size())
            for (auto item: BlockLeft_.rho_block_dict)
                MatDestroy(&item.second);

        if(BlockRight_.rho_block_dict.size())
            for (auto item: BlockRight_.rho_block_dict)
                MatDestroy(&item.second);

        BlockLeft_.rho_block_dict.clear();
        BlockRight_.rho_block_dict.clear();

        /* Using VecScatter gather all elements of gsv */
        Vec         vec = gsv_r_;
        Vec         vec_seq;
        VecScatter  ctx;

        ierr = VecScatterCreateToAll(vec, &ctx, &vec_seq); CHKERRQ(ierr);
        ierr = VecScatterBegin(ctx, vec, vec_seq, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);
        ierr = VecScatterEnd(ctx, vec, vec_seq, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);

        PetscInt size_left, size_right, size_right2;

        Mat psi0_sector = nullptr;

        for(auto elem: sector_indices)
        {
            const PetscScalar&      sys_enl_Sz = elem.first;
            const PetscScalar       env_enl_Sz = target_Sz - sys_enl_Sz;
            std::vector<PetscInt>&  indices    = elem.second;

            if(indices.size() > 0)
            {
                auto& sys_enl_basis_by_sector = BlockLeft_.basis_by_sector;
                auto& env_enl_basis_by_sector = BlockRight_.basis_by_sector;
                size_left  = sys_enl_basis_by_sector[sys_enl_Sz].size();
                size_right = indices.size() / size_left;
                size_right2= env_enl_basis_by_sector[target_Sz - sys_enl_Sz].size();

                if(size_right != size_right2)
                    SETERRQ(comm_, 1, "Right block dimension mismatch.");

                if(size_left*size_right != indices.size())
                    SETERRQ(comm_, 1, "Reshape dimension mismatch.");

                ierr = LocalVecReshapeToLocalMat(
                    vec_seq, psi0_sector, size_left, size_right, indices); CHKERRQ(ierr);

                ierr = MatMultSelfHC(psi0_sector, dm_left, PETSC_TRUE); CHKERRQ(ierr);
                ierr = MatMultSelfHC(psi0_sector, dm_right, PETSC_FALSE); CHKERRQ(ierr);

                BlockLeft_.rho_block_dict[sys_enl_Sz] = dm_left;
                BlockRight_.rho_block_dict[env_enl_Sz] = dm_right;

                dm_left = nullptr;
                dm_right = nullptr;

                if(psi0_sector) {
                    ierr = MatDestroy(&psi0_sector); CHKERRQ(ierr);
                    psi0_sector = nullptr;
                }
            }
        }

        ierr = VecScatterDestroy(&ctx); CHKERRQ(ierr);
        ierr = VecDestroy(&vec_seq); CHKERRQ(ierr);

    } else {
        /*
            Collect information regarding the basis size of the
            left and right blocks
         */
        PetscInt size_left, size_right;
        ierr = MatGetSize(BlockLeft_.H(), &size_left, nullptr); CHKERRQ(ierr);
        ierr = MatGetSize(BlockRight_.H(), &size_right, nullptr); CHKERRQ(ierr);

        /*
            Collect entire groundstate vector to all processes
         */
        ierr = VecReshapeToLocalMat(gsv_r_, gsv_mat_seq, size_left, size_right); CHKERRQ(ierr);
        ierr = MatMultSelfHC(gsv_mat_seq, dm_left, PETSC_TRUE); CHKERRQ(ierr);
        ierr = MatMultSelfHC(gsv_mat_seq, dm_right, PETSC_FALSE); CHKERRQ(ierr);

        /*
            Destroy temporary matrices
        */
        if (gsv_mat_seq) MatDestroy(&gsv_mat_seq); gsv_mat_seq = nullptr;

    }

    /*
        Toggle switches
    */
    groundstate_solved_ = PETSC_FALSE;
    dm_solved = PETSC_TRUE;

    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}

/**
    Define the tuple type representing a possible eigenstate
    0 - PetscReal - eigenvalue
    1 - PetscInt - index of the SVD object and the corresponding reduced density matrix
    2 - PetscInt - index of eigenstate in SVD_OBJECT
    3 - PetscScalar - value of Sz_sector acting as key for element 4
    4 - std::vector<PetscInt> - current sector basis from basis by sector
*/
typedef std::tuple<PetscReal, PetscInt, PetscInt, PetscScalar, std::vector<PetscInt>> eigenstate_t;


/** Comparison function for eigenstates in descending order */
bool compare_descending_eigenstates(eigenstate_t a, eigenstate_t b)
{
    return (std::get<0>(a)) > (std::get<0>(b));
}


PetscErrorCode GetRotationMatrices_targetSz(
    const PetscInt mstates,
    DMRGBlock& block,
    Mat& mat,
    PetscReal& truncation_error)
{
    PetscErrorCode ierr = 0;

    /* Get information on MPI */

    PetscMPIInt     nprocs, rank;
    MPI_Comm comm = PETSC_COMM_WORLD;
    MPI_Comm_size(comm, &nprocs);
    MPI_Comm_rank(comm, &rank);

    const std::unordered_map<PetscScalar,std::vector<PetscInt>>&
        sys_enl_basis_by_sector = block.basis_by_sector;

    std::vector< SVD_OBJECT >   svd_list(block.rho_block_dict.size());
    std::vector< Vec >          vec_list(block.rho_block_dict.size());
    std::vector< eigenstate_t > possible_eigenstates;

    /* Diagonalize each block of the reduced density matrix */

    PetscInt counter = 0;
    for (auto elem: block.rho_block_dict)
    {
        /* Keys and values of rho_block_dict */
        PetscScalar         Sz_sector = elem.first;
        Mat&                rho_block = elem.second;

        /* SVD of the reduced density matrices */
        SVD_OBJECT          svd;
        Vec                 Vr;
        PetscScalar         error;
        PetscInt            nconv;

        ierr = MatGetSVD(rho_block, svd, nconv, error, NULL); CHKERRQ(ierr);

        /* Dump svd into map */
        svd_list[counter] = svd;

        /* Create corresponding vector for later use */
        ierr = MatCreateVecs(rho_block, &Vr, nullptr); CHKERRQ(ierr);
        vec_list[counter] = Vr;

        /* Get current sector basis indices */
        std::vector<PetscInt> current_sector_basis = sys_enl_basis_by_sector.at(Sz_sector);

        /* Verify that sizes match */
        PetscInt vec_size;
        ierr = VecGetSize(Vr, &vec_size); CHKERRQ(ierr);
        if(vec_size!=current_sector_basis.size())
            SETERRQ2(comm,1,"Vector size mismatch. Expected %d from current sector basis. Got %d from Vec.",current_sector_basis.size(),vec_size);

        /* Loop through the eigenstates and dump as tuple to vector */
        for (PetscInt svd_id = 0; svd_id < nconv; ++svd_id)
        {
            PetscReal sigma; /* May require PetscReal */
            ierr = SVDGetSingularTriplet(svd, svd_id, &sigma, nullptr, nullptr); CHKERRQ(ierr);
            eigenstate_t tuple(sigma, counter, svd_id, Sz_sector, current_sector_basis);
            possible_eigenstates.push_back(tuple);

        }

        svd = nullptr;
        Vr = nullptr;
        ++counter;
    }

    /* Sort all possible eigenstates in descending order of eigenvalues */
    std::stable_sort(possible_eigenstates.begin(),possible_eigenstates.end(),compare_descending_eigenstates);

    /* Build the transformation matrix from the `m` overall most significant eigenvectors */

    PetscInt my_m = std::min((PetscInt) possible_eigenstates.size(), (PetscInt) mstates);

    /* Create the transformation matrix */

    PetscInt nrows = block.basis_size();
    PetscInt ncols = my_m;
    ierr = MatCreate(PETSC_COMM_WORLD, &mat); CHKERRQ(ierr);
    ierr = MatSetSizes(mat, PETSC_DECIDE, PETSC_DECIDE, nrows, ncols); CHKERRQ(ierr);
    ierr = MatSetFromOptions(mat); CHKERRQ(ierr);
    ierr = MatSetUp(mat); CHKERRQ(ierr);

    /* Guess the local ownership of resultant matrix */

    PetscInt remrows = nrows % nprocs;
    PetscInt locrows = nrows / nprocs;
    PetscInt Istart = locrows * rank;

    if (rank < remrows){
        locrows += 1;
        Istart += rank;
    } else {
        Istart += remrows;
    }

    // PetscInt Iend = Istart + locrows;

    /* FIXME: Preallocate w/ optimization options */

    PetscInt max_nz_rows = locrows;

    /* Prepare buffers */
    PetscInt*       mat_rows;
    PetscScalar*    mat_vals;
    PetscReal sum_sigma = 0.0;

    ierr = PetscMalloc1(max_nz_rows,&mat_rows); CHKERRQ(ierr);
    ierr = PetscMalloc1(max_nz_rows,&mat_vals); CHKERRQ(ierr);

    std::vector<PetscScalar> new_sector_array(my_m);

    /* Loop through eigenstates*/

    for (PetscInt Ieig = 0; Ieig < my_m; ++Ieig)
    {
        /* Unpack tuple */

        eigenstate_t tuple = possible_eigenstates[Ieig];

        PetscReal   sigma     = std::get<0>(tuple);
        PetscInt    block_id  = std::get<1>(tuple);
        PetscInt    svd_id    = std::get<2>(tuple);
        PetscScalar Sz_sector = std::get<3>(tuple);
        std::vector<PetscInt>&  current_sector_basis = std::get<4>(tuple);

        // PetscPrintf(comm, "Sigma: %f\n",sigma);

        sum_sigma += sigma;

        /* Get a copy of the vector's array associated to this process */

        SVD_OBJECT& svd = svd_list[block_id];
        Vec&        Vr  = vec_list[block_id];

        PetscReal sigma_svd;
        ierr = SVDGetSingularTriplet(svd, svd_id, &sigma_svd, Vr, nullptr); CHKERRQ(ierr);
        if(sigma_svd!=sigma)
            SETERRQ2(comm,1,"Eigenvalue mismatch. Expected %f. Got %f.", sigma, sigma_svd);

        /* Get ownership and check sizes */

        PetscInt vec_size, Vstart, Vend;

        ierr = VecGetOwnershipRange(Vr, &Vstart, &Vend);
        // PetscInt Vloc = Vend - Vstart;

        ierr = VecGetSize(Vr,&vec_size); CHKERRQ(ierr);
        if(vec_size!=current_sector_basis.size())
            SETERRQ2(comm,1,"Vector size mismatch. Expected %d. Got %d.",current_sector_basis.size(),vec_size);


        const PetscScalar *vec_vals;
        ierr = VecGetArrayRead(Vr, &vec_vals);

        /* Loop through current_sector_basis and eigenvectors (depending on ownership ) */
        // for (PetscInt Jsec = 0; Jsec < current_sector_basis.size(); ++Jsec)

        /* Inspection */
        // PetscPrintf(comm_,"\n\n\nsigma = %f\n",sigma);
        // PetscPrintf(comm_,"current_sector_basis = ");
        // for (auto& item: current_sector_basis) PetscPrintf(comm,"%d ", item);
        // PetscPrintf(comm_,"\n");
        // VecPeek(Vr, "Vr");

        // MPI_Barrier(PETSC_COMM_WORLD);

        PetscInt nrows_write = 0;
        for (PetscInt Jsec = Vstart; Jsec < Vend; ++Jsec)
        {
            PetscInt j_idx = current_sector_basis[Jsec];

            mat_rows[nrows_write] = j_idx;
            mat_vals[nrows_write] = vec_vals[Jsec-Vstart];

            // printf("[%d] %3d %3d mat_vals = %f\n",rank,Ieig,Jsec,mat_vals[nrows_write]);
            ++nrows_write;
        }

        new_sector_array[Ieig] = Sz_sector;

        /* Set values over one possibly non-local column */
        ierr = MatSetValues(mat, nrows_write, mat_rows, 1, &Ieig, mat_vals, INSERT_VALUES);

        ierr = VecRestoreArrayRead(Vr, &vec_vals);
    }

    /* Output truncation error */
    truncation_error = 1.0 - sum_sigma;

    /* Replace block's sector_array */
    block.basis_sector_array = new_sector_array;

    /* Final assembly of output matrix */
    PetscBool assembled;
    ierr = MatAssembled(mat, &assembled); CHKERRQ(ierr);
    if (assembled == PETSC_FALSE){
        ierr = MatAssemblyBegin(mat, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
        ierr = MatAssemblyEnd(mat, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
    }

    /* Destroy buffers */
    ierr = PetscFree(mat_rows); CHKERRQ(ierr);
    ierr = PetscFree(mat_vals); CHKERRQ(ierr);

    /* Destroy temporary PETSc objects */
    for (auto svd: svd_list){
        ierr = SVDDestroy(&svd); CHKERRQ(ierr);
    }
    for (auto vec: vec_list){
        ierr = VecDestroy(&vec); CHKERRQ(ierr);
    }

    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::GetRotationMatrices"
PetscErrorCode iDMRG::GetRotationMatrices()
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);
    DMRG_SUB_TIMINGS_START(__FUNCT__)

    #define __GET_SVD "    GetSVD"

    if (do_target_Sz) {

        /* Checkpoint */
        if(!dm_solved)
            SETERRQ(comm_, 1, "Reduced density matrices not yet solved.");
        if(BlockLeft_.rho_block_dict.size()==0)
            SETERRQ(comm_, 1, "No density matrices for left block.");
        if(BlockLeft_.rho_block_dict.size()==0)
            SETERRQ(comm_, 1, "No density matrices for right block.");

        DMRG_SUB_TIMINGS_START(__GET_SVD)

        PetscReal truncation_error;
        ierr = GetRotationMatrices_targetSz(mstates_, BlockLeft_, U_left_, truncation_error); CHKERRQ(ierr);
        ierr = GetRotationMatrices_targetSz(mstates_, BlockRight_, U_right_, truncation_error); CHKERRQ(ierr);

        DMRG_SUB_TIMINGS_END(__GET_SVD)

        /* Clear rho_block_dict for both blocks */
        if(BlockLeft_.rho_block_dict.size())
            for (auto item: BlockLeft_.rho_block_dict)
                MatDestroy(&item.second);

        if(BlockRight_.rho_block_dict.size())
            for (auto item: BlockRight_.rho_block_dict)
                MatDestroy(&item.second);

        BlockLeft_.rho_block_dict.clear();
        BlockRight_.rho_block_dict.clear();

    } else {

        if(!(dm_left && dm_right && dm_solved))
            SETERRQ(comm_, 1, "Reduced density matrices not yet solved.");

        PetscScalar trunc_error_left, trunc_error_right;
        FILE *fp_left = nullptr, *fp_right = nullptr;

        #ifdef __TESTING
            char filename[PETSC_MAX_PATH_LEN];
            sprintf(filename,"data/dm_left_singularvalues_%06d.dat",iter());
            ierr = PetscFOpen(PETSC_COMM_WORLD, filename, "w", &fp_left); CHKERRQ(ierr);
            sprintf(filename,"data/dm_right_singularvalues_%06d.dat",iter());
            ierr = PetscFOpen(PETSC_COMM_WORLD, filename, "w", &fp_right); CHKERRQ(ierr);
        #endif

        PetscInt M_left, M_right;
        ierr = MatGetSize(dm_left, &M_left, nullptr); CHKERRQ(ierr);
        ierr = MatGetSize(dm_right, &M_right, nullptr); CHKERRQ(ierr);
        M_left = std::min(M_left, mstates_);
        M_right = std::min(M_right, mstates_);

        DMRG_SUB_TIMINGS_START(__GET_SVD)

        #ifdef __SVD_USE_EPS
            ierr = EPSLargestEigenpairs(dm_left, M_left, trunc_error_left, U_left_,fp_left); CHKERRQ(ierr);
            ierr = EPSLargestEigenpairs(dm_right, M_right, trunc_error_right, U_right_,fp_right); CHKERRQ(ierr);
        #else
            ierr = SVDLargestStates(dm_left, M_left, trunc_error_left, U_left_,fp_left); CHKERRQ(ierr);
            ierr = SVDLargestStates(dm_right, M_right, trunc_error_right, U_right_,fp_right); CHKERRQ(ierr);
        #endif

        DMRG_SUB_TIMINGS_END(__GET_SVD)

        #ifdef __TESTING
            ierr = PetscFClose(PETSC_COMM_WORLD, fp_left); CHKERRQ(ierr);
            ierr = PetscFClose(PETSC_COMM_WORLD, fp_right); CHKERRQ(ierr);
        #endif
    }

    #ifdef __PRINT_TRUNCATION_ERROR
        ierr = PetscPrintf(comm_,
            "%12sTruncation error (left):  %12e\n",
            " ", trunc_error_left);

        ierr = PetscPrintf(comm_,
            "%12sTruncation error (right): %12e\n",
            " ", trunc_error_right); CHKERRQ(ierr);
    #endif

    dm_solved = PETSC_FALSE;
    dm_svd = PETSC_TRUE;

    #ifdef __TESTING
        sprintf(filename,"data/dm_left_%06d.dat",iter());
        ierr = MatWrite(dm_left, filename); CHKERRQ(ierr);
        sprintf(filename,"data/dm_right_%06d.dat",iter());
        ierr = MatWrite(dm_right, filename); CHKERRQ(ierr);
        sprintf(filename,"data/U_left_%06d.dat",iter());
        ierr = MatWrite(U_left_, filename); CHKERRQ(ierr);
        sprintf(filename,"data/U_right_%06d.dat",iter());
        ierr = MatWrite(U_right_, filename); CHKERRQ(ierr);
    #endif

    if (dm_left)   {ierr = MatDestroy(&dm_left); CHKERRQ(ierr);}
    if (dm_right)  {ierr = MatDestroy(&dm_right); CHKERRQ(ierr);}

    DMRG_SUB_TIMINGS_END(__FUNCT__)
    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::TruncateOperators"
PetscErrorCode iDMRG::TruncateOperators()
{
    PetscErrorCode ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);

    /* Save operator state before rotation */
    #ifdef __CHECK_ROTATION
        char filename[PETSC_MAX_PATH_LEN];

        sprintf(filename,"data/H_left_pre_%06d.dat",iter());
        MatWrite(BlockLeft_.H(), filename);

        sprintf(filename,"data/Sz_left_pre_%06d.dat",iter());
        MatWrite(BlockLeft_.Sz(), filename);

        sprintf(filename,"data/Sp_left_pre_%06d.dat",iter());
        MatWrite(BlockLeft_.Sp(), filename);

        sprintf(filename,"data/H_right_pre_%06d.dat",iter());
        MatWrite(BlockRight_.H(), filename);

        sprintf(filename,"data/Sz_right_pre_%06d.dat",iter());
        MatWrite(BlockRight_.Sz(), filename);

        sprintf(filename,"data/Sp_right_pre_%06d.dat",iter());
        MatWrite(BlockRight_.Sp(), filename);

    #endif // __CHECK_ROTATION


    /* Rotation */
    Mat mat_temp = nullptr;
    Mat U_hc = nullptr;

    if(!(dm_svd && U_left_))
        SETERRQ(comm_, 1, "SVD of (LEFT) reduced density matrices not yet solved.");

    ierr = MatHermitianTranspose(U_left_, MAT_INITIAL_MATRIX, &U_hc); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockLeft_.H(), U_left_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockLeft_.update_H(mat_temp); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockLeft_.Sz(), U_left_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockLeft_.update_Sz(mat_temp); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockLeft_.Sp(), U_left_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockLeft_.update_Sp(mat_temp); CHKERRQ(ierr);

    ierr = MatDestroy(&U_hc); CHKERRQ(ierr);


    if(!(dm_svd && U_right_))
        SETERRQ(comm_, 1, "SVD of (RIGHT) reduced density matrices not yet solved.");

    ierr = MatHermitianTranspose(U_right_, MAT_INITIAL_MATRIX, &U_hc); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockRight_.H(), U_right_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockRight_.update_H(mat_temp); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockRight_.Sz(), U_right_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockRight_.update_Sz(mat_temp); CHKERRQ(ierr);

    ierr = MatMatMatMult(U_hc, BlockRight_.Sp(), U_right_, MAT_INITIAL_MATRIX, PETSC_DECIDE, &mat_temp); CHKERRQ(ierr);
    ierr = BlockRight_.update_Sp(mat_temp); CHKERRQ(ierr);

    ierr = MatDestroy(&U_hc); CHKERRQ(ierr);

    if(mat_temp)    {ierr = MatDestroy(&mat_temp); CHKERRQ(ierr);}
    if(U_left_)     {ierr = MatDestroy(&U_left_); CHKERRQ(ierr);}
    if(U_right_)    {ierr = MatDestroy(&U_right_); CHKERRQ(ierr);}

    ntruncations_ += 1;

    /* Save operator state after rotation */

    #ifdef __CHECK_ROTATION
        sprintf(filename,"data/H_left_post_%06d.dat",iter());
        ierr = MatWrite(BlockLeft_.H(), filename); CHKERRQ(ierr);

        sprintf(filename,"data/Sz_left_post_%06d.dat",iter());
        ierr = MatWrite(BlockLeft_.Sz(), filename); CHKERRQ(ierr);

        sprintf(filename,"data/Sp_left_post_%06d.dat",iter());
        ierr = MatWrite(BlockLeft_.Sp(), filename); CHKERRQ(ierr);

        sprintf(filename,"data/H_right_post_%06d.dat",iter());
        ierr = MatWrite(BlockRight_.H(), filename); CHKERRQ(ierr);

        sprintf(filename,"data/Sz_right_post_%06d.dat",iter());
        ierr = MatWrite(BlockRight_.Sz(), filename); CHKERRQ(ierr);

        sprintf(filename,"data/Sp_right_post_%06d.dat",iter());
        ierr = MatWrite(BlockRight_.Sp(), filename); CHKERRQ(ierr);
    #endif // __CHECK_ROTATION
    #undef __CHECK_ROTATION

    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::MatPeekOperators"
PetscErrorCode iDMRG::MatPeekOperators()
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);


    PetscPrintf(comm_, "\nLeft Block Operators\nBlock Length = %d\n", BlockLeft_.length());
    ierr = MatPeek(BlockLeft_.H(), "H (left)");
    ierr = MatPeek(BlockLeft_.Sz(), "Sz (left)");
    ierr = MatPeek(BlockLeft_.Sp(), "Sp (left)");

    PetscPrintf(comm_, "\nRight Block Operators\nBlock Length = %d\n", BlockRight_.length());
    ierr = MatPeek(BlockRight_.H(), "H (right)");
    ierr = MatPeek(BlockRight_.Sz(), "Sz (right)");
    ierr = MatPeek(BlockRight_.Sp(), "Sp (right)");

    if (superblock_H_ && (superblock_set_ == PETSC_TRUE)){
        PetscPrintf(comm_, "\nSuperblock\nBlock Length = %d\n", BlockLeft_.length() + BlockRight_.length());
        ierr = MatPeek(superblock_H_, "H (superblock)"); CHKERRQ(ierr);
    }

    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}


#undef __FUNCT__
#define __FUNCT__ "iDMRG::MatSaveOperators"
PetscErrorCode iDMRG::MatSaveOperators()
{
    PetscErrorCode  ierr = 0;
    DMRG_TIMINGS_START(__FUNCT__);

    char filename[PETSC_MAX_PATH_LEN];
    char extended[PETSC_MAX_PATH_LEN];

    if (superblock_set_==PETSC_TRUE){
        sprintf(extended,"_ext_");
    } else {
        sprintf(extended,"_");
    }

    sprintf(filename,"data/H_left%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockLeft_.H(), filename); CHKERRQ(ierr);

    sprintf(filename,"data/Sz_left%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockLeft_.Sz(), filename); CHKERRQ(ierr);

    sprintf(filename,"data/Sp_left%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockLeft_.Sp(), filename); CHKERRQ(ierr);

    sprintf(filename,"data/H_right%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockRight_.H(), filename); CHKERRQ(ierr);

    sprintf(filename,"data/Sz_right%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockRight_.Sz(), filename); CHKERRQ(ierr);

    sprintf(filename,"data/Sp_right%s%06d.dat",extended,iter());
    ierr = MatWrite(BlockRight_.Sp(), filename); CHKERRQ(ierr);

    if (superblock_H_ && (superblock_set_ == PETSC_TRUE)){
        sprintf(filename,"data/H_superblock_%06d.dat",iter());
        ierr = MatWrite(superblock_H_, filename); CHKERRQ(ierr);
    }

    DMRG_TIMINGS_END(__FUNCT__);
    return ierr;
}
