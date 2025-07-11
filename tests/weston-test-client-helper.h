/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _WESTON_TEST_CLIENT_HELPER_H_
#define _WESTON_TEST_CLIENT_HELPER_H_

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pixman.h>

#include <wayland-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "weston-test-runner.h"
#include "weston-test-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "weston-output-capture-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "weston-testsuite-data.h"

struct client {
	struct wl_display *wl_display;

	/*
	 * Have successfully received an expected protocol error, the
	 * connection is in error state, and that is ok.
	 */
	bool errored_ok;

	struct wl_registry *wl_registry;
	struct wl_compositor *wl_compositor;
	struct wl_shm *wl_shm;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct test *test;
	/* the seat that is actually used for input events */
	struct input *input;
	/* server can have more wl_seats. We need keep them all until we
	 * find the one that we need. After that, the others
	 * will be destroyed, so this list will have the length of 1.
	 * If some day in the future we will need the other seats,
	 * we can just keep them here. */
	struct wl_list inputs;
	struct output *output;
	struct surface *surface;
	struct wl_array shm_formats;
	struct wl_array drm_formats;
	struct wl_list global_list;
	struct wl_list output_list; /* struct output::link */
};

struct global {
	uint32_t name;
	char *interface;
	uint32_t version;
	struct wl_list link;
};

struct test {
	struct weston_test *weston_test;
	int pointer_x;
	int pointer_y;
	uint32_t n_egl_buffers;
};

struct input {
	struct client *client;
	uint32_t global_name;
	struct wl_seat *wl_seat;
	struct pointer *pointer;
	struct keyboard *keyboard;
	struct touch *touch;
	char *seat_name;
	enum wl_seat_capability caps;
	struct wl_list link;
};

struct pointer {
	struct wl_pointer *wl_pointer;
	struct surface *focus;
	uint32_t serial;
	int x;
	int y;
	uint32_t button;
	uint32_t state;
	uint32_t axis;
	double axis_value;
	uint32_t motion_time_msec;
	uint32_t button_time_msec;
	uint32_t axis_time_msec;
	uint32_t axis_stop_time_msec;
	struct timespec input_timestamp;
	struct timespec motion_time_timespec;
	struct timespec button_time_timespec;
	struct timespec axis_time_timespec;
	struct timespec axis_stop_time_timespec;
};

struct keyboard {
	struct wl_keyboard *wl_keyboard;
	struct surface *focus;
	uint32_t key;
	uint32_t state;
	uint32_t mods_depressed;
	uint32_t mods_latched;
	uint32_t mods_locked;
	uint32_t group;
	struct {
		int rate;
		int delay;
	} repeat_info;
	uint32_t key_time_msec;
	struct timespec input_timestamp;
	struct timespec key_time_timespec;
};

struct touch {
	struct wl_touch *wl_touch;
	int down_x;
	int down_y;
	int x;
	int y;
	int id;
	int up_id; /* id of last wl_touch.up event */
	int frame_no;
	int cancel_no;
	uint32_t down_time_msec;
	uint32_t up_time_msec;
	uint32_t motion_time_msec;
	struct timespec input_timestamp;
	struct timespec down_time_timespec;
	struct timespec up_time_timespec;
	struct timespec motion_time_timespec;
};

struct output {
	struct wl_output *wl_output;
	struct wl_list link; /* struct client::output_list */
	int x;
	int y;
	int width;
	int height;
	int scale;
	int initialized;
	char *name;
	char *desc;
};

struct buffer {
	struct wl_buffer *proxy;
	size_t len;
	pixman_image_t *image;
};

struct surface {
	struct client *client; /* not owned */

	struct wl_surface *wl_surface;
	struct output *output; /* not owned */
	int x;
	int y;
	int width;
	int height;
	struct buffer *buffer;
};

struct rectangle {
	int x;
	int y;
	int width;
	int height;
};

struct range {
	int a;
	int b;
};

struct client *
create_client(void);

void
client_destroy(struct client *client);

struct surface *
create_test_surface(struct client *client);

void
surface_destroy(struct surface *surface);

void
surface_set_opaque_rect(struct surface *surface, const struct rectangle *rect);

struct client *
create_client_and_test_surface(int x, int y, int width, int height);

bool
support_shm_format(struct client *client, uint32_t shm_format);

struct buffer *
create_shm_buffer(struct client *client, int width, int height,
		  uint32_t drm_format);

struct buffer *
create_shm_buffer_a8r8g8b8(struct client *client, int width, int height);

bool
support_drm_format(struct client *client, uint32_t format, uint64_t modifier);

void
buffer_destroy(struct buffer *buf);

int
surface_contains(struct surface *surface, int x, int y);

void
move_client(struct client *client, int x, int y);

void
move_client_frame_sync(struct client *client, int x, int y);

void
move_client_offscreenable(struct client *client, int x, int y);

#define client_roundtrip(c) do { \
	test_assert_int_ge(wl_display_roundtrip((c)->wl_display), 0); \
} while (0)

struct wl_callback *
frame_callback_set(struct wl_surface *surface, int *done);

int
frame_callback_wait_nofail(struct client *client, int *done);

#define frame_callback_wait(c, d) test_assert_true(frame_callback_wait_nofail((c), (d)))

void
expect_protocol_error(struct client *client,
		      const struct wl_interface *intf, uint32_t code);

char *
screenshot_reference_filename(const char *basename, uint32_t seq);

char *
image_filename(const char *basename);

char *
output_filename_for_test_program(const char *test_program, const char *suffix,
				 const char *file_ext);

char *
output_filename_for_fixture(const char *test_program,
			    struct weston_test_harness *harness,
			    const char *suffix, const char *file_ext);

char *
output_filename_for_test_case(const char *suffix, uint32_t seq_number,
			      const char *file_ext);

FILE *
fopen_dump_file(const char *suffix);

bool
check_images_match(pixman_image_t *img_a, pixman_image_t *img_b,
		   const struct rectangle *clip,
		   const struct range *prec);

pixman_image_t *
visualize_image_difference(pixman_image_t *img_a, pixman_image_t *img_b,
			   const struct rectangle *clip_rect,
			   const struct range *prec);

bool
write_image_as_png(pixman_image_t *image, const char *fname);

pixman_image_t *
load_image_from_png(const char *fname);

struct buffer *
capture_screenshot_of_output(struct client *client, const char *output_name);

struct buffer *
client_capture_output(struct client *client,
		      struct output *output,
		      enum weston_capture_v1_source src);

pixman_image_t *
image_convert_to_a8r8g8b8(pixman_image_t *image);

bool
verify_image(pixman_image_t *shot,
	     const char *ref_image,
	     int ref_seq_no,
	     const struct rectangle *clip,
	     int seq_no);

bool
verify_screen_content(struct client *client,
		      const char *ref_image,
		      int ref_seq_no,
		      const struct rectangle *clip,
		      int seq_no, const char *output_name);

struct buffer *
client_buffer_from_image_file(struct client *client,
			      const char *basename,
			      int scale);

void *
bind_to_singleton_global(struct client *client,
			 const struct wl_interface *iface,
			 int version);

struct wp_viewport *
client_create_viewport(struct client *client);

void
fill_image_with_color(pixman_image_t *image, const pixman_color_t *color);

pixman_color_t *
color_rgb888(pixman_color_t *tmp, uint8_t r, uint8_t g, uint8_t b);

/* Helper to wait for the next breakpoint and execute inside it; as this is a
 * for loop, continue/break/etc will not affect an enclosing scope! */
#define RUN_INSIDE_BREAKPOINT(client_, suite_data_)				\
	for (struct wet_test_active_breakpoint *breakpoint =			\
		client_wait_breakpoint(client_, suite_data_);			\
	     suite_data_->breakpoints.in_client_break;				\
	     client_release_breakpoint(client_, suite_data_, breakpoint))

/* Specifies that the currently-executing breakpoint should be rearmed */
#define REARM_BREAKPOINT(breakpoint_) breakpoint_->rearm_on_release = true

void
client_push_breakpoint(struct client *client,
		       struct wet_testsuite_data *suite_data,
		       enum weston_test_breakpoint breakpoint,
		       struct wl_proxy *proxy);

struct wet_test_active_breakpoint *
client_wait_breakpoint(struct client *client,
		       struct wet_testsuite_data *suite_data);

void
client_insert_breakpoint(struct client *client,
			 struct wet_testsuite_data *suite_data,
			 enum weston_test_breakpoint breakpoint,
			 struct wl_proxy *proxy);

void
client_remove_breakpoint(struct client *client,
			 struct wet_testsuite_data *suite_data,
			 enum weston_test_breakpoint breakpoint,
			 struct wl_proxy *proxy);

void
client_release_breakpoint(struct client *client,
			  struct wet_testsuite_data *suite_data,
			  struct wet_test_active_breakpoint *active_bp);
void *
get_resource_data_from_proxy(struct wet_testsuite_data *suite_data,
			     struct wl_proxy *proxy);
void
assert_resource_is_proxy(struct wet_testsuite_data *suite_data,
			 struct wl_resource *r, void *p);
void
assert_output_matches(struct wet_testsuite_data *suite_data,
		      struct weston_output *s, struct output *c);
void
assert_surface_matches(struct wet_testsuite_data *suite_data,
		       struct weston_surface *s, struct surface *c);

#endif
