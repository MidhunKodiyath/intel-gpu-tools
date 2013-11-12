/*
 * Copyright © 2013 Intel Corporation
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
 *    Paulo Zanoni <paulo.r.zanoni@intel.com>
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "drm.h"
#include "drmtest.h"
#include "intel_batchbuffer.h"
#include "intel_gpu_tools.h"
#include "i915_drm.h"
#include "igt_kms.h"

#define MSR_PC8_RES	0x630
#define MSR_PC9_RES	0x631
#define MSR_PC10_RES	0x632

#define MAX_CONNECTORS	32
#define MAX_ENCODERS	32
#define MAX_CRTCS	16

#define POWER_DIR "/sys/devices/pci0000:00/0000:00:02.0/power"

enum runtime_pm_status {
	RUNTIME_PM_STATUS_ACTIVE,
	RUNTIME_PM_STATUS_SUSPENDED,
	RUNTIME_PM_STATUS_SUSPENDING,
	RUNTIME_PM_STATUS_UNKNOWN,
};

enum screen_type {
	SCREEN_TYPE_LPSP,
	SCREEN_TYPE_NON_LPSP,
	SCREEN_TYPE_ANY,
};

enum residency_wait {
	WAIT,
	DONT_WAIT,
};

int drm_fd, msr_fd, pm_status_fd;
bool has_runtime_pm, has_pc8;
struct mode_set_data ms_data;

/* Stuff used when creating FBs and mode setting. */
struct mode_set_data {
	drmModeResPtr res;
	drmModeConnectorPtr connectors[MAX_CONNECTORS];
	drmModePropertyBlobPtr edids[MAX_CONNECTORS];

	uint32_t devid;
};

/* Stuff we query at different times so we can compare. */
struct compare_data {
	drmModeResPtr res;
	drmModeEncoderPtr encoders[MAX_ENCODERS];
	drmModeConnectorPtr connectors[MAX_CONNECTORS];
	drmModeCrtcPtr crtcs[MAX_CRTCS];
	drmModePropertyBlobPtr edids[MAX_CONNECTORS];
};

struct compare_registers {
	/* We know these are lost */
	uint32_t arb_mode;
	uint32_t tilectl;

	/* Stuff touched at init_clock_gating, so we can make sure we
	 * don't need to call it when reiniting. */
	uint32_t gen6_ucgctl2;
	uint32_t gen7_l3cntlreg1;
	uint32_t transa_chicken1;

	uint32_t deier;
	uint32_t gtier;

	uint32_t ddi_buf_trans_a_1;
	uint32_t ddi_buf_trans_b_5;
	uint32_t ddi_buf_trans_c_10;
	uint32_t ddi_buf_trans_d_15;
	uint32_t ddi_buf_trans_e_20;
};

/* If the read fails, then the machine doesn't support PC8+ residencies. */
static bool supports_pc8_plus_residencies(void)
{
	int rc;
	uint64_t val;

	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC8_RES);
	if (rc != sizeof(val))
		return false;
	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC9_RES);
	if (rc != sizeof(val))
		return false;
	rc = pread(msr_fd, &val, sizeof(uint64_t), MSR_PC10_RES);
	if (rc != sizeof(val))
		return false;

	return true;
}

static uint64_t get_residency(uint32_t type)
{
	int rc;
	uint64_t ret;

	rc = pread(msr_fd, &ret, sizeof(uint64_t), type);
	igt_assert(rc == sizeof(ret));

	return ret;
}

static bool pc8_plus_residency_changed(unsigned int timeout_sec)
{
	unsigned int i;
	uint64_t res_pc8, res_pc9, res_pc10;
	int to_sleep = 100 * 1000;

	res_pc8 = get_residency(MSR_PC8_RES);
	res_pc9 = get_residency(MSR_PC9_RES);
	res_pc10 = get_residency(MSR_PC10_RES);

	for (i = 0; i < timeout_sec * 1000 * 1000; i += to_sleep) {
		if (res_pc8 != get_residency(MSR_PC8_RES) ||
		    res_pc9 != get_residency(MSR_PC9_RES) ||
		    res_pc10 != get_residency(MSR_PC10_RES)) {
			return true;
		}
		usleep(to_sleep);
	}

	return false;
}

/* Checks not only if PC8+ is allowed, but also if we're reaching it.
 * We call this when we expect this function to return quickly since PC8 is
 * actually enabled, so the 30s timeout we use shouldn't matter. */
static bool pc8_plus_enabled(void)
{
	return pc8_plus_residency_changed(30);
}

/* We call this when we expect PC8+ to be actually disabled, so we should not
 * return until the 5s timeout expires. In other words: in the "happy case",
 * every time we call this function the program will take 5s more to finish. */
static bool pc8_plus_disabled(void)
{
	return !pc8_plus_residency_changed(5);
}

static enum runtime_pm_status get_runtime_pm_status(void)
{
	ssize_t n_read;
	char buf[32];

	lseek(pm_status_fd, 0, SEEK_SET);
	n_read = read(pm_status_fd, buf, ARRAY_SIZE(buf));
	igt_assert(n_read >= 0);
	buf[n_read] = '\0';

	if (strncmp(buf, "suspended\n", n_read) == 0)
		return RUNTIME_PM_STATUS_SUSPENDED;
	else if (strncmp(buf, "active\n", n_read) == 0)
		return RUNTIME_PM_STATUS_ACTIVE;
	else if (strncmp(buf, "suspending\n", n_read) == 0)
		return RUNTIME_PM_STATUS_SUSPENDING;

	igt_assert_f(false, "Unknown status %s\n", buf);
	return RUNTIME_PM_STATUS_UNKNOWN;
}

static bool wait_for_pm_status(enum runtime_pm_status status)
{
	int i;
	int hundred_ms = 100 * 1000, ten_s = 10 * 1000 * 1000;

	for (i = 0; i < ten_s; i += hundred_ms) {
		if (get_runtime_pm_status() == status)
			return true;

		usleep(hundred_ms);
	}

	return false;
}

static bool wait_for_suspended(void)
{
	if (has_pc8 && !has_runtime_pm)
		return pc8_plus_enabled();
	else
		return wait_for_pm_status(RUNTIME_PM_STATUS_SUSPENDED);
}

static bool wait_for_active(void)
{
	if (has_pc8 && !has_runtime_pm)
		return pc8_plus_disabled();
	else
		return wait_for_pm_status(RUNTIME_PM_STATUS_ACTIVE);
}

static void disable_all_screens(struct mode_set_data *data)
{
	int i, rc;

	for (i = 0; i < data->res->count_crtcs; i++) {
		rc = drmModeSetCrtc(drm_fd, data->res->crtcs[i], -1, 0, 0,
				    NULL, 0, NULL);
		igt_assert(rc == 0);
	}
}

static uint32_t create_fb(struct mode_set_data *data, int width, int height)
{
	struct kmstest_fb fb;
	cairo_t *cr;
	uint32_t buffer_id;

	buffer_id = kmstest_create_fb(drm_fd, width, height, 32, 24, false,
				      &fb);
	cr = kmstest_get_cairo_ctx(drm_fd, &fb);
	kmstest_paint_test_pattern(cr, width, height);
	return buffer_id;
}

static bool enable_one_screen_with_type(struct mode_set_data *data,
					enum screen_type type)
{
	uint32_t crtc_id = 0, buffer_id = 0, connector_id = 0;
	drmModeModeInfoPtr mode = NULL;
	int i, rc;

	for (i = 0; i < data->res->count_connectors; i++) {
		drmModeConnectorPtr c = data->connectors[i];

		if (type == SCREEN_TYPE_LPSP &&
		    c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		if (type == SCREEN_TYPE_NON_LPSP &&
		    c->connector_type == DRM_MODE_CONNECTOR_eDP)
			continue;

		if (c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			connector_id = c->connector_id;
			mode = &c->modes[0];
			break;
		}
	}

	if (connector_id == 0)
		return false;

	crtc_id = data->res->crtcs[0];
	buffer_id = create_fb(data, mode->hdisplay, mode->vdisplay);

	igt_assert(crtc_id);
	igt_assert(buffer_id);
	igt_assert(connector_id);
	igt_assert(mode);

	rc = drmModeSetCrtc(drm_fd, crtc_id, buffer_id, 0, 0, &connector_id,
			    1, mode);
	igt_assert(rc == 0);

	return true;
}

static void enable_one_screen(struct mode_set_data *data)
{
	igt_assert(enable_one_screen_with_type(data, SCREEN_TYPE_ANY));
}

static drmModePropertyBlobPtr get_connector_edid(drmModeConnectorPtr connector,
						 int index)
{
	unsigned int i;
	drmModeObjectPropertiesPtr props;
	drmModePropertyBlobPtr ret = NULL;

	props = drmModeObjectGetProperties(drm_fd, connector->connector_id,
					   DRM_MODE_OBJECT_CONNECTOR);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(drm_fd,
							     props->props[i]);

		if (strcmp(prop->name, "EDID") == 0) {
			igt_assert(prop->flags & DRM_MODE_PROP_BLOB);
			igt_assert(prop->count_blobs == 0);
			ret = drmModeGetPropertyBlob(drm_fd,
						     props->prop_values[i]);
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
	return ret;
}

static void init_mode_set_data(struct mode_set_data *data)
{
	int i;

	data->res = drmModeGetResources(drm_fd);
	igt_assert(data->res);
	igt_assert(data->res->count_connectors <= MAX_CONNECTORS);

	for (i = 0; i < data->res->count_connectors; i++) {
		data->connectors[i] = drmModeGetConnector(drm_fd,
						data->res->connectors[i]);
		data->edids[i] = get_connector_edid(data->connectors[i], i);
	}

	data->devid = intel_get_drm_devid(drm_fd);

	igt_set_vt_graphics_mode();
}

static void fini_mode_set_data(struct mode_set_data *data)
{
	int i;

	for (i = 0; i < data->res->count_connectors; i++) {
		drmModeFreeConnector(data->connectors[i]);
		drmModeFreePropertyBlob(data->edids[i]);
	}
	drmModeFreeResources(data->res);
}

static void get_drm_info(struct compare_data *data)
{
	int i;

	data->res = drmModeGetResources(drm_fd);
	igt_assert(data->res);

	igt_assert(data->res->count_connectors <= MAX_CONNECTORS);
	igt_assert(data->res->count_encoders <= MAX_ENCODERS);
	igt_assert(data->res->count_crtcs <= MAX_CRTCS);

	for (i = 0; i < data->res->count_connectors; i++) {
		data->connectors[i] = drmModeGetConnector(drm_fd,
						data->res->connectors[i]);
		data->edids[i] = get_connector_edid(data->connectors[i], i);
	}
	for (i = 0; i < data->res->count_encoders; i++)
		data->encoders[i] = drmModeGetEncoder(drm_fd,
						data->res->encoders[i]);
	for (i = 0; i < data->res->count_crtcs; i++)
		data->crtcs[i] = drmModeGetCrtc(drm_fd, data->res->crtcs[i]);
}

static void get_registers(struct compare_registers *data)
{
	intel_register_access_init(intel_get_pci_device(), 0);
	data->arb_mode = INREG(0x4030);
	data->tilectl = INREG(0x101000);
	data->gen6_ucgctl2 = INREG(0x9404);
	data->gen7_l3cntlreg1 = INREG(0xB0C1);
	data->transa_chicken1 = INREG(0xF0060);
	data->deier = INREG(0x4400C);
	data->gtier = INREG(0x4401C);
	data->ddi_buf_trans_a_1 = INREG(0x64E00);
	data->ddi_buf_trans_b_5 = INREG(0x64E70);
	data->ddi_buf_trans_c_10 = INREG(0x64EE0);
	data->ddi_buf_trans_d_15 = INREG(0x64F58);
	data->ddi_buf_trans_e_20 = INREG(0x64FCC);
	intel_register_access_fini();
}

static void free_drm_info(struct compare_data *data)
{
	int i;

	for (i = 0; i < data->res->count_connectors; i++) {
		drmModeFreeConnector(data->connectors[i]);
		drmModeFreePropertyBlob(data->edids[i]);
	}
	for (i = 0; i < data->res->count_encoders; i++)
		drmModeFreeEncoder(data->encoders[i]);
	for (i = 0; i < data->res->count_crtcs; i++)
		drmModeFreeCrtc(data->crtcs[i]);

	drmModeFreeResources(data->res);
}

#define COMPARE(d1, d2, data) igt_assert(d1->data == d2->data)
#define COMPARE_ARRAY(d1, d2, size, data) do { \
	for (i = 0; i < size; i++) \
		igt_assert(d1->data[i] == d2->data[i]); \
} while (0)

static void assert_drm_resources_equal(struct compare_data *d1,
				       struct compare_data *d2)
{
	COMPARE(d1, d2, res->count_connectors);
	COMPARE(d1, d2, res->count_encoders);
	COMPARE(d1, d2, res->count_crtcs);
	COMPARE(d1, d2, res->min_width);
	COMPARE(d1, d2, res->max_width);
	COMPARE(d1, d2, res->min_height);
	COMPARE(d1, d2, res->max_height);
}

static void assert_modes_equal(drmModeModeInfoPtr m1, drmModeModeInfoPtr m2)
{
	COMPARE(m1, m2, clock);
	COMPARE(m1, m2, hdisplay);
	COMPARE(m1, m2, hsync_start);
	COMPARE(m1, m2, hsync_end);
	COMPARE(m1, m2, htotal);
	COMPARE(m1, m2, hskew);
	COMPARE(m1, m2, vdisplay);
	COMPARE(m1, m2, vsync_start);
	COMPARE(m1, m2, vsync_end);
	COMPARE(m1, m2, vtotal);
	COMPARE(m1, m2, vscan);
	COMPARE(m1, m2, vrefresh);
	COMPARE(m1, m2, flags);
	COMPARE(m1, m2, type);
	igt_assert(strcmp(m1->name, m2->name) == 0);
}

static void assert_drm_connectors_equal(drmModeConnectorPtr c1,
					drmModeConnectorPtr c2)
{
	int i;

	COMPARE(c1, c2, connector_id);
	COMPARE(c1, c2, connector_type);
	COMPARE(c1, c2, connector_type_id);
	COMPARE(c1, c2, mmWidth);
	COMPARE(c1, c2, mmHeight);
	COMPARE(c1, c2, count_modes);
	COMPARE(c1, c2, count_props);
	COMPARE(c1, c2, count_encoders);
	COMPARE_ARRAY(c1, c2, c1->count_props, props);
	COMPARE_ARRAY(c1, c2, c1->count_encoders, encoders);

	for (i = 0; i < c1->count_modes; i++)
		assert_modes_equal(&c1->modes[0], &c2->modes[0]);
}

static void assert_drm_encoders_equal(drmModeEncoderPtr e1,
				      drmModeEncoderPtr e2)
{
	COMPARE(e1, e2, encoder_id);
	COMPARE(e1, e2, encoder_type);
	COMPARE(e1, e2, possible_crtcs);
	COMPARE(e1, e2, possible_clones);
}

static void assert_drm_crtcs_equal(drmModeCrtcPtr c1, drmModeCrtcPtr c2)
{
	COMPARE(c1, c2, crtc_id);
}

static void assert_drm_edids_equal(drmModePropertyBlobPtr e1,
				   drmModePropertyBlobPtr e2)
{
	if (!e1 && !e2)
		return;
	igt_assert(e1 && e2);

	COMPARE(e1, e2, id);
	COMPARE(e1, e2, length);

	igt_assert(memcmp(e1->data, e2->data, e1->length) == 0);
}

static void compare_registers(struct compare_registers *d1,
			      struct compare_registers *d2)
{
	COMPARE(d1, d2, gen6_ucgctl2);
	COMPARE(d1, d2, gen7_l3cntlreg1);
	COMPARE(d1, d2, transa_chicken1);
	COMPARE(d1, d2, arb_mode);
	COMPARE(d1, d2, tilectl);
	COMPARE(d1, d2, arb_mode);
	COMPARE(d1, d2, tilectl);
	COMPARE(d1, d2, gtier);
	COMPARE(d1, d2, ddi_buf_trans_a_1);
	COMPARE(d1, d2, ddi_buf_trans_b_5);
	COMPARE(d1, d2, ddi_buf_trans_c_10);
	COMPARE(d1, d2, ddi_buf_trans_d_15);
	COMPARE(d1, d2, ddi_buf_trans_e_20);
}

static void assert_drm_infos_equal(struct compare_data *d1,
				   struct compare_data *d2)
{
	int i;

	assert_drm_resources_equal(d1, d2);

	for (i = 0; i < d1->res->count_connectors; i++) {
		assert_drm_connectors_equal(d1->connectors[i],
					    d2->connectors[i]);
		assert_drm_edids_equal(d1->edids[i], d2->edids[i]);
	}

	for (i = 0; i < d1->res->count_encoders; i++)
		assert_drm_encoders_equal(d1->encoders[i], d2->encoders[i]);

	for (i = 0; i < d1->res->count_crtcs; i++)
		assert_drm_crtcs_equal(d1->crtcs[i], d2->crtcs[i]);
}

/* We could check the checksum too, but just the header is probably enough. */
static bool edid_is_valid(const unsigned char *edid)
{
	char edid_header[] = {
		0x0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0,
	};

	return (memcmp(edid, edid_header, sizeof(edid_header)) == 0);
}

static int count_drm_valid_edids(struct mode_set_data *data)
{
	int i, ret = 0;

	for (i = 0; i < data->res->count_connectors; i++)
		if (data->edids[i] && edid_is_valid(data->edids[i]->data))
			ret++;
	return ret;
}

static bool i2c_edid_is_valid(int fd)
{
	int rc;
	unsigned char edid[128] = {};
	struct i2c_msg msgs[] = {
		{ /* Start at 0. */
			.addr = 0x50,
			.flags = 0,
			.len = 1,
			.buf = edid,
		}, { /* Now read the EDID. */
			.addr = 0x50,
			.flags = I2C_M_RD,
			.len = 128,
			.buf = edid,
		}
	};
	struct i2c_rdwr_ioctl_data msgset = {
		.msgs = msgs,
		.nmsgs = 2,
	};

	rc = ioctl(fd, I2C_RDWR, &msgset);
	return (rc >= 0) ? edid_is_valid(edid) : false;
}

static int count_i2c_valid_edids(void)
{
	int fd, ret = 0;
	DIR *dir;

	struct dirent *dirent;
	char full_name[32];

	dir = opendir("/dev/");
	igt_assert(dir);

	while ((dirent = readdir(dir))) {
		if (strncmp(dirent->d_name, "i2c-", 4) == 0) {
			snprintf(full_name, 32, "/dev/%s", dirent->d_name);
			fd = open(full_name, O_RDWR);
			igt_assert(fd != -1);
			if (i2c_edid_is_valid(fd))
				ret++;
			close(fd);
		}
	}

	closedir(dir);

	return ret;
}

static void test_i2c(struct mode_set_data *data)
{
	int i2c_edids = count_i2c_valid_edids();
	int drm_edids = count_drm_valid_edids(data);

	igt_assert(i2c_edids == drm_edids);
}

static void setup_runtime_pm(void)
{
	int fd;
	ssize_t size;
	char buf[6];

	/* Our implementation uses autosuspend. Try to set it to 0ms so the test
	 * suite goes faster and we have a higher probability of triggering race
	 * conditions. */
	fd = open(POWER_DIR "/autosuspend_delay_ms", O_WRONLY);
	igt_assert_f(fd >= 0,
		     "Can't open " POWER_DIR "/autosuspend_delay_ms\n");

	/* If we fail to write to the file, it means this system doesn't support
	 * runtime PM. */
	size = write(fd, "0\n", 2);
	has_runtime_pm = (size == 2);

	close(fd);

	if (!has_runtime_pm)
		return;

	/* We know we support runtime PM, let's try to enable it now. */
	fd = open(POWER_DIR "/control", O_RDWR);
	igt_assert_f(fd >= 0, "Can't open " POWER_DIR "/control\n");

	size = write(fd, "auto\n", 5);
	igt_assert(size == 5);

	lseek(fd, 0, SEEK_SET);
	size = read(fd, buf, ARRAY_SIZE(buf));
	igt_assert(size == 5);
	igt_assert(strncmp(buf, "auto\n", 5) == 0);

	close(fd);

	pm_status_fd = open(POWER_DIR "/runtime_status", O_RDONLY);
	igt_assert_f(pm_status_fd >= 0,
		     "Can't open " POWER_DIR "/runtime_status\n");
}

static void setup_pc8(void)
{
	has_pc8 = false;

	/* Only Haswell supports the PC8 feature. */
	if (!IS_HASWELL(ms_data.devid))
		return;

	/* Make sure our Kernel supports MSR and the module is loaded. */
	msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
	igt_assert_f(msr_fd >= 0,
		     "Can't open /dev/cpu/0/msr.\n");

	/* Non-ULT machines don't support PC8+. */
	if (!supports_pc8_plus_residencies())
		return;

	has_pc8 = true;
}

static void setup_environment(void)
{
	drm_fd = drm_open_any();
	igt_assert(drm_fd >= 0);

	init_mode_set_data(&ms_data);

	setup_runtime_pm();
	setup_pc8();

	printf("Runtime PM support: %d\n", has_runtime_pm);
	printf("PC8 residency support: %d\n", has_pc8);

	igt_require(has_runtime_pm || has_pc8);
}

static void teardown_environment(void)
{
	fini_mode_set_data(&ms_data);
	drmClose(drm_fd);
	close(msr_fd);
	if (has_runtime_pm)
		close(pm_status_fd);
}

static void basic_subtest(void)
{
	/* Make sure PC8+ residencies move! */
	disable_all_screens(&ms_data);
	igt_assert_f(pc8_plus_enabled(),
		     "Machine is not reaching PC8+ states, please check its "
		     "configuration.\n");

	/* Make sure PC8+ residencies stop! */
	enable_one_screen(&ms_data);
	igt_assert_f(pc8_plus_disabled(),
		     "PC8+ residency didn't stop with screen enabled.\n");
}

static void modeset_subtest(enum screen_type type, int rounds,
			    enum residency_wait wait)
{
	int i;

	for (i = 0; i < rounds; i++) {
		disable_all_screens(&ms_data);
		if (wait == WAIT)
			igt_assert(wait_for_suspended());

		/* If we skip this line it's because the type of screen we want
		 * is not connected. */
		igt_require(enable_one_screen_with_type(&ms_data, type));
		if (wait == WAIT)
			igt_assert(wait_for_active());
	}
}

/* Test of the DRM resources reported by the IOCTLs are still the same. This
 * ensures we still see the monitors with the same eyes. We get the EDIDs and
 * compare them, which ensures we use DP AUX or GMBUS depending on what's
 * connected. */
static void drm_resources_equal_subtest(void)
{
	struct compare_data pre_pc8, during_pc8, post_pc8;

	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	get_drm_info(&pre_pc8);
	igt_assert(wait_for_active());

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());
	get_drm_info(&during_pc8);
	igt_assert(wait_for_suspended());

	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	get_drm_info(&post_pc8);
	igt_assert(wait_for_active());

	assert_drm_infos_equal(&pre_pc8, &during_pc8);
	assert_drm_infos_equal(&pre_pc8, &post_pc8);

	free_drm_info(&pre_pc8);
	free_drm_info(&during_pc8);
	free_drm_info(&post_pc8);
}

static void i2c_subtest_check_environment(void)
{
	int i2c_dev_files = 0;
	DIR *dev_dir;
	struct dirent *dirent;

	/* Make sure the /dev/i2c-* files exist. */
	dev_dir = opendir("/dev");
	igt_assert(dev_dir);
	while ((dirent = readdir(dev_dir))) {
		if (strncmp(dirent->d_name, "i2c-", 4) == 0)
			i2c_dev_files++;
	}
	closedir(dev_dir);
	igt_require(i2c_dev_files);
}

/* Try to use raw I2C, which also needs interrupts. */
static void i2c_subtest(void)
{
	i2c_subtest_check_environment();

	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());
	test_i2c(&ms_data);
	igt_assert(wait_for_suspended());

	enable_one_screen(&ms_data);
}

/* Just reading/writing registers from outside the Kernel is not really a safe
 * thing to do on Haswell, so don't do this test on the default case. */
static void register_compare_subtest(void)
{
	struct compare_registers pre_pc8, post_pc8;

	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	get_registers(&pre_pc8);
	igt_assert(wait_for_active());

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	/* Wait for the registers to be restored. */
	sleep(1);
	get_registers(&post_pc8);
	igt_assert(wait_for_active());

	compare_registers(&pre_pc8, &post_pc8);
}

static void read_full_file(const char *name)
{
	int rc, fd;
	char buf[128];

	igt_assert_f(wait_for_suspended(), "File: %s\n", name);

	fd = open(name, O_RDONLY);
	if (fd < 0)
		return;

	do {
		rc = read(fd, buf, ARRAY_SIZE(buf));
	} while (rc == ARRAY_SIZE(buf));

	rc = close(fd);
	igt_assert(rc == 0);

	igt_assert_f(wait_for_suspended(), "File: %s\n", name);
}

static void read_files_from_dir(const char *name, int level)
{
	DIR *dir;
	struct dirent *dirent;
	char *full_name;
	int rc;

	dir = opendir(name);
	igt_assert(dir);

	full_name = malloc(PATH_MAX);

	igt_assert(level < 128);

	while ((dirent = readdir(dir))) {
		struct stat stat_buf;

		if (strcmp(dirent->d_name, ".") == 0)
			continue;
		if (strcmp(dirent->d_name, "..") == 0)
			continue;

		snprintf(full_name, PATH_MAX, "%s/%s", name, dirent->d_name);

		rc = lstat(full_name, &stat_buf);
		igt_assert(rc == 0);

		if (S_ISDIR(stat_buf.st_mode))
			read_files_from_dir(full_name, level + 1);

		if (S_ISREG(stat_buf.st_mode))
			read_full_file(full_name);
	}

	free(full_name);
	closedir(dir);
}

/* This test will probably pass, with a small chance of hanging the machine in
 * case of bugs. Many of the bugs exercised by this patch just result in dmesg
 * errors, so a "pass" here should be confirmed by a check on dmesg. */
static void debugfs_read_subtest(void)
{
	const char *path = "/sys/kernel/debug/dri/0";
	DIR *dir;

	dir = opendir(path);
	igt_require_f(dir, "Can't open the debugfs directory\n");
	closedir(dir);

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	read_files_from_dir(path, 0);
}

/* Read the comment on debugfs_read_subtest(). */
static void sysfs_read_subtest(void)
{
	const char *path = "/sys/devices/pci0000:00/0000:00:02.0";
	DIR *dir;

	dir = opendir(path);
	igt_require_f(dir, "Can't open the sysfs directory\n");
	closedir(dir);

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	read_files_from_dir(path, 0);
}

/* Make sure we don't suspend when we have the i915_forcewake_user file open. */
static void debugfs_forcewake_user_subtest(void)
{
	int fd, rc;

	igt_require(!(IS_GEN2(ms_data.devid) || IS_GEN3(ms_data.devid) ||
		      IS_GEN4(ms_data.devid) || IS_GEN5(ms_data.devid)));

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	fd = open("/sys/kernel/debug/dri/0/i915_forcewake_user", O_RDONLY);
	igt_require(fd);

	igt_assert(wait_for_active());
	sleep(10);
	igt_assert(wait_for_active());

	rc = close(fd);
	igt_assert(rc == 0);

	igt_assert(wait_for_suspended());
}

static void gem_mmap_subtest(bool gtt_mmap)
{
	int i;
	uint32_t handle;
	int buf_size = 8192;
	uint8_t *gem_buf;

	/* Create, map and set data while the device is active. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	handle = gem_create(drm_fd, buf_size);

	if (gtt_mmap)
		gem_buf = gem_mmap__gtt(drm_fd, handle, buf_size,
					PROT_READ | PROT_WRITE);
	else
		gem_buf = gem_mmap__cpu(drm_fd, handle, buf_size, 0);


	for (i = 0; i < buf_size; i++)
		gem_buf[i] = i & 0xFF;

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	/* Now suspend, read and modify. */
	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));
	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		gem_buf[i] = (~i & 0xFF);
	igt_assert(wait_for_suspended());

	/* Now resume and see if it's still there. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (~i & 0xFF));

	igt_assert(munmap(gem_buf, buf_size) == 0);

	/* Now the opposite: suspend, and try to create the mmap while
	 * suspended. */
	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	if (gtt_mmap)
		gem_buf = gem_mmap__gtt(drm_fd, handle, buf_size,
					PROT_READ | PROT_WRITE);
	else
		gem_buf = gem_mmap__cpu(drm_fd, handle, buf_size, 0);

	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		gem_buf[i] = i & 0xFF;

	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	igt_assert(wait_for_suspended());

	/* Resume and check if it's still there. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());
	for (i = 0; i < buf_size; i++)
		igt_assert(gem_buf[i] == (i & 0xFF));

	igt_assert(munmap(gem_buf, buf_size) == 0);
	gem_close(drm_fd, handle);
}

static void gem_pread_subtest(void)
{
	int i;
	uint32_t handle;
	int buf_size = 8192;
	uint8_t *cpu_buf, *read_buf;

	cpu_buf = malloc(buf_size);
	read_buf = malloc(buf_size);
	igt_assert(cpu_buf);
	igt_assert(read_buf);
	memset(cpu_buf, 0, buf_size);
	memset(read_buf, 0, buf_size);

	/* Create and set data while the device is active. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	handle = gem_create(drm_fd, buf_size);

	for (i = 0; i < buf_size; i++)
		cpu_buf[i] = i & 0xFF;

	gem_write(drm_fd, handle, 0, cpu_buf, buf_size);

	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);

	/* Now suspend, read and modify. */
	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	memset(read_buf, 0, buf_size);
	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);
	igt_assert(wait_for_suspended());

	for (i = 0; i < buf_size; i++)
		cpu_buf[i] = (~i & 0xFF);
	gem_write(drm_fd, handle, 0, cpu_buf, buf_size);
	igt_assert(wait_for_suspended());

	/* Now resume and see if it's still there. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	memset(read_buf, 0, buf_size);
	gem_read(drm_fd, handle, 0, read_buf, buf_size);

	for (i = 0; i < buf_size; i++)
		igt_assert(cpu_buf[i] == read_buf[i]);

	gem_close(drm_fd, handle);

	free(cpu_buf);
	free(read_buf);
}

/* Paints a square of color $color, size $width x $height, at position $x x $y
 * of $dst_handle, which contains pitch $pitch. */
static void submit_blt_cmd(uint32_t dst_handle, uint32_t x, uint32_t y,
			   uint32_t width, uint32_t height, uint32_t pitch,
			   uint32_t color, uint32_t *presumed_dst_offset)
{
	int i, reloc_pos;
	int bpp = 4;
	uint32_t batch_handle;
	int batch_size = 8 * sizeof(uint32_t);
	uint32_t batch_buf[batch_size];
	uint32_t offset_in_dst = (pitch * y) + (x * bpp);
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 objs[2] = {{}, {}};
	struct drm_i915_gem_relocation_entry relocs[1] = {{}};
	struct drm_i915_gem_wait gem_wait;

	i = 0;

	batch_buf[i++] = COLOR_BLT_CMD | COLOR_BLT_WRITE_ALPHA |
			 COLOR_BLT_WRITE_RGB;
	batch_buf[i++] = (3 << 24) | (0xF0 << 16) | pitch;
	batch_buf[i++] = (height << 16) | width * bpp;
	reloc_pos = i;
	batch_buf[i++] = *presumed_dst_offset + offset_in_dst;
	batch_buf[i++] = color;

	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_BATCH_BUFFER_END;
	batch_buf[i++] = MI_NOOP;

	igt_assert(i * sizeof(uint32_t) == batch_size);

	batch_handle = gem_create(drm_fd, batch_size);
	gem_write(drm_fd, batch_handle, 0, batch_buf, batch_size);

	relocs[0].target_handle = dst_handle;
	relocs[0].delta = offset_in_dst;
	relocs[0].offset = reloc_pos * sizeof(uint32_t);
	relocs[0].presumed_offset = *presumed_dst_offset;
	relocs[0].read_domains = 0;
	relocs[0].write_domain = I915_GEM_DOMAIN_RENDER;

	objs[0].handle = dst_handle;
	objs[0].alignment = 64;

	objs[1].handle = batch_handle;
	objs[1].relocation_count = 1;
	objs[1].relocs_ptr = (uintptr_t)relocs;

	execbuf.buffers_ptr = (uintptr_t)objs;
	execbuf.buffer_count = 2;
	execbuf.batch_len = batch_size;
	execbuf.flags = I915_EXEC_BLT;
	i915_execbuffer2_set_context_id(execbuf, 0);

	do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);

	*presumed_dst_offset = relocs[0].presumed_offset;

	gem_wait.flags = 0;
	gem_wait.timeout_ns = 10000000000LL; /* 10s */

	gem_wait.bo_handle = batch_handle;
	do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_WAIT, &gem_wait);

	gem_wait.bo_handle = dst_handle;
	do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_WAIT, &gem_wait);

	gem_close(drm_fd, batch_handle);
}

/* Make sure we can submit a batch buffer and verify its result. */
static void gem_execbuf_subtest(void)
{
	int x, y;
	uint32_t handle;
	int bpp = 4;
	int pitch = 128 * bpp;
	int dst_size = 128 * 128 * bpp; /* 128x128 square */
	uint32_t *cpu_buf;
	uint32_t presumed_offset = 0;
	int sq_x = 5, sq_y = 10, sq_w = 15, sq_h = 20;
	uint32_t color;

	/* Create and set data while the device is active. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	handle = gem_create(drm_fd, dst_size);

	cpu_buf = malloc(dst_size);
	igt_assert(cpu_buf);
	memset(cpu_buf, 0, dst_size);
	gem_write(drm_fd, handle, 0, cpu_buf, dst_size);

	/* Now suspend and try it. */
	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	color = 0x12345678;
	submit_blt_cmd(handle, sq_x, sq_y, sq_w, sq_h, pitch, color,
		       &presumed_offset);
	igt_assert(wait_for_suspended());

	gem_read(drm_fd, handle, 0, cpu_buf, dst_size);
	igt_assert(wait_for_suspended());
	for (y = 0; y < 128; y++) {
		for (x = 0; x < 128; x++) {
			uint32_t px = cpu_buf[y * 128 + x];

			if (y >= sq_y && y < (sq_y + sq_h) &&
			    x >= sq_x && x < (sq_x + sq_w))
				igt_assert(px == color);
			else
				igt_assert(px == 0);
		}
	}

	/* Now resume and check for it again. */
	enable_one_screen(&ms_data);
	igt_assert(wait_for_active());

	memset(cpu_buf, 0, dst_size);
	gem_read(drm_fd, handle, 0, cpu_buf, dst_size);
	for (y = 0; y < 128; y++) {
		for (x = 0; x < 128; x++) {
			uint32_t px = cpu_buf[y * 128 + x];

			if (y >= sq_y && y < (sq_y + sq_h) &&
			    x >= sq_x && x < (sq_x + sq_w))
				igt_assert(px == color);
			else
				igt_assert(px == 0);
		}
	}

	/* Now we'll do the opposite: do the blt while active, then read while
	 * suspended. We use the same spot, but a different color. As a bonus,
	 * we're testing the presumed_offset from the previous command. */
	color = 0x87654321;
	submit_blt_cmd(handle, sq_x, sq_y, sq_w, sq_h, pitch, color,
		       &presumed_offset);

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	memset(cpu_buf, 0, dst_size);
	gem_read(drm_fd, handle, 0, cpu_buf, dst_size);
	for (y = 0; y < 128; y++) {
		for (x = 0; x < 128; x++) {
			uint32_t px = cpu_buf[y * 128 + x];

			if (y >= sq_y && y < (sq_y + sq_h) &&
			    x >= sq_x && x < (sq_x + sq_w))
				igt_assert(px == color);
			else
				igt_assert(px == 0);
		}
	}

	gem_close(drm_fd, handle);

	free(cpu_buf);
}

/* Assuming execbuf already works, let's see what happens when we force many
 * suspend/resume cycles with commands. */
static void gem_execbuf_stress_subtest(void)
{
	int i;
	int max = 50;
	int batch_size = 4 * sizeof(uint32_t);
	uint32_t batch_buf[batch_size];
	uint32_t handle;
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 objs[1] = {{}};

	i = 0;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_NOOP;
	batch_buf[i++] = MI_BATCH_BUFFER_END;
	batch_buf[i++] = MI_NOOP;
	igt_assert(i * sizeof(uint32_t) == batch_size);

	disable_all_screens(&ms_data);
	igt_assert(wait_for_suspended());

	handle = gem_create(drm_fd, batch_size);
	gem_write(drm_fd, handle, 0, batch_buf, batch_size);

	objs[0].handle = handle;

	execbuf.buffers_ptr = (uintptr_t)objs;
	execbuf.buffer_count = 1;
	execbuf.batch_len = batch_size;
	execbuf.flags = I915_EXEC_RENDER;
	i915_execbuffer2_set_context_id(execbuf, 0);

	for (i = 0; i < max; i++) {
		do_ioctl(drm_fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);
		igt_assert(wait_for_suspended());
	}

	gem_close(drm_fd, handle);
}

int main(int argc, char *argv[])
{
	bool do_register_compare = false;

	if (argc > 1 && strcmp(argv[1], "--do-register-compare") == 0)
		do_register_compare = true;

	igt_subtest_init(argc, argv);

	/* Skip instead of failing in case the machine is not prepared to reach
	 * PC8+. We don't want bug reports from cases where the machine is just
	 * not properly configured. */
	igt_fixture
		setup_environment();

	/* Essential things */
	igt_subtest("rte")
		basic_subtest();
	igt_subtest("drm-resources-equal")
		drm_resources_equal_subtest();

	/* Basic modeset */
	igt_subtest("modeset-lpsp")
		modeset_subtest(SCREEN_TYPE_LPSP, 1, WAIT);
	igt_subtest("modeset-non-lpsp")
		modeset_subtest(SCREEN_TYPE_NON_LPSP, 1, WAIT);

	/* GEM */
	igt_subtest("gem-mmap-cpu")
		gem_mmap_subtest(false);
	igt_subtest("gem-mmap-gtt")
		gem_mmap_subtest(true);
	igt_subtest("gem-pread")
		gem_pread_subtest();
	igt_subtest("gem-execbuf")
		gem_execbuf_subtest();

	/* Misc */
	igt_subtest("i2c")
		i2c_subtest();
	igt_subtest("debugfs-read")
		debugfs_read_subtest();
	igt_subtest("debugfs-forcewake-user")
		debugfs_forcewake_user_subtest();
	igt_subtest("sysfs-read")
		sysfs_read_subtest();

	/* Modeset stress */
	igt_subtest("modeset-lpsp-stress")
		modeset_subtest(SCREEN_TYPE_LPSP, 50, WAIT);
	igt_subtest("modeset-non-lpsp-stress")
		modeset_subtest(SCREEN_TYPE_NON_LPSP, 50, WAIT);
	igt_subtest("modeset-lpsp-stress-no-wait")
		modeset_subtest(SCREEN_TYPE_LPSP, 50, DONT_WAIT);
	igt_subtest("modeset-non-lpsp-stress-no-wait")
		modeset_subtest(SCREEN_TYPE_NON_LPSP, 50, DONT_WAIT);

	/* GEM stress */
	igt_subtest("gem-execbuf-stress")
		gem_execbuf_stress_subtest();

	/* Optional */
	igt_subtest("register-compare") {
		igt_require(do_register_compare);
		register_compare_subtest();
	}

	igt_fixture
		teardown_environment();

	igt_exit();
}
