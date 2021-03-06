
TARGETS = test_dmrg.x

CXXFLAGS += -O3 -std=c++11 -Wall
# CXXFLAGS += -D SUPERBLOCK_OPTIMIZATION=4
# CXXFLAGS += -D__TESTING
# CXXFLAGS += -D__DMRG_TIMINGS

CXXFLAGS += -D__TIMINGS

CXXFLAGS += -D__DMRG_SUB_TIMINGS
CXXFLAGS += -D__DMRG_SUB_SVD_TIMINGS

CXXFLAGS += -D__KRON_TIMINGS
CXXFLAGS += -D__KRON_PS_TIMINGS

# CXXFLAGS += -D__KRON_TIMINGS_ACCUM
# CXXFLAGS += -D__KRON_PS_TIMINGS_ACCUM

# CXXFLAGS += -D__DMRG_MPI_BARRIERS
# CXXFLAGS += -D__DMRG_MPI_HARD_BARRIERS
CXXFLAGS += -D__DMRG_MPI_BARRIERS_MESSAGE

# CXXFLAGS += -D__LINALG_TOOLS_TIMINGS
# CXXFLAGS += -D__KRON_DENSE_PREALLOCATION
# CXXFLAGS += -D__PRINT_SIZES -D__PRINT_TRUNCATION_ERROR
# CXXFLAGS += -D__PRINT_SVD_CONVERGENCE
# CXXFLAGS += -D__PRINT_SVD_LARGEST
# CXXFLAGS += -D__SVD_USE_EPS
# CXXFLAGS += -D__PRINT_EPS_CONVERGENCE
# CXXFLAGS += -D__PRINT_EPS_LARGEST

TEST_DMRG_DEPS = test_dmrg.o idmrg_1d_heisenberg.o dmrgblock.o kron.o idmrg.o linalg_tools.o
TEST_REBUILD_H_DEPS = test_rebuild_H.o DMRG_Block.o DMRG_Site.o linalg_tools.o kron.o

all: ${TARGETS}

test_dmrg.x: ${TEST_DMRG_DEPS} chkopts
	-${CLINKER} -o test_dmrg.x ${TEST_DMRG_DEPS} ${SLEPC_EPS_LIB}
	${RM} test_dmrg.o

test_rebuild_H.x: ${TEST_REBUILD_H_DEPS} DMRG_Block.cpp chkopts
	-${CLINKER} -o test_rebuild_H.x ${TEST_REBUILD_H_DEPS} ${SLEPC_EPS_LIB}
	${RM} test_rebuild_H.o

test_kron.x: test_kron.o linalg_tools.o kron.o chkopts
	-${CLINKER} -o test_kron.x test_kron.o linalg_tools.o kron.o ${SLEPC_EPS_LIB}
	${RM} test_kron.o

test_reshape.x: test_reshape.o linalg_tools.o chkopts
	-${CLINKER} -o test_reshape.x test_reshape.o linalg_tools.o ${SLEPC_EPS_LIB}
	${RM} test_reshape.o

tests: test_dmrg.x test_kron.x test_reshape.x
	mpirun -np 2 ./test_dmrg.x
	python test_kron.py
	mpirun -np 2 ./test_reshape.x

docs:
	cd .. && doxygen Doxyfile

flush: clean
	${RM} ${TARGETS} *.dat *.dat.info *.x
	${RM} -rf test_kron/*
	${RM} -rf test_reshape/*
	${RM} -rf data/*

include ${SLEPC_DIR}/lib/slepc/conf/slepc_common
