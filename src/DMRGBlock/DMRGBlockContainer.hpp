#ifndef __DMRG_BLOCK_HPP__
#define __DMRG_BLOCK_HPP__

/**
    @defgroup   DMRGBlockContainer   DMRGBlockContainer
    @brief      Implementation of the DMRGBlockContainer class
    @addtogroup DMRGBlockContainer
    @{ */

#include <petsctime.h>
#include <slepceps.h>
#include <petscmat.h>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "DMRGKron.hpp"

PETSC_EXTERN PetscErrorCode Makedir(const std::string& dir_name);

#if defined(PETSC_USE_DEBUG)
#include <iostream>
#endif

/** Provides an alias of Side_t to follow the Sys-Env convention */
typedef enum
{
    BlockSys = 0,
    BlockEnv = 1
}
Block_t;

/** Storage for information on resulting eigenpairs of the reduced density matrices */
struct Eigen_t
{
    PetscScalar eigval; /**< Eigenvalue */
    PetscInt    seqIdx; /**< Index of the EPS and matrix objects in the vector sequence */
    PetscInt    epsIdx; /**< Index in the EPS object */
    PetscInt    blkIdx; /**< Index in the block's magnetization sectors */
};

/** Comparison operator for sorting Eigen_t objects by decreasing eigenvalues */
bool greater_eigval(const Eigen_t &e1, const Eigen_t &e2) { return e1.eigval > e2.eigval; }

/** Comparison operator for sorting Eigen_t objects by increasing blkIdx (decreasing qn's) */
bool less_blkIdx(const Eigen_t &e1, const Eigen_t &e2) { return e1.blkIdx < e2.blkIdx; }

/** Contains and manipulates the system and environment blocks used in a single DMRG run */
template<class Block, class Hamiltonian> class DMRGBlockContainer
{

public:

    /** Initializes the container object with blocks of one site on each of the system and environment */
    DMRGBlockContainer(const MPI_Comm& mpi_comm): mpi_comm(mpi_comm)
    {
        PetscInt ierr = 0;

        /*  Get MPI attributes */
        ierr = MPI_Comm_size(mpi_comm, &mpi_size); assert(!ierr);
        ierr = MPI_Comm_rank(mpi_comm, &mpi_rank); assert(!ierr);

        /*  Initialize Hamiltonian object */
        ierr = Ham.SetFromOptions(); assert(!ierr);

        /*  Initialize SingleSite which is used as added site */
        ierr = SingleSite.Initialize(mpi_comm, 1, PETSC_DEFAULT); assert(!ierr);

        num_sites = Ham.NumSites();

        if((num_sites) < 2) throw std::runtime_error("There must be at least two total sites.");
        if((num_sites) % 2)  throw std::runtime_error("Total number of sites must be even.");

        /*  Get some info from command line */
        ierr = PetscOptionsGetBool(NULL,NULL,"-verbose",&verbose,NULL); assert(!ierr);
        ierr = PetscOptionsGetBool(NULL,NULL,"-no_symm",&no_symm,NULL); assert(!ierr);

        char path[512];
        PetscBool opt_do_save_dir;
        ierr = PetscOptionsGetString(NULL,NULL,"-save_dir",path,512,&opt_do_save_dir); assert(!ierr);
        do_save_dir = opt_do_save_dir;
        ierr = PetscOptionsGetBool(NULL,NULL,"-do_save_dir",&do_save_dir,NULL); assert(!ierr);
        if(do_save_dir){
            save_dir = std::string(path);
            if(save_dir.back()!='/') save_dir += '/';
        }

        /*  Print some info */
        if(verbose){
            ierr = PetscPrintf(mpi_comm,
                "=========================================\n"
                "DENSITY MATRIX RENORMALIZATION GROUP\n"
                "-----------------------------------------\n"); assert(!ierr);
            if(do_save_dir){
                ierr = PetscPrintf(mpi_comm,
                "Save Directory:     %s\n", save_dir.c_str()); assert(!ierr);
            }
            ierr = PetscPrintf(mpi_comm,
                "=========================================\n"); assert(!ierr);
        }
    }

    /** Destroys all created blocks */
    ~DMRGBlockContainer()
    {
        PetscInt ierr = 0;
        ierr = SingleSite.Destroy(); assert(!ierr);
        for(Block blk: sys_blocks) { ierr = blk.Destroy(); assert(!ierr); }
        for(Block blk: env_blocks) { ierr = blk.Destroy(); assert(!ierr); }
    }

    /** Get parameters from command line options */
    PetscErrorCode SetFromOptions()
    {
        PetscErrorCode ierr;
        ierr = Ham.SetFromOptions(); CHKERRQ(ierr);
        return(0);
    }

    #define PrintLines() printf("-----------------------------------------\n")
    #define PRINTLINES() printf("=========================================\n")
    #define PrintBlocks(LEFT,RIGHT) printf(" [%d]-* *-[%d]\n",(LEFT),(RIGHT))

    /** Returns the path to the directory for the storage of a specific system block */
    std::string BlockDir(const std::string& BlockType, const PetscInt& iblock){
        std::ostringstream oss;
        oss << save_dir << BlockType << "_" << std::setfill('0') << std::setw(9) << iblock;
        return oss.str();
    }

    /** Ensure that required blocks are loaded while unrequired blocks are saved */
    PetscErrorCode SysBlocksActive(const std::set< PetscInt >& SysIdx)
    {
        PetscErrorCode ierr;
        PetscInt sys_idx = 0;
        std::set< PetscInt >::iterator act_it;
        for(act_it = SysIdx.begin(); act_it != SysIdx.end(); ++act_it){
            for(PetscInt idx = sys_idx; idx < *act_it; ++idx){
                // PetscPrintf(mpi_comm,"EnsureSaved:     %d\n",idx);
                ierr = sys_blocks[idx].EnsureSaved(); CHKERRQ(ierr);
            }
            // PetscPrintf(mpi_comm,"EnsureRetrieved: %d\n",*act_it);
            ierr = sys_blocks[*act_it].EnsureRetrieved(); CHKERRQ(ierr);
            sys_idx = *act_it+1;
        }
        for(PetscInt idx = sys_idx; idx < sys_ninit; ++idx){
            // PetscPrintf(mpi_comm,"EnsureSaved:     %d\n",idx);
            ierr = sys_blocks[idx].EnsureSaved(); CHKERRQ(ierr);
        }
        return(0);
    }

    /** Performs the warmup stage of DMRG.
        The system and environment blocks are grown until both reach the maximum number which is half the total number
        of sites. All created system blocks are stored and will be represented by at most `MStates` number of basis states */
    PetscErrorCode Warmup(
        const PetscInt& MStates /**< [in] the maximum number of states to keep after each truncation */
        )
    {
        PetscErrorCode ierr = 0;
        if(warmed_up) SETERRQ(mpi_comm,1,"Warmup has already been called, and it can only be called once.");
        if(!mpi_rank && verbose) printf("WARMUP\n");

        /*  Initialize array of blocks */
        num_sys_blocks = num_sites - 1;
        sys_blocks.resize(num_sys_blocks);

        /*  Initialize directories for saving the block operators */
        if(do_save_dir){
            PetscBool flg;
            ierr = PetscTestDirectory(save_dir.c_str(), 'r', &flg); CHKERRQ(ierr);
            if(!flg) SETERRQ1(mpi_comm,1,"Directory %s does not exist.",save_dir.c_str());
            if(!mpi_rank){
                for(PetscInt iblock = 0; iblock < num_sys_blocks; ++iblock){
                    std::string path = BlockDir("Sys",iblock);
                    ierr = Makedir(path); CHKERRQ(ierr);
                }
            }
            for(PetscInt iblock = 0; iblock < num_sys_blocks; ++iblock){
                std::string path = BlockDir("Sys",iblock);
                ierr = sys_blocks[iblock].Initialize(mpi_comm); CHKERRQ(ierr);
                ierr = sys_blocks[iblock].InitializeSave(path); CHKERRQ(ierr);
            }
        }

        /*  Initialize the 0th system block with one site  */
        ierr = sys_blocks[sys_ninit++].Initialize(mpi_comm, 1, PETSC_DEFAULT); CHKERRQ(ierr);

        /*  Create a set of small but exact initial blocks */
        {
            if(num_sites % 2) SETERRQ1(mpi_comm,1,"Total number of sites must be even. Got %d.", num_sites);
            if(AddSite.NumSites() != 1) SETERRQ1(mpi_comm,1,"Routine assumes an additional site of 1. Got %d.", AddSite.NumSites());

            /*  Number of sites in a single cluster, whose multiples form a full lattice ensuring that the total size is even */
            PetscInt nsites_cluster = Ham.NumEnvSites();
            if (nsites_cluster % 2) nsites_cluster *= 2;

            /*  Prepare an exact representation of blocks of sites incremented up to the cluster size */
            if(!mpi_rank && verbose){
                PrintLines();
                printf(" Preparing initial blocks.\n");
            }
            while(sys_ninit < nsites_cluster){
                PetscInt NumSitesTotal = sys_blocks[sys_ninit-1].NumSites() + AddSite.NumSites();
                ierr = KronEye_Explicit(sys_blocks[sys_ninit-1], AddSite, Ham.H(NumSitesTotal), sys_blocks[sys_ninit]); CHKERRQ(ierr);
                ++sys_ninit;
            }

            #if defined(PETSC_USE_DEBUG)
            {
                if(!mpi_rank && verbose) printf("  sys_ninit: %d\n", sys_ninit);
                for(PetscInt isys = 0; isys < sys_ninit; ++isys){
                    if(!mpi_rank && verbose) printf("   > block %d, num_sites %d\n", isys, sys_blocks[isys].NumSites());
                }
            }
            #endif

            /*  Continuously enlarge the system block until it reaches half the total system size and use the largest
                available environment block that forms a full lattice (multiple of nsites_cluster) */
            while(sys_ninit < num_sites/2)
            {
                PetscInt full_cluster = (((sys_ninit+2) / nsites_cluster)+1) * nsites_cluster;
                PetscInt env_numsites = full_cluster - sys_ninit - 2;

                /* Increment env_numsites up to the highest number of env_blocks available */
                PetscInt env_add = ((sys_ninit - env_numsites) / nsites_cluster) * nsites_cluster;
                env_numsites += env_add;
                full_cluster += env_add;

                if(env_numsites < 1 || env_numsites > sys_ninit)
                    SETERRQ1(mpi_comm,1,"Incorrect number of sites. Got %d.", env_numsites);

                if(!mpi_rank && verbose){
                    PrintLines();
                    PrintBlocks(sys_ninit,env_numsites);
                }
                if(do_save_dir){
                    std::set< PetscInt > SysIdx = {sys_ninit-1, sys_ninit, env_numsites-1, env_numsites};
                    ierr = SysBlocksActive(SysIdx); CHKERRQ(ierr);
                }
                ierr = SingleDMRGStep(
                    sys_blocks[sys_ninit-1],  sys_blocks[env_numsites-1], MStates,
                    sys_blocks[sys_ninit],    sys_blocks[env_numsites]); CHKERRQ(ierr);

                ++sys_ninit;

                #if defined(PETSC_USE_DEBUG)
                    if(!mpi_rank && verbose) printf("  Number of system blocks: %d\n", sys_ninit);
                #endif
            }
        }

        if(sys_ninit != num_sites/2)
            SETERRQ2(mpi_comm,1,"Expected sys_ninit = num_sites/2 = %d. Got %d.",num_sites/2, sys_ninit);
        /* Destroy environment blocks (if any) */
        for(PetscInt ienv = 0; ienv < env_ninit; ++ienv){
            ierr = env_blocks[0].Destroy(); CHKERRQ(ierr);
        }
        env_ninit = 0;
        warmed_up = PETSC_TRUE;

        if(verbose){
            PetscPrintf(mpi_comm,
                "  Initialized system blocks: %d\n"
                "  Target number of sites:    %d\n\n", sys_ninit, num_sites);
            if(!mpi_rank) PRINTLINES();
        }
        return(0);
    }

    /** Performs the sweep stage of DMRG. */
    PetscErrorCode Sweep(
        const PetscInt& MStates, /**< [in] the maximum number of states to keep after each truncation */
        const PetscInt& MinBlock = PETSC_DEFAULT /**< [in] the minimum block length when performing sweeps. Defaults to 1 */
        )
    {
        PetscErrorCode ierr;
        if(!warmed_up) SETERRQ(mpi_comm,1,"Warmup must be called first before performing sweeps.");
        if(!mpi_rank && verbose) printf("SWEEP MStates=%d\n", MStates);

        /*  Set a minimum number of blocks (min_block). Decide whether to set it statically or let
            the number correspond to the least number of sites needed to exactly build MStates. */
        PetscInt min_block = MinBlock==PETSC_DEFAULT ? 1 : MinBlock;
        if(min_block < 1) SETERRQ1(mpi_comm,1,"MinBlock must at least be 1. Got %d.", min_block);

        /*  Starting from the midpoint, perform a center to right sweep */
        for(PetscInt iblock = num_sites/2; iblock < num_sites - min_block - 2; ++iblock)
        {
            const PetscInt  insys  = iblock-1,   inenv  = num_sites - iblock - 3;
            const PetscInt  outsys = iblock,     outenv = num_sites - iblock - 2;
            if(!mpi_rank && verbose){
                PrintLines();
                PrintBlocks(insys+1,inenv+1);
            }
            if(do_save_dir){
                std::set< PetscInt > SysIdx = {insys, outsys, inenv, outenv};
                ierr = SysBlocksActive(SysIdx); CHKERRQ(ierr);
            }
            ierr = SingleDMRGStep(sys_blocks[insys],  sys_blocks[inenv], MStates,
                                    sys_blocks[outsys], sys_blocks[outenv]); CHKERRQ(ierr);
        }

        /*  Since we ASSUME REFLECTION SYMMETRY, the remainder of the sweep can be done as follows:
            Starting from the right-most min_block, perform a right to left sweep up to the MIDPOINT */
        for(PetscInt iblock = min_block; iblock < num_sites/2; ++iblock)
        {
            const PetscInt  insys  = num_sites - iblock - 3,    inenv  = iblock-1;
            const PetscInt  outsys = num_sites - iblock - 2,    outenv = iblock;
            if(!mpi_rank && verbose){
                PrintLines();
                PrintBlocks(insys+1,inenv+1);
            }
            if(do_save_dir){
                std::set< PetscInt > SysIdx = {insys, outsys, inenv, outenv};
                ierr = SysBlocksActive(SysIdx); CHKERRQ(ierr);
            }
            ierr = SingleDMRGStep(sys_blocks[insys],  sys_blocks[inenv], MStates,
                                    sys_blocks[outsys], sys_blocks[outenv]); CHKERRQ(ierr);
        }

        if(!mpi_rank && verbose) PRINTLINES();

        return(0);
    };

    /** Destroys the container object */
    PetscErrorCode Destroy();

    /** Accesses the specified system block */
    const Block& SysBlock(const PetscInt& BlockIdx) const {
        if(BlockIdx >= sys_ninit) throw std::runtime_error("Attempted to access uninitialized system block.");
        return sys_blocks[BlockIdx];
    }

    /** Accesses the specified environment block */
    const Block& EnvBlock(const PetscInt& BlockIdx) const {
        if(BlockIdx >= env_ninit) throw std::runtime_error("Attempted to access uninitialized environment block.");
        return env_blocks[BlockIdx];
    }

    /** Accesses the 0th environment block */
    const Block& EnvBlock() const{ return env_blocks[0]; }

    /** Returns that number of sites recorded in the Hamiltonian object */
    PetscInt NumSites() const { return num_sites; }

private:

    /** MPI Communicator */
    MPI_Comm    mpi_comm = PETSC_COMM_SELF;

    /** MPI rank in mpi_comm */
    PetscMPIInt mpi_rank;

    /** MPI size of mpi_comm */
    PetscMPIInt mpi_size;

    /** Tells whether to printout info during certain function calls */
    PetscBool   verbose = PETSC_FALSE;

    /** Tells whether the object was initialized using Initialize() */
    PetscBool   warmed_up = PETSC_FALSE;

    /** Tells whether no quantum number symmetries will be implemented */
    PetscBool   no_symm = PETSC_FALSE;

    /** Total number of sites */
    PetscInt    num_sites;

    /** Number of system blocks to be stored.
        Usually it is the maximum number of system sites (num_sites - 1) */
    PetscInt    num_sys_blocks;

    /** Number of environment blocks to be stored.
        Usually it is only 1 since the environment block will be re-used */
    PetscInt    num_env_blocks = 1;

    /** Array of system blocks each of which will be kept
        all throughout the simulation */
    std::vector< Block > sys_blocks;

    /** Number of initialized blocks in SysBlocks */
    PetscInt    sys_ninit = 0;

    /** Environment blocks to be used only during warmup.
        For our purposes, this will contain only one block which will
        continuously be enlarged after each iteration */
    std::vector< Block > env_blocks;

    /** Number of initialized blocks in EnvBlocks */
    PetscInt    env_ninit = 0;

    /** Container for the Hamiltonian and geometry */
    Hamiltonian Ham;

    /** Single site that is added to each block
        during the block enlargement procedure */
    Block SingleSite;

    /** Reference to the block of site/s added during enlargement */
    Block& AddSite = SingleSite;

    /** Directory in which the blocks will be saved */
    std::string save_dir = ".";

    /** Tells whether to save and retrieve blocks to reduce memory usage at runtime.
        This is automatically set when indicating -save_dir */
    PetscBool do_save_dir = PETSC_FALSE;

    /** Performs a single DMRG iteration taking in a system and environment block, adding one site
        to each and performing a truncation to at most MStates */
    PetscErrorCode SingleDMRGStep(
        Block& SysBlock,            /**< [in] the old system (left) block */
        Block& EnvBlock,            /**< [in] the old environment (right) block */
        const PetscInt& MStates,    /**< [in] the maximum number of states to keep */
        Block& SysBlockOut,         /**< [out] the new system (left) block */
        Block& EnvBlockOut          /**< [out] the new environment (right) block */
        )
    {
        PetscErrorCode ierr;
        PetscLogDouble t0, tenl, tkron, tdiag, trdm, trot;
        ierr = PetscTime(&t0); CHKERRQ(ierr);

        /* Check whether the system and environment blocks are the same */
        Mat H = nullptr; /* Hamiltonian matrix */
        const PetscBool flg = PetscBool(&SysBlock==&EnvBlock);

        /* (Block) Add one site to each block */
        Block SysBlockEnl, EnvBlockEnl;
        PetscInt NumSitesSysEnl = SysBlock.NumSites() + AddSite.NumSites();
        const std::vector< Hamiltonians::Term > TermsSys = Ham.H(NumSitesSysEnl);
        ierr = KronEye_Explicit(SysBlock, AddSite, TermsSys, SysBlockEnl); CHKERRQ(ierr);
        if(!flg){
            PetscInt NumSitesEnvEnl = EnvBlock.NumSites() + AddSite.NumSites();
            const std::vector< Hamiltonians::Term > TermsEnv = Ham.H(NumSitesEnvEnl);
            ierr = KronEye_Explicit(EnvBlock, AddSite, TermsEnv, EnvBlockEnl); CHKERRQ(ierr);
        } else {
            EnvBlockEnl = SysBlockEnl;
        }

        #if defined(PETSC_USE_DEBUG)
        {
            PetscBool flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_qn",&flg,NULL); CHKERRQ(ierr);
            if(flg){
                /* Print the enlarged system block's quantum numbers for each state */
                ierr = PetscPrintf(mpi_comm,"  SysBlockEnl  "); CHKERRQ(ierr);
                ierr = SysBlockEnl.Magnetization.PrintQNs(); CHKERRQ(ierr);
                ierr = PetscPrintf(mpi_comm,"  EnvBlockEnl  "); CHKERRQ(ierr);
                ierr = EnvBlockEnl.Magnetization.PrintQNs(); CHKERRQ(ierr);
            }
        }
        #endif

        /* Prepare the Hamiltonian taking both enlarged blocks together */
        PetscInt NumSitesTotal = SysBlockEnl.NumSites() + EnvBlockEnl.NumSites();
        const std::vector< Hamiltonians::Term > Terms = Ham.H(NumSitesTotal);

        /* Set the QN sectors as an option */
        std::vector<PetscReal> QNSectors = {0};
        if(no_symm) {
            QNSectors = {};
        }
        KronBlocks_t KronBlocks(SysBlockEnl, EnvBlockEnl, QNSectors);

        #if defined(PETSC_USE_DEBUG)
        {
            PetscBool flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_H_kron",&flg,NULL); CHKERRQ(ierr);
            if(flg && !mpi_rank){
                std::cout << "***** Kron_Explicit *****" << std::endl;
                std::cout << "SysBlockEnl  qn_list:   ";
                for(auto i: SysBlockEnl.Magnetization.List()) std::cout << i << "   ";
                std::cout << std::endl;

                std::cout << "SysBlockEnl  qn_size:   ";
                for(auto i: SysBlockEnl.Magnetization.Sizes()) std::cout << i << "   ";
                std::cout << std::endl;

                std::cout << "SysBlockEnl  qn_offset: ";
                for(auto i: SysBlockEnl.Magnetization.Offsets()) std::cout << i << "   ";
                std::cout << std::endl;

                std::cout << std::endl;

                std::cout << "EnvBlockEnl qn_list:   ";
                for(auto i: EnvBlockEnl.Magnetization.List()) std::cout << i << "   ";
                std::cout << std::endl;

                std::cout << "EnvBlockEnl qn_size:   ";
                for(auto i: EnvBlockEnl.Magnetization.Sizes()) std::cout << i << "   ";
                std::cout << std::endl;

                std::cout << "EnvBlockEnl qn_offset: ";
                for(auto i: EnvBlockEnl.Magnetization.Offsets()) std::cout << i << "   ";
                std::cout << std::endl;

                PetscInt i = 0;
                std::cout << "KronBlocks: \n";
                for(KronBlock_t kb: KronBlocks.data())
                {
                    std::cout << "( "
                        << std::get<0>(kb) << ", "
                        << std::get<1>(kb) << ", "
                        << std::get<2>(kb) << ", "
                        << std::get<3>(kb) << ", "
                        << KronBlocks.Offsets()[i++] <<" )\n";
                }
                std::cout << "*************************" << std::endl;
            }
            if(flg){
                if(!mpi_rank){std::cout << "***** SysBlockEnl *****" << std::endl;}
                for(const Mat& mat: SysBlockEnl.Sz())
                {
                    MatPeek(mat,"Sz");
                }
                for(const Mat& mat: SysBlockEnl.Sp())
                {
                    MatPeek(mat,"Sp");
                }
                if(!mpi_rank){std::cout << "***** EnvBlockEnl *****" << std::endl;}
                for(const Mat& mat: EnvBlockEnl.Sz())
                {
                    MatPeek(mat,"Sz");
                }
                for(const Mat& mat: EnvBlockEnl.Sp())
                {
                    MatPeek(mat,"Sp");
                }
                if(!mpi_rank){std::cout << "***********************" << std::endl;}
            }
        }
        #endif

        ierr = PetscTime(&tenl); CHKERRQ(ierr);
        ierr = KronBlocks.KronSumConstruct(Terms, H); CHKERRQ(ierr);
        ierr = PetscTime(&tkron); CHKERRQ(ierr);

        #if defined(PETSC_USE_DEBUG)
        {
            PetscBool flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_H",&flg,NULL); CHKERRQ(ierr);
            if(flg){ ierr = MatPeek(H,"H"); CHKERRQ(ierr); }
            flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_H_terms",&flg,NULL); CHKERRQ(ierr);
            if(flg){
                if(!mpi_rank) printf(" H(%d)\n", NumSitesTotal);
                for(const Hamiltonians::Term& term: Terms)
                {
                    if(!mpi_rank) printf("%.2f %2s(%2d) %2s(%2d)\n", term.a, (OpString.find(term.Iop)->second).c_str(), term.Isite,
                        (OpString.find(term.Jop)->second).c_str(), term.Jsite );
                }
            }
            ierr = MPI_Barrier(mpi_comm); CHKERRQ(ierr);
        }
        #endif

        /* Solve for the ground state */

        #if defined(PETSC_USE_COMPLEX)
            SETERRQ(mpi_comm,PETSC_ERR_SUP,"This function is only implemented for scalar-type=real.");
            /*  Using both gsv_r and gsv_i but assuming that gsv_i = 0 */
        #endif

        Vec gsv_r, gsv_i;
        PetscScalar gse_r, gse_i;
        ierr = MatCreateVecs(H, &gsv_r, nullptr); CHKERRQ(ierr);
        ierr = MatCreateVecs(H, &gsv_i, nullptr); CHKERRQ(ierr);
        {
            EPS eps;
            ierr = EPSCreate(mpi_comm, &eps); CHKERRQ(ierr);
            ierr = EPSSetOperators(eps, H, nullptr); CHKERRQ(ierr);
            ierr = EPSSetProblemType(eps, EPS_HEP); CHKERRQ(ierr);
            ierr = EPSSetWhichEigenpairs(eps, EPS_SMALLEST_REAL); CHKERRQ(ierr);
            ierr = EPSSetFromOptions(eps); CHKERRQ(ierr);
            ierr = EPSSolve(eps); CHKERRQ(ierr);
            ierr = EPSGetEigenpair(eps, 0, &gse_r, &gse_i, gsv_r, gsv_i); CHKERRQ(ierr);
            ierr = EPSDestroy(&eps); CHKERRQ(ierr);
        }
        ierr = MatDestroy(&H); CHKERRQ(ierr);
        ierr = PetscTime(&tdiag); CHKERRQ(ierr);
        if(!mpi_rank && verbose)
        {
            printf("  NumSites:    %d\n", NumSitesTotal);
            printf("  Energy:      %-10.10g\n", gse_r);
            printf("  Energy/site: %-10.10g\n", gse_r/PetscReal(NumSitesTotal));
        }

        #if defined(PETSC_USE_DEBUG)
        {
            PetscBool flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_H_gs",&flg,NULL); CHKERRQ(ierr);
            if(flg){
                ierr = PetscPrintf(mpi_comm, "\n Ground State Energy: %g + %gj\n", gse_r, gse_i); CHKERRQ(ierr);
                ierr = VecPeek(gsv_r, " gsv_r"); CHKERRQ(ierr);
            }
        }
        #endif

        if(no_symm){
            ierr = MPI_Barrier(mpi_comm); CHKERRQ(ierr);
            SETERRQ(mpi_comm,PETSC_ERR_SUP,"Unsupported option: no_symm.");
        }

        /*  Calculate the reduced density matrices in block-diagonal form, and from this we can calculate the
            (transposed) rotation matrix */
        Mat             RotMatT_L, RotMatT_R;
        QuantumNumbers  QN_L, QN_R;
        PetscReal       TruncErr_L, TruncErr_R;
        ierr = GetTruncation(KronBlocks, gsv_r, MStates, RotMatT_L, QN_L, TruncErr_L, RotMatT_R, QN_R, TruncErr_R); CHKERRQ(ierr);
        /* TODO: Add an option to accept flg for redundant blocks */

        if(!mpi_rank && verbose) printf("  Left  Block Truncation Error: %g\n", TruncErr_L);
        if(!mpi_rank && verbose) printf("  Right Block Truncation Error: %g\n", TruncErr_R);

        ierr = VecDestroy(&gsv_r); CHKERRQ(ierr);
        ierr = VecDestroy(&gsv_i); CHKERRQ(ierr);

        /*  Initialize the new blocks and copy the new blocks */

        /* (Block) Initialize the new blocks
            copy enlarged blocks to out blocks but overwrite the matrices */
        ierr = SysBlockOut.Destroy(); CHKERRQ(ierr);
        ierr = EnvBlockOut.Destroy(); CHKERRQ(ierr);
        ierr = PetscTime(&trdm); CHKERRQ(ierr);


        ierr = SysBlockOut.Initialize(SysBlockEnl.NumSites(), QN_L); CHKERRQ(ierr);
        ierr = SysBlockOut.RotateOperators(SysBlockEnl, RotMatT_L); CHKERRQ(ierr);
        ierr = SysBlockEnl.Destroy(); CHKERRQ(ierr);
        if(!flg){
            ierr = EnvBlockOut.Initialize(EnvBlockEnl.NumSites(), QN_R); CHKERRQ(ierr);
            ierr = EnvBlockOut.RotateOperators(EnvBlockEnl, RotMatT_R); CHKERRQ(ierr);
            ierr = EnvBlockEnl.Destroy(); CHKERRQ(ierr);
        }

        #if defined(PETSC_USE_DEBUG)
        {
            PetscBool flg = PETSC_FALSE;
            ierr = PetscOptionsGetBool(NULL,NULL,"-print_qn",&flg,NULL); CHKERRQ(ierr);
            if(flg){
                /* Print the enlarged system block's quantum numbers for each state */
                ierr = PetscPrintf(mpi_comm,"  SysBlockOut  "); CHKERRQ(ierr);
                ierr = SysBlockOut.Magnetization.PrintQNs(); CHKERRQ(ierr);
                ierr = PetscPrintf(mpi_comm,"  EnvBlockOut  "); CHKERRQ(ierr);
                ierr = EnvBlockOut.Magnetization.PrintQNs(); CHKERRQ(ierr);
            }
        }
        #endif

        ierr = MatDestroy(&RotMatT_L); CHKERRQ(ierr);
        ierr = MatDestroy(&RotMatT_R); CHKERRQ(ierr);
        ierr = PetscTime(&trot); CHKERRQ(ierr);

        PetscLogDouble ttotal = trot - t0;
        ttotal += (ttotal < 0) * 86400.0; /* Just in case it transitions from a previous day */

        if(verbose){
            ierr = PetscPrintf(mpi_comm,"  Total Time (s):  %g\n", trot-t0); CHKERRQ(ierr);
            ierr = PetscPrintf(mpi_comm,"\n"); CHKERRQ(ierr);
        }
        return(0);
    }

    /** Obtain the rotation matrix for the truncation step from the ground state vector */
    PetscErrorCode GetTruncation(
        const KronBlocks_t& KronBlocks, /**< [in] Context for quantum numbers aware Kronecker product */
        const Vec& gsv_r,               /**< [in] Real part of the superblock ground state vector */
        const PetscInt& MStates,        /**< [in] the maximum number of states to keep */
        Mat& RotMatT_L,                 /**< [out] rotation matrix for the system (left) block */
        QuantumNumbers& QN_L,           /**< [out] quantum numbers context for the system (left) block */
        PetscReal& TruncErr_L,          /**< [out] total weights of discarded states for the system (left) block */
        Mat& RotMatT_R,                 /**< [out] rotation matrix for the environment (right) block */
        QuantumNumbers& QN_R,           /**< [out] quantum numbers context for the environment (right) block */
        PetscReal& TruncErr_R           /**< [out] total weights of discarded states for the environment (right) block */
        )
    {
        PetscErrorCode ierr;

        if(no_symm) SETERRQ(mpi_comm,PETSC_ERR_SUP,"Unsupported option: no_symm.");
        #if defined(PETSC_USE_COMPLEX)
            SETERRQ(mpi_comm,PETSC_ERR_SUP,"This function is only implemented for scalar-type=real.");
            /* Error due to re-use of *v buffer for *vT */
        #endif

        /*  Send the whole vector to the root process */
        Vec gsv_r_loc;
        VecScatter ctx;
        ierr = VecScatterCreateToZero(gsv_r, &ctx, &gsv_r_loc); CHKERRQ(ierr);
        ierr = VecScatterBegin(ctx, gsv_r, gsv_r_loc, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);
        ierr = VecScatterEnd(ctx, gsv_r, gsv_r_loc, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);

        #if defined(PETSC_USE_DEBUG)
        PetscBool flg = PETSC_FALSE;
        ierr = PetscOptionsGetBool(NULL,NULL,"-print_trunc",&flg,NULL); CHKERRQ(ierr);
        if(false){
            for(PetscMPIInt irank = 0; irank < mpi_size; ++irank){
                if(irank==mpi_rank){std::cout << "[" << mpi_rank << "]<<" << std::endl;

                    ierr = PetscPrintf(PETSC_COMM_SELF, "gsv_r_loc\n"); CHKERRQ(ierr);
                    ierr = VecView(gsv_r_loc, PETSC_VIEWER_STDOUT_SELF); CHKERRQ(ierr);

                std::cout << ">>[" << mpi_rank << "]" << std::endl;}\
            ierr = MPI_Barrier(mpi_comm); CHKERRQ(ierr);}
        }
        #endif

        std::vector< Eigen_t > eigen_L, eigen_R;        /* Container for eigenvalues of the RDMs */
        std::vector< EPS > eps_list_L, eps_list_R;      /* Container for EPS objects */
        std::vector< Mat > rdmd_list_L, rdmd_list_R;    /* Container for block diagonals of RMDs */
        std::vector< Vec > rdmd_vecs_L, rdmd_vecs_R;    /* Container for the corresponding vectors */

        /*  Do eigendecomposition on root process  */
        if(!mpi_rank)
        {
            /*  Verify the vector length  */
            PetscInt size;
            ierr = VecGetSize(gsv_r_loc, &size);
            if(KronBlocks.NumStates() != size) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect vector length. "
                "Expected %d. Got %d.", KronBlocks.NumStates(), size);

            #if defined(PETSC_USE_DEBUG)
                if(flg) printf("\n\n");
            #endif

            PetscScalar *v;
            ierr = VecGetArray(gsv_r_loc, &v); CHKERRQ(ierr);

            /*  Loop through the L-R pairs forming the target sector in KronBlocks */
            for(PetscInt idx = 0; idx < KronBlocks.size(); ++idx)
            {
                const PetscInt Istart = KronBlocks.Offsets(idx);
                const PetscInt Iend   = KronBlocks.Offsets(idx+1);
                const PetscInt Idx_L  = KronBlocks.LeftIdx(idx);
                const PetscInt Idx_R  = KronBlocks.RightIdx(idx);
                const PetscInt N_L    = KronBlocks.LeftBlockRef().Magnetization.Sizes(Idx_L);
                const PetscInt N_R    = KronBlocks.RightBlockRef().Magnetization.Sizes(Idx_R);

                /*  Verify the segment length */
                if(Iend - Istart != N_L * N_R) SETERRQ2(PETSC_COMM_SELF,1, "Incorrect segment length. "
                    "Expected %d. Got %d.", N_L * N_R, Iend - Istart);

                /*  Initialize and fill sequential dense matrices containing the diagonal blocks of the
                    reduced density matrices */
                Mat Psi, PsiT, rdmd_L, rdmd_R;
                ierr = MatCreateSeqDense(PETSC_COMM_SELF, N_R, N_L, &v[Istart], &PsiT); CHKERRQ(ierr);
                ierr = MatHermitianTranspose(PsiT, MAT_INITIAL_MATRIX, &Psi); CHKERRQ(ierr);
                ierr = MatMatMult(Psi, PsiT, MAT_INITIAL_MATRIX, PETSC_DEFAULT, &rdmd_L);
                ierr = MatMatMult(PsiT, Psi, MAT_INITIAL_MATRIX, PETSC_DEFAULT, &rdmd_R);
                /*  TODO: Check for possible bleeding of memory due to ownership of *v */
                ierr = MatDestroy(&Psi); CHKERRQ(ierr);
                ierr = MatDestroy(&PsiT); CHKERRQ(ierr);

                /*  Verify the sizes of the reduced density matrices */
                {
                    PetscInt Nrows, Ncols;
                    ierr = MatGetSize(rdmd_L, &Nrows, &Ncols); CHKERRQ(ierr);
                    if(Nrows != N_L) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect Nrows in L. Expected %d. Got %d.", N_L, Nrows);
                    if(Ncols != N_L) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect Ncols in L. Expected %d. Got %d.", N_L, Ncols);
                    ierr = MatGetSize(rdmd_R, &Nrows, &Ncols); CHKERRQ(ierr);
                    if(Nrows != N_R) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect Nrows in R. Expected %d. Got %d.", N_R, Nrows);
                    if(Ncols != N_R) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect Ncols in R. Expected %d. Got %d.", N_R, Ncols);
                }

                /*  Solve the full eigenspectrum of the reduced density matrices */
                EPS eps_L, eps_R;
                ierr = EigRDM_BlockDiag(rdmd_L, idx, Idx_L, eigen_L, eps_L); CHKERRQ(ierr);
                ierr = EigRDM_BlockDiag(rdmd_R, idx, Idx_R, eigen_R, eps_R); CHKERRQ(ierr);

                #if defined(PETSC_USE_DEBUG)
                if(flg){
                    printf(" KB QN: %-6g  Left :%3d  Right: %3d\n", KronBlocks.QN(idx), Idx_L, Idx_R)   ;
                    ierr = MatPeek(rdmd_L, "rdmd_L"); CHKERRQ(ierr);
                    ierr = MatPeek(rdmd_R, "rdmd_R"); CHKERRQ(ierr);
                    printf("\n");
                }
                #endif

                eps_list_L.push_back(eps_L);
                eps_list_R.push_back(eps_R);
                rdmd_list_L.push_back(rdmd_L);
                rdmd_list_R.push_back(rdmd_R);

                /*  Prepare the vectors for getting the eigenvectors */
                Vec v_L, v_R;
                ierr = MatCreateVecs(rdmd_L, NULL, &v_L); CHKERRQ(ierr);
                rdmd_vecs_L.push_back(v_L);
                ierr = MatCreateVecs(rdmd_R, NULL, &v_R); CHKERRQ(ierr);
                rdmd_vecs_R.push_back(v_R);
            }

            #if defined(PETSC_USE_DEBUG)
            if(flg){
                printf("\nBefore sorting\n");
                for(const Eigen_t& eig: eigen_L) printf(" L: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n");
                for(const Eigen_t& eig: eigen_R) printf(" R: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n\n");
            }
            #endif

            /*  Sort the eigenvalue lists in descending order */
            std::stable_sort(eigen_L.begin(), eigen_L.end(), greater_eigval);
            std::stable_sort(eigen_R.begin(), eigen_R.end(), greater_eigval);

            #if defined(PETSC_USE_DEBUG)
            if(flg){
                printf("\nAfter sorting\n");
                for(const Eigen_t& eig: eigen_L) printf(" L: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n");
                for(const Eigen_t& eig: eigen_R) printf(" R: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n\n");
            }
            #endif
            ierr = VecRestoreArray(gsv_r_loc, &v); CHKERRQ(ierr);

        }
        /*  Broadcast the number of eigenstates from 0 to all processes */
        PetscInt NEigenStates_L = eigen_L.size(); /* Number of eigenstates in the left block reduced density matrix */
        PetscInt NEigenStates_R = eigen_R.size(); /* Number of eigenstates in the right block reduced density matrix */
        ierr = MPI_Bcast(&NEigenStates_L, 1, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        ierr = MPI_Bcast(&NEigenStates_R, 1, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);

        /*  Decide how many states to retain */
        const PetscInt m_L = PetscMin(MStates, NEigenStates_L);
        const PetscInt m_R = PetscMin(MStates, NEigenStates_R);

        /*  The number of states present in the enlarged blocks */
        const PetscInt NStates_L = KronBlocks.LeftBlockRef().Magnetization.NumStates();
        const PetscInt NStates_R = KronBlocks.RightBlockRef().Magnetization.NumStates();

        /*  The rotation matrices take have the dimension m x NStates so that it is actually*/
        ierr = MatCreate(mpi_comm, &RotMatT_L); CHKERRQ(ierr);
        ierr = MatCreate(mpi_comm, &RotMatT_R); CHKERRQ(ierr);
        ierr = MatSetSizes(RotMatT_L, PETSC_DECIDE, PETSC_DECIDE, m_L, NStates_L); CHKERRQ(ierr);
        ierr = MatSetSizes(RotMatT_R, PETSC_DECIDE, PETSC_DECIDE, m_R, NStates_R); CHKERRQ(ierr);
        ierr = MatSetFromOptions(RotMatT_L); CHKERRQ(ierr);
        ierr = MatSetFromOptions(RotMatT_R); CHKERRQ(ierr);
        ierr = MatSetUp(RotMatT_L); CHKERRQ(ierr);
        ierr = MatSetUp(RotMatT_R); CHKERRQ(ierr);

        #if defined(PETSC_USE_DEBUG)
            if(flg && !mpi_rank) printf("    m_L: %-d  m_R: %-d\n\n", m_L, m_R);
        #endif

        std::vector< PetscReal > qn_list_L, qn_list_R;
        std::vector< PetscInt >  qn_size_L, qn_size_R;
        PetscInt numBlocks_L, numBlocks_R;
        if(!mpi_rank)
        {
            /* Take only the first m states and sort in ascending order of blkIdx */
            eigen_L.resize(m_L);
            eigen_R.resize(m_R);
            std::stable_sort(eigen_L.begin(), eigen_L.end(), less_blkIdx);
            std::stable_sort(eigen_R.begin(), eigen_R.end(), less_blkIdx);

            #if defined(PETSC_USE_DEBUG)
            if(flg) {
                printf("\n\n");
                for(const Eigen_t& eig: eigen_L) printf(" L: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n");
                for(const Eigen_t& eig: eigen_R) printf(" R: %-16.10g seq: %-5d eps: %-5d blk: %-5d\n", eig.eigval, eig.seqIdx, eig.epsIdx, eig.blkIdx);
                printf("\n\n");
            }
            #endif

            /*  Calculate the elements of the rotation matrices and the QN object */
            ierr = FillRotation_BlockDiag(eigen_L, eps_list_L, rdmd_vecs_L, KronBlocks.LeftBlockRef(),  RotMatT_L); CHKERRQ(ierr);
            ierr = FillRotation_BlockDiag(eigen_R, eps_list_R, rdmd_vecs_R, KronBlocks.RightBlockRef(), RotMatT_R); CHKERRQ(ierr);

            /*  Calculate the truncation error */
            TruncErr_L = 1.0;
            for(const Eigen_t &eig: eigen_L) TruncErr_L -= (eig.eigval > 0) * eig.eigval;
            TruncErr_R = 1.0;
            for(const Eigen_t &eig: eigen_R) TruncErr_R -= (eig.eigval > 0) * eig.eigval;

            /*  Calculate the quantum numbers lists */
            {
                std::map< PetscReal, PetscInt > BlockIdxs;
                for(const Eigen_t &eig: eigen_L) BlockIdxs[ eig.blkIdx ] += 1;
                for(const auto& idx: BlockIdxs) qn_list_L.push_back( KronBlocks.LeftBlockRef().Magnetization.List(idx.first) );
                for(const auto& idx: BlockIdxs) qn_size_L.push_back( idx.second );
                numBlocks_L = qn_list_L.size();
            }

            {
                std::map< PetscReal, PetscInt > BlockIdxs;
                for(const Eigen_t &eig: eigen_R) BlockIdxs[ eig.blkIdx ] += 1;
                for(const auto& idx: BlockIdxs) qn_list_R.push_back( KronBlocks.RightBlockRef().Magnetization.List(idx.first) );
                for(const auto& idx: BlockIdxs) qn_size_R.push_back( idx.second );
                numBlocks_R = qn_list_R.size();
            }

            #if defined(PETSC_USE_DEBUG)
            if(flg){
                for(PetscInt i = 0; i < numBlocks_L; ++i) printf("    %g  %d\n", qn_list_L[i], qn_size_L[i]);
                printf("\n");
                for(PetscInt i = 0; i < numBlocks_R; ++i) printf("    %g  %d\n", qn_list_R[i], qn_size_R[i]);
            }
            #endif
        }

        /*  Broadcast the truncation errors to all processes */
        ierr = MPI_Bcast(&TruncErr_L, 1, MPIU_SCALAR, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        ierr = MPI_Bcast(&TruncErr_R, 1, MPIU_SCALAR, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);

        /*  Broadcast the number of quantum blocks */
        ierr = MPI_Bcast(&numBlocks_L, 1, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        ierr = MPI_Bcast(&numBlocks_R, 1, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);

        /*  Broadcast the information on quantum number blocks */
        if(mpi_rank) qn_list_L.resize(numBlocks_L);
        if(mpi_rank) qn_size_L.resize(numBlocks_L);
        ierr = MPI_Bcast(qn_list_L.data(), numBlocks_L, MPIU_REAL, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        ierr = MPI_Bcast(qn_size_L.data(), numBlocks_L, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        if(mpi_rank) qn_list_R.resize(numBlocks_R);
        if(mpi_rank) qn_size_R.resize(numBlocks_R);
        ierr = MPI_Bcast(qn_list_R.data(), numBlocks_R, MPIU_REAL, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);
        ierr = MPI_Bcast(qn_size_R.data(), numBlocks_R, MPIU_INT, 0, PETSC_COMM_WORLD); CHKERRQ(ierr);

        /*  Assemble the rotation matrix */
        ierr = MatAssemblyBegin(RotMatT_L, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
        ierr = MatAssemblyBegin(RotMatT_R, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
        ierr = MatAssemblyEnd(RotMatT_L, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
        ierr = MatAssemblyEnd(RotMatT_R, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);

        #if defined(PETSC_USE_DEBUG)
        if(flg){
            ierr = MatPeek(RotMatT_L, "RotMatT_L"); CHKERRQ(ierr);
            ierr = MatPeek(RotMatT_R, "RotMatT_R"); CHKERRQ(ierr);
        }
        #endif

        ierr = QN_L.Initialize(mpi_comm, qn_list_L, qn_size_L); CHKERRQ(ierr);
        ierr = QN_R.Initialize(mpi_comm, qn_list_R, qn_size_R); CHKERRQ(ierr);

        for(EPS& eps: eps_list_L){
            ierr = EPSDestroy(&eps); CHKERRQ(ierr);
        }
        for(EPS& eps: eps_list_R){
            ierr = EPSDestroy(&eps); CHKERRQ(ierr);
        }
        for(Mat& mat: rdmd_list_L){
            ierr = MatDestroy(&mat); CHKERRQ(ierr);
        }
        for(Mat& mat: rdmd_list_R){
            ierr = MatDestroy(&mat); CHKERRQ(ierr);
        }
        for(Vec& vec: rdmd_vecs_L){
            ierr = VecDestroy(&vec); CHKERRQ(ierr);
        }
        for(Vec& vec: rdmd_vecs_R){
            ierr = VecDestroy(&vec); CHKERRQ(ierr);
        }
        ierr = VecScatterDestroy(&ctx); CHKERRQ(ierr);
        ierr = VecDestroy(&gsv_r_loc); CHKERRQ(ierr);

        return(0);
    }

    /** Obtain the eigenspectrum of a diagonal block of the reduced density matrix through an interface to a lapack routine */
    PetscErrorCode EigRDM_BlockDiag(
        const Mat& matin,                   /**< [in] diagonal block matrix */
        const PetscInt& seqIdx,             /**< [in] sequence index */
        const PetscInt& blkIdx,             /**< [in] block index */
        std::vector< Eigen_t >& eigList,    /**< [out] resulting list of eigenstates */
        EPS& eps                            /**< [out] eigensolver context */
        )
    {
        PetscErrorCode ierr;
        /*  Require that input matrix be square */
        PetscInt Nrows, Ncols;
        ierr = MatGetSize(matin, &Nrows, &Ncols); CHKERRQ(ierr);
        if(Nrows!=Ncols) SETERRQ2(PETSC_COMM_SELF,1,"Input must be square matrix. Got size %d x %d.", Nrows, Ncols);

        ierr = EPSCreate(PETSC_COMM_SELF, &eps); CHKERRQ(ierr);
        ierr = EPSSetOperators(eps, matin, nullptr); CHKERRQ(ierr);
        ierr = EPSSetProblemType(eps, EPS_HEP); CHKERRQ(ierr);
        ierr = EPSSetWhichEigenpairs(eps, EPS_LARGEST_REAL); CHKERRQ(ierr);
        ierr = EPSSetType(eps, EPSLAPACK);
        ierr = EPSSetTolerances(eps, 1.0e-16, PETSC_DEFAULT); CHKERRQ(ierr);
        ierr = EPSSolve(eps); CHKERRQ(ierr);

        /*  Verify convergence */
        PetscInt nconv;
        ierr = EPSGetConverged(eps, &nconv); CHKERRQ(ierr);
        if(nconv != Nrows) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect number of converged eigenpairs. "
            "Expected %d. Got %d.", Nrows, nconv); CHKERRQ(ierr);

        /*  Get the converged eigenvalue */
        for(PetscInt epsIdx = 0; epsIdx < nconv; ++epsIdx)
        {
            PetscScalar eigr=0.0, eigi=0.0;
            ierr = EPSGetEigenvalue(eps, epsIdx, &eigr, &eigi); CHKERRQ(ierr);

            /*  Verify that the eigenvalue is real */
            if(eigi != 0.0) SETERRQ1(PETSC_COMM_SELF,1,"Imaginary part of eigenvalue must be zero. "
                "Got %g\n", eigi);

            eigList.push_back({ eigr, seqIdx, epsIdx, blkIdx });
        }
        return(0);
    }

    /** FIlls the rotation matrix assumming that the reduced density matrix has a block diagonal structure */
    PetscErrorCode FillRotation_BlockDiag(
        const std::vector< Eigen_t >&   eigen_list,     /**< [in] full list of eigenstates */
        const std::vector< EPS >&       eps_list,       /**< [in] ordered list of EPS contexts */
        const std::vector< Vec >&       rdmd_vecs,      /**< [in] ordered list of corresponding eigenvector containers */
        const Block&                    BlockRef,       /**< [in] reference to the block object to get the magnetization */
        Mat&                            RotMatT         /**< [out] resulting rotation matrix */
        )
    {
        PetscErrorCode ierr;

        #if defined(PETSC_USE_COMPLEX)
            SETERRQ(mpi_comm,PETSC_ERR_SUP,"This function is only implemented for scalar-type=real.");
        #endif

        /*  Allocate space for column indices using the maximum size */
        std::vector< PetscInt> qnsize = BlockRef.Magnetization.Sizes();
        std::vector< PetscInt>::iterator it = std::max_element(qnsize.begin(), qnsize.end());
        PetscInt max_qnsize = PetscInt(*it);
        PetscInt *idx;
        ierr = PetscCalloc1(max_qnsize+1, &idx); CHKERRQ(ierr);

        PetscScalar eigr, eigi, *vals;
        PetscInt prev_blkIdx = -1;
        PetscInt startIdx = 0;
        PetscInt numStates = 0;
        PetscInt rowCtr = 0;
        for(const Eigen_t &eig: eigen_list)
        {
            /*  Retrieve the eigenpair from the Eigen_t object */
            const PetscInt seqIdx = eig.seqIdx;
            ierr = EPSGetEigenpair(eps_list[seqIdx], eig.epsIdx, &eigr, &eigi, rdmd_vecs[seqIdx], nullptr); CHKERRQ(ierr);
            ierr = VecGetArray(rdmd_vecs[seqIdx], &vals); CHKERRQ(ierr);
            /*  Verify that eigr is the same eigenvalue as eig.eigval */
            if(eigr != eig.eigval) SETERRQ2(PETSC_COMM_SELF,1,"Incorrect eigenvalue. Expected %g. Got %g.", eig.eigval, eigr);

            /*  Determine the block indices, updating only when the block index changes */
            if(prev_blkIdx != eig.blkIdx){
                startIdx = BlockRef.Magnetization.Offsets(eig.blkIdx); assert(startIdx!=-1);
                numStates = BlockRef.Magnetization.Sizes(eig.blkIdx); assert(numStates!=-1);
                for(PetscInt i = 0; i < numStates+1; ++i) idx[i] = startIdx + i;
                prev_blkIdx = eig.blkIdx;
            }

            /*  Set the value of the rotation matrix to the values of the eigenvector from the root process */
            ierr = MatSetValues(RotMatT, 1, &rowCtr, numStates, idx, vals, INSERT_VALUES); CHKERRQ(ierr);

            ierr = VecRestoreArray(rdmd_vecs[seqIdx], &vals); CHKERRQ(ierr);
            ++rowCtr;
        }
        ierr = PetscFree(idx); CHKERRQ(ierr);
        return(0);
    }

};

/**
    @}
 */

#endif // __DMRG_BLOCK_HPP__
