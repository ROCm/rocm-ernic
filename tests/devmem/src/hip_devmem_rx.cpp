// hip_devmem_rx: GPU compute memory (hipMalloc) as the devmem TCP Rx buffer.
//
// Copyright (C) Advanced Micro Devices, Inc.
// SPDX-License-Identifier: GPL-2.0-or-later
//
//   hip_devmem_rx [-d DEV] [-c] SIZE -- RECEIVER [receiver args...]
//
// hipMalloc(SIZE), fill it with 0xa5 through HIP (host->device) and read that
// back, export it with hsa_amd_portable_export_dmabuf(), then fork and exec
// RECEIVER (devmem_rx_gpu) with "-E fd -O off -Y r,w -M shmfd" appended. The
// receiver binds the dma-buf for RX. Each time it writes "R" on the gate this
// process copies the WHOLE buffer device->host with hipMemcpy into the shared
// memfd and answers "k"; the receiver verifies the payload there at the
// offsets its cmsgs reported. With -c the copy goes through GPU compute
// instead: a GPU kernel copies the buffer into a second hipMalloc allocation,
// which is then read device->host (the GPU itself read the NIC's bytes).
// After the receiver exits: is the allocation still usable by HIP?
//
// Build: hipcc -O2 -o hip_devmem_rx hip_devmem_rx.cpp -lhsa-runtime64
#include <hip/hip_runtime.h>
#include <hsa/hsa_ext_amd.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

__global__ void copy_kernel(uint8_t *dst, const uint8_t *src, uint64_t size)
{
	uint64_t x = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;

	if (x < size)
		dst[x] = src[x];
}

#define HIPCHK(call)                                                                   \
	do {                                                                           \
		hipError_t e_ = (call);                                                \
		if (e_ != hipSuccess) {                                                \
			fprintf(stderr, "hip: %s failed: %s\n", #call, hipGetErrorString(e_)); \
			return 2;                                                      \
		}                                                                      \
	} while (0)

static double now()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int dev = 0, compute = 0, i = 1;

	for (; i < argc && argv[i][0] == '-' && strcmp(argv[i], "--"); i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c"))
			compute = 1;
	}
	if (argc - i < 3 || strcmp(argv[i + 1], "--")) {
		fprintf(stderr, "usage: %s [-d DEV] [-c] SIZE -- RECEIVER [args]\n", argv[0]);
		return 2;
	}
	uint64_t size = strtoull(argv[i], nullptr, 0);
	char **rx = argv + i + 2;
	int nrx = argc - i - 2;

	HIPCHK(hipInit(0));
	HIPCHK(hipSetDevice(dev));
	hipDeviceProp_t prop;
	HIPCHK(hipGetDeviceProperties(&prop, dev));
	fprintf(stderr, "hip: device %d = %s (%s)\n", dev, prop.name, prop.gcnArchName);

	uint8_t *gpu = nullptr, *gpu2 = nullptr;
	HIPCHK(hipMalloc((void **)&gpu, size));
	if (compute)
		HIPCHK(hipMalloc((void **)&gpu2, size));
	fprintf(stderr, "hip: hipMalloc(%" PRIu64 ") = %p pid %d%s\n", size, (void *)gpu, (int)getpid(),
		compute ? " (+ a second buffer for the GPU-kernel copy)" : "");

	int shm = memfd_create("hip_devmem_rx", 0);
	if (shm < 0 || ftruncate(shm, size)) {
		perror("memfd");
		return 2;
	}
	uint8_t *back = (uint8_t *)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
	if (back == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	{
		std::vector<uint8_t> a5(size, 0xa5);

		HIPCHK(hipMemcpy(gpu, a5.data(), size, hipMemcpyHostToDevice));
		HIPCHK(hipMemcpy(back, gpu, size, hipMemcpyDeviceToHost));
		if (memcmp(back, a5.data(), size)) {
			fprintf(stderr, "hip: 0xa5 pre-fill READBACK MISMATCH\n");
			return 2;
		}
		memset(back, 0, size);
		fprintf(stderr, "hip: pre-filled %" PRIu64 " bytes with 0xa5 (host->device), read back equal\n", size);
	}

	int fd = -1;
	uint64_t off = 0;
	hsa_status_t st = hsa_amd_portable_export_dmabuf(gpu, size, &fd, &off);
	if (st != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hip: hsa_amd_portable_export_dmabuf failed: status %#x\n", (unsigned)st);
		return 3;
	}
	fprintf(stderr, "hip: hsa_amd_portable_export_dmabuf ok: fd=%d offset=%" PRIu64 " dma-buf size=%lld\n",
		fd, off, (long long)lseek(fd, 0, SEEK_END));
	int fl = fcntl(fd, F_GETFD);
	if (fl < 0 || fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC) < 0) {
		perror("fcntl");
		return 2;
	}

	int to_child[2], from_child[2];
	if (pipe(to_child) || pipe(from_child)) {
		perror("pipe");
		return 2;
	}
	std::string sfd = std::to_string(fd), soff = std::to_string(off), sshm = std::to_string(shm),
		    sgate = std::to_string(to_child[0]) + "," + std::to_string(from_child[1]);
	std::vector<char *> av(rx, rx + nrx);
	av.push_back((char *)"-E"); av.push_back((char *)sfd.c_str());
	av.push_back((char *)"-O"); av.push_back((char *)soff.c_str());
	av.push_back((char *)"-Y"); av.push_back((char *)sgate.c_str());
	av.push_back((char *)"-M"); av.push_back((char *)sshm.c_str());
	av.push_back(nullptr);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (!pid) {
		close(to_child[1]);
		close(from_child[0]);
		execv(av[0], av.data());
		perror("execv");
		_exit(127);
	}
	close(to_child[0]);
	close(from_child[1]);
	FILE *in = fdopen(from_child[0], "r");
	char line[64];
	unsigned copies = 0;
	double tcopy = 0;

	while (in && fgets(line, sizeof(line), in)) {
		if (line[0] != 'R')
			continue;
		double t = now();
		hipError_t e;

		if (compute) {
			const unsigned bs = 256;

			hipLaunchKernelGGL(copy_kernel, dim3((unsigned)((size + bs - 1) / bs)), dim3(bs), 0, 0,
					   gpu2, gpu, size);
			e = hipGetLastError();
			if (e == hipSuccess)
				e = hipDeviceSynchronize();
			if (e == hipSuccess)
				e = hipMemcpy(back, gpu2, size, hipMemcpyDeviceToHost);
		} else {
			e = hipMemcpy(back, gpu, size, hipMemcpyDeviceToHost);
		}
		tcopy += now() - t;
		copies++;
		if (e != hipSuccess)
			fprintf(stderr, "hip: readback %u failed: %s\n", copies, hipGetErrorString(e));
		if (write(to_child[1], e == hipSuccess ? "k" : "E", 1) != 1)
			break;
	}
	int wst = 0;
	waitpid(pid, &wst, 0);
	int rc = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst);
	fprintf(stderr, "hip: receiver exit status %d; %u whole-buffer device->host readbacks (%s) while bound, %.2f s\n",
		rc, copies, compute ? "GPU kernel copy, then hipMemcpy" : "hipMemcpy", tcopy);

	// Is the allocation still usable by HIP after the NIC wrote into it?
	{
		std::vector<uint8_t> w(size), r(size, 0);

		for (uint64_t x = 0; x < size; x++)
			w[x] = (uint8_t)(x * 2654435761u >> 13);
		HIPCHK(hipMemcpy(gpu, w.data(), size, hipMemcpyHostToDevice));
		HIPCHK(hipMemcpy(r.data(), gpu, size, hipMemcpyDeviceToHost));
		fprintf(stderr, "hip: after the receive: write through HIP and read back: %s\n",
			memcmp(w.data(), r.data(), size) ? "DIFFERS" : "equal");
	}
	st = hsa_amd_portable_close_dmabuf(fd);
	fprintf(stderr, "hip: hsa_amd_portable_close_dmabuf status %#x\n", (unsigned)st);
	HIPCHK(hipFree(gpu));
	return rc;
}
