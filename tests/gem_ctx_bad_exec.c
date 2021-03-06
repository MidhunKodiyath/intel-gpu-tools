/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

/*
 * Negative test cases:
 *  test we can't submit contexts to unsupported rings
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"

/* Copied from gem_exec_nop.c */
static int exec(int fd, uint32_t handle, int ring, int ctx_id)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 gem_exec;
	int ret = 0;

	gem_exec.handle = handle;
	gem_exec.relocation_count = 0;
	gem_exec.relocs_ptr = 0;
	gem_exec.alignment = 0;
	gem_exec.offset = 0;
	gem_exec.flags = 0;
	gem_exec.rsvd1 = 0;
	gem_exec.rsvd2 = 0;

	execbuf.buffers_ptr = (uintptr_t)&gem_exec;
	execbuf.buffer_count = 1;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = 8;
	execbuf.cliprects_ptr = 0;
	execbuf.num_cliprects = 0;
	execbuf.DR1 = 0;
	execbuf.DR4 = 0;
	execbuf.flags = ring;
	i915_execbuffer2_set_context_id(execbuf, ctx_id);
	execbuf.rsvd2 = 0;

	ret = drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2,
			&execbuf);
	gem_sync(fd, handle);

	return ret;
}

uint32_t handle;
uint32_t batch[2] = {MI_BATCH_BUFFER_END};
uint32_t ctx_id;
int fd;

igt_main
{
	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_any_render();

		ctx_id = gem_context_create(fd);

		handle = gem_create(fd, 4096);
		gem_write(fd, handle, 0, batch, sizeof(batch));
	}

	igt_subtest("render")
		igt_assert(exec(fd, handle, I915_EXEC_RENDER, ctx_id) == 0);
	igt_subtest("bsd")
		igt_assert(exec(fd, handle, I915_EXEC_BSD, ctx_id) != 0);
	igt_subtest("blt")
		igt_assert(exec(fd, handle, I915_EXEC_BLT, ctx_id) != 0);
#ifdef I915_EXEC_VEBOX
	igt_fixture
		igt_require(gem_has_vebox(fd));
	igt_subtest("vebox")
		igt_assert(exec(fd, handle, I915_EXEC_VEBOX, ctx_id) != 0);
#endif
}
