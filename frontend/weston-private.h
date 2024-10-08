/*
 * Copyright 2022 Collabora, Ltd.
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

#pragma once

#include <libweston/libweston.h>
#include <libweston/config-parser.h>

bool
get_backend_from_string(const char *name,
			enum weston_compositor_backend *backend);

bool
get_renderer_from_string(const char *name,
			 enum weston_renderer_type *renderer);

int
wet_output_set_color_characteristics(struct weston_output *output,
				     struct weston_config *wc,
				     struct weston_config_section *section);

int
wet_output_set_eotf_mode(struct weston_output *output,
			 struct weston_config_section *section,
			 bool have_color_manager);

int
wet_output_set_colorimetry_mode(struct weston_output *output,
				struct weston_config_section *section,
				bool have_color_manager);

typedef void (*wet_head_additional_setup)(struct weston_head *head,
					  struct weston_head *head_to_mirror);
