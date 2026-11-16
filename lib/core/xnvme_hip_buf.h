// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

bool
xnvme_buf_is_hip(const void *buf);

int
xnvme_hip_buf_clear(void *buf, size_t nbytes);

int
xnvme_hip_buf_memcpy(void *dst, const void *src, size_t nbytes);

int
xnvme_hip_buf_diff(const void *expected, const void *actual, size_t nbytes, size_t *diff,
		   bool print);

int
xnvme_hip_buf_fill(void *buf, size_t nbytes, const char *content);

int
xnvme_hip_buf_get_range(void *vaddr, void **base, size_t *nbytes);

/**
 * Export the dma-buf a range of HIP device memory lies in
 *
 * ROCm exports whole buffer objects, and small allocations share one, so the
 * dma-buf can cover more than the range and start before it. An offset into
 * the dma-buf is counted from `base`, not from the range or its allocation.
 *
 * @param vaddr Start of the range
 * @param nbytes Length of the range in bytes
 * @param base Receives the address the dma-buf starts at
 * @param dmabuf_nbytes Receives the size of the dma-buf in bytes
 * @param fd Receives the dma-buf; the caller closes it
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_hip_buf_export_dmabuf(void *vaddr, size_t nbytes, void **base, size_t *dmabuf_nbytes,
			    int *fd);
