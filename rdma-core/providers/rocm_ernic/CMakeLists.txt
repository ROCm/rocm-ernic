include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES
	${CMAKE_BINARY_DIR}/include
	${CMAKE_SOURCE_DIR}/libibverbs
	${CMAKE_SOURCE_DIR})
check_c_source_compiles("
#include <infiniband/verbs.h>
int main(void) {
    struct ibv_fd_arr a;
    (void)a;
    return 0;
}
" HAVE_IBV_FD_ARR)

rdma_shared_provider(rocm_ernic rocm_ernic.map
	1 1.0.${PACKAGE_VERSION}
	main.c
	verbs.c
	dv.c
)

if(HAVE_IBV_FD_ARR)
	target_compile_definitions(rocm_ernic PRIVATE
		HAVE_IBV_FD_ARR=1)
endif()

publish_headers(infiniband
	rocm_ernic_dv.h
)

rdma_pkg_config("rocm_ernic" "libibverbs"
	"${CMAKE_THREAD_LIBS_INIT}")
