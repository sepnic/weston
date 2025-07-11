/*
 * Copyright 2010-2012 Intel Corporation
 * Copyright 2013 Raspberry Pi Foundation
 * Copyright 2011-2012,2020 Collabora, Ltd.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kiosk-shell.h"
#include "kiosk-shell-grab.h"
#include "frontend/weston.h"
#include "libweston/libweston.h"
#include "shared/helpers.h"
#include <libweston/shell-utils.h>

#include <libweston/xwayland-api.h>

static struct kiosk_shell_surface *
get_kiosk_shell_surface(struct weston_surface *surface)
{
	struct weston_desktop_surface *desktop_surface =
		weston_surface_get_desktop_surface(surface);

	if (desktop_surface)
		return weston_desktop_surface_get_user_data(desktop_surface);

	return NULL;
}

static void
kiosk_shell_seat_handle_destroy(struct wl_listener *listener, void *data);

static struct kiosk_shell_seat *
get_kiosk_shell_seat(struct weston_seat *seat)
{
	struct wl_listener *listener;

	if (!seat)
		return NULL;

	listener = wl_signal_get(&seat->destroy_signal,
				 kiosk_shell_seat_handle_destroy);

	if (!listener)
		return NULL;

	return container_of(listener,
			    struct kiosk_shell_seat, seat_destroy_listener);
}


static struct weston_seat *
get_kiosk_shell_first_seat(struct kiosk_shell *shell)
{
	struct wl_list *node;
	struct weston_compositor *compositor = shell->compositor;

	if (wl_list_empty(&compositor->seat_list))
		return NULL;

	node = compositor->seat_list.next;
	return container_of(node, struct weston_seat, link);
}

static void
transform_handler(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct kiosk_shell_surface *shsurf = get_kiosk_shell_surface(surface);
	const struct weston_xwayland_surface_api *api;

	if (!shsurf)
		return;

	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}

	if (!api || !api->is_xwayland_surface(surface))
		return;

	if (!weston_view_is_mapped(shsurf->view))
		return;

	api->send_position(surface,
			   shsurf->view->geometry.pos_offset.x,
			   shsurf->view->geometry.pos_offset.y);
}

static const char *
xwayland_get_xwayland_name(struct kiosk_shell_surface *shsurf, enum window_atom_type type)
{
	const struct weston_xwayland_surface_api *api;
	struct weston_surface *surface;

	api = shsurf->shell->xwayland_surface_api;
	if (!api) {
		api = weston_xwayland_surface_get_api(shsurf->shell->compositor);
		shsurf->shell->xwayland_surface_api = api;
	}

	surface = weston_desktop_surface_get_surface(shsurf->desktop_surface);
	if (!api || !api->is_xwayland_surface(surface))
		return NULL;

	return api->get_xwayland_window_name(surface, type);
}

/*
 * kiosk_shell_surface
 */

static void
kiosk_shell_surface_set_output(struct kiosk_shell_surface *shsurf,
			       struct kiosk_shell_output *shoutput);
static void
kiosk_shell_surface_set_parent(struct kiosk_shell_surface *shsurf,
			       struct kiosk_shell_surface *parent);
static void
kiosk_shell_output_set_active_surface_tree(struct kiosk_shell_output *shoutput,
					   struct kiosk_shell_surface *shroot);
static void
kiosk_shell_output_raise_surface_subtree(struct kiosk_shell_output *shoutput,
					 struct kiosk_shell_surface *shroot);

static void
kiosk_shell_surface_notify_parent_destroy(struct wl_listener *listener, void *data)
{
	struct kiosk_shell_surface *shsurf =
		container_of(listener,
			     struct kiosk_shell_surface, parent_destroy_listener);

	kiosk_shell_surface_set_parent(shsurf, shsurf->parent->parent);
}

static void
kiosk_shell_surface_notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct kiosk_shell_surface *shsurf =
		container_of(listener,
			     struct kiosk_shell_surface, output_destroy_listener);

	kiosk_shell_surface_set_output(shsurf, NULL);
}

static struct kiosk_shell_surface *
kiosk_shell_surface_get_parent_root(struct kiosk_shell_surface *shsurf)
{
	struct kiosk_shell_surface *root = shsurf;
	while (root->parent)
		root = root->parent;
	return root;
}

static bool
kiosk_shell_output_has_app_id(char *config_app_ids, const char *app_id);

static struct kiosk_shell_output *
kiosk_shell_surface_find_best_output_for_xwayland(struct kiosk_shell_surface *shsurf)
{
	struct kiosk_shell_output *shoutput;
	const char *wm_name;
	const char *wm_class;

	wm_name = xwayland_get_xwayland_name(shsurf, WM_NAME);
	wm_class = xwayland_get_xwayland_name(shsurf, WM_CLASS);

	if (wm_name && wm_class) {
		bool found_wm_name = false;
		bool found_wm_class = false;

		wl_list_for_each(shoutput, &shsurf->shell->output_list, link) {
			if (kiosk_shell_output_has_app_id(shoutput->x11_wm_name_app_ids,
							  wm_name))
				found_wm_name = true;

			if (kiosk_shell_output_has_app_id(shoutput->x11_wm_class_app_ids,
							  wm_class))
				found_wm_class = true;

			if (found_wm_name && found_wm_class) {
				shsurf->appid_output_assigned = true;
				return shoutput;
			}
		}
	}

	/* fallback to search for each entry */
	if (wm_name) {
		wl_list_for_each(shoutput, &shsurf->shell->output_list, link) {
			if (kiosk_shell_output_has_app_id(shoutput->x11_wm_name_app_ids,
							  wm_name)) {
				shsurf->appid_output_assigned = true;
				return shoutput;
			}
		}
	}

	if (wm_class) {
		wl_list_for_each(shoutput, &shsurf->shell->output_list, link) {
			if (kiosk_shell_output_has_app_id(shoutput->x11_wm_class_app_ids,
							  wm_class)) {
				shsurf->appid_output_assigned = true;
				return shoutput;
			}
		}
	}

	return NULL;
}

static struct kiosk_shell_output *
kiosk_shell_surface_find_best_output(struct kiosk_shell_surface *shsurf)
{
	struct weston_output *output;
	struct kiosk_shell_output *shoutput;
	struct kiosk_shell_surface *root;
	const char *app_id;

	/* Always use current output if any. */
	if (shsurf->output)
		return shsurf->output;

	/* Check if we have a designated output for this app. */
	app_id = weston_desktop_surface_get_app_id(shsurf->desktop_surface);
	if (app_id) {
		wl_list_for_each(shoutput, &shsurf->shell->output_list, link) {
			if (kiosk_shell_output_has_app_id(shoutput->app_ids, app_id)) {
				shsurf->appid_output_assigned = true;
				return shoutput;
			}
		}
	}

	shoutput = kiosk_shell_surface_find_best_output_for_xwayland(shsurf);
	if (shoutput)
		return shoutput;

	/* Group all related windows in the same output. */
	root = kiosk_shell_surface_get_parent_root(shsurf);
	if (root->output)
		return root->output;

	output = weston_shell_utils_get_focused_output(shsurf->shell->compositor);
	if (output)
		return weston_output_get_shell_private(output);

	output = weston_shell_utils_get_default_output(shsurf->shell->compositor);
	if (output)
		return weston_output_get_shell_private(output);

	return NULL;
}

static void
kiosk_shell_surface_set_output(struct kiosk_shell_surface *shsurf,
			       struct kiosk_shell_output *shoutput)
{
	shsurf->output = shoutput;

	if (shsurf->output_destroy_listener.notify) {
		wl_list_remove(&shsurf->output_destroy_listener.link);
		shsurf->output_destroy_listener.notify = NULL;
	}

	if (!shsurf->output)
		return;

	shsurf->output_destroy_listener.notify =
		kiosk_shell_surface_notify_output_destroy;
	wl_signal_add(&shsurf->output->output->destroy_signal,
		      &shsurf->output_destroy_listener);
}

static void
kiosk_shell_surface_set_fullscreen(struct kiosk_shell_surface *shsurf,
				   struct kiosk_shell_output *shoutput)
{
	if (!shoutput)
		shoutput = kiosk_shell_surface_find_best_output(shsurf);

	kiosk_shell_surface_set_output(shsurf, shoutput);

	weston_desktop_surface_set_fullscreen(shsurf->desktop_surface, true);
	if (shsurf->output)
		weston_desktop_surface_set_size(shsurf->desktop_surface,
						shsurf->output->output->width,
						shsurf->output->output->height);
}

static void
kiosk_shell_surface_set_maximized(struct kiosk_shell_surface *shsurf)
{
	struct kiosk_shell_output *shoutput =
		kiosk_shell_surface_find_best_output(shsurf);

	kiosk_shell_surface_set_output(shsurf, shoutput);

	weston_desktop_surface_set_maximized(shsurf->desktop_surface, true);
	if (shsurf->output)
		weston_desktop_surface_set_size(shsurf->desktop_surface,
						shsurf->output->output->width,
						shsurf->output->output->height);
}

static void
kiosk_shell_surface_set_normal(struct kiosk_shell_surface *shsurf)
{
	if (!shsurf->output)
		kiosk_shell_surface_set_output(shsurf,
			kiosk_shell_surface_find_best_output(shsurf));

	weston_desktop_surface_set_fullscreen(shsurf->desktop_surface, false);
	weston_desktop_surface_set_maximized(shsurf->desktop_surface, false);
	weston_desktop_surface_set_size(shsurf->desktop_surface, 0, 0);
}

static bool
kiosk_shell_surface_is_surface_in_tree(struct kiosk_shell_surface *shsurf,
				       struct kiosk_shell_surface *shroot)
{
	struct kiosk_shell_surface *s;

	wl_list_for_each(s, &shroot->surface_tree_list, surface_tree_link) {
		if (s == shsurf)
			return true;
	}

	return false;
}

static bool
kiosk_shell_surface_is_descendant_of(struct kiosk_shell_surface *shsurf,
				     struct kiosk_shell_surface *ancestor)
{
	while (shsurf) {
		if (shsurf == ancestor)
			return true;
		shsurf = shsurf->parent;
	}

	return false;
}

static void
active_surface_tree_move_element_to_top(struct wl_list *active_surface_tree,
					struct wl_list *element)
{
	wl_list_remove(element);
	wl_list_insert(active_surface_tree, element);
}

static void
kiosk_shell_surface_set_parent(struct kiosk_shell_surface *shsurf,
			       struct kiosk_shell_surface *parent)
{
	struct kiosk_shell_output *shoutput = shsurf->output;
	struct kiosk_shell_surface *shroot = parent ?
		kiosk_shell_surface_get_parent_root(parent) :
		kiosk_shell_surface_get_parent_root(shsurf);

	/* There are cases where xdg clients call .set_parent(nil) on a surface
	 * that does not have a parent. The protocol states that this is
	 * effectively a no-op. */
	if (!parent && shsurf == shroot)
		return;

	if (shsurf->parent_destroy_listener.notify) {
		wl_list_remove(&shsurf->parent_destroy_listener.link);
		shsurf->parent_destroy_listener.notify = NULL;
	}

	shsurf->parent = parent;

	if (shsurf->parent) {
		shsurf->parent_destroy_listener.notify =
			kiosk_shell_surface_notify_parent_destroy;
		wl_signal_add(&parent->parent_destroy_signal,
			      &shsurf->parent_destroy_listener);

		if (!kiosk_shell_surface_is_surface_in_tree(shsurf, shroot)) {
			active_surface_tree_move_element_to_top(&shroot->surface_tree_list,
								&shsurf->surface_tree_link);
		}
		kiosk_shell_surface_set_output(shsurf, NULL);
		kiosk_shell_surface_set_normal(shsurf);
	} else {
		struct kiosk_shell_surface *s, *tmp;

		/* Relink the child and all its descendents to a new surface
		 * tree list, with the child as root. */
		wl_list_init(&shsurf->surface_tree_list);
		wl_list_for_each_reverse_safe(s, tmp, &shroot->surface_tree_list,
					      surface_tree_link) {
			if (kiosk_shell_surface_is_descendant_of(s, shsurf)) {
				active_surface_tree_move_element_to_top(&shsurf->surface_tree_list,
									&s->surface_tree_link);
			}
		}
		kiosk_shell_output_set_active_surface_tree(shoutput, shsurf);
		kiosk_shell_surface_set_fullscreen(shsurf, shsurf->output);
	}
}

static void
kiosk_shell_surface_reconfigure_for_output(struct kiosk_shell_surface *shsurf)
{
	struct weston_desktop_surface *desktop_surface;
	struct weston_output *w_output;

	if (!shsurf->output)
		return;

	w_output = shsurf->output->output;
	desktop_surface = shsurf->desktop_surface;

	if (weston_desktop_surface_get_maximized(desktop_surface) ||
	    weston_desktop_surface_get_fullscreen(desktop_surface)) {
		weston_desktop_surface_set_size(desktop_surface,
						w_output->width,
						w_output->height);
	}

	weston_shell_utils_center_on_output(shsurf->view, w_output);
	weston_view_update_transform(shsurf->view);
}

static void
kiosk_shell_surface_destroy(struct kiosk_shell_surface *shsurf)
{
	wl_signal_emit(&shsurf->destroy_signal, shsurf);
	wl_list_remove(&shsurf->surface_tree_link);

	weston_desktop_surface_set_user_data(shsurf->desktop_surface, NULL);
	shsurf->desktop_surface = NULL;

	weston_desktop_surface_unlink_view(shsurf->view);

	weston_view_destroy(shsurf->view);

	if (shsurf->output_destroy_listener.notify) {
		wl_list_remove(&shsurf->output_destroy_listener.link);
		shsurf->output_destroy_listener.notify = NULL;
	}

	if (shsurf->parent_destroy_listener.notify) {
		wl_list_remove(&shsurf->parent_destroy_listener.link);
		shsurf->parent_destroy_listener.notify = NULL;
		shsurf->parent = NULL;
	}

	free(shsurf);
}

static struct kiosk_shell_surface *
kiosk_shell_surface_create(struct kiosk_shell *shell,
			   struct weston_desktop_surface *desktop_surface)
{
	struct weston_desktop_client *client =
		weston_desktop_surface_get_client(desktop_surface);
	struct wl_client *wl_client =
		weston_desktop_client_get_client(client);
	struct weston_view *view;
	struct kiosk_shell_surface *shsurf;

	view = weston_desktop_surface_create_view(desktop_surface);
	if (!view)
		return NULL;

	shsurf = zalloc(sizeof *shsurf);
	if (!shsurf) {
		if (wl_client)
			wl_client_post_no_memory(wl_client);
		else
			weston_log("no memory to allocate shell surface\n");
		return NULL;
	}

	shsurf->desktop_surface = desktop_surface;
	shsurf->view = view;
	shsurf->shell = shell;
	shsurf->appid_output_assigned = false;

	weston_desktop_surface_set_user_data(desktop_surface, shsurf);

	wl_signal_init(&shsurf->destroy_signal);
	wl_signal_init(&shsurf->parent_destroy_signal);

	/* start life inserting itself as root of its own surface tree list */
	wl_list_init(&shsurf->surface_tree_list);
	wl_list_init(&shsurf->surface_tree_link);
	wl_list_insert(&shsurf->surface_tree_list, &shsurf->surface_tree_link);

	return shsurf;
}

static void
kiosk_shell_surface_activate(struct kiosk_shell_surface *shsurf,
			     struct kiosk_shell_seat *kiosk_seat,
			     uint32_t activate_flags)
{
	struct weston_desktop_surface *dsurface = shsurf->desktop_surface;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(dsurface);
	struct kiosk_shell_output *shoutput = shsurf->output;

	/* keyboard focus */
	weston_view_activate_input(shsurf->view, kiosk_seat->seat, activate_flags);

	/* xdg-shell deactivation if there's a focused one */
	if (kiosk_seat->focused_surface) {
		struct kiosk_shell_surface *current_focus =
			get_kiosk_shell_surface(kiosk_seat->focused_surface);
		struct weston_desktop_surface *dsurface_focus;
		assert(current_focus);

		dsurface_focus = current_focus->desktop_surface;
		if (--current_focus->focus_count == 0)
			weston_desktop_surface_set_activated(dsurface_focus, false);
	}

	/* xdg-shell activation for the new one */
	kiosk_seat->focused_surface = surface;
	if (shsurf->focus_count++ == 0)
		weston_desktop_surface_set_activated(dsurface, true);

	/* raise the focused subtree to the top of the visible layer */
	kiosk_shell_output_raise_surface_subtree(shoutput, shsurf);
}

/*
 * kiosk_shell_seat
 */

static void
kiosk_shell_seat_destroy(struct kiosk_shell_seat *shseat)
{
	wl_list_remove(&shseat->seat_destroy_listener.link);
	wl_list_remove(&shseat->link);
	free(shseat);
}

static void
kiosk_shell_seat_handle_destroy(struct wl_listener *listener, void *data)
{
	struct kiosk_shell_seat *shseat =
		container_of(listener,
			     struct kiosk_shell_seat, seat_destroy_listener);

	kiosk_shell_seat_destroy(shseat);
}

static struct kiosk_shell_seat *
kiosk_shell_seat_create(struct kiosk_shell *shell, struct weston_seat *seat)
{
	struct kiosk_shell_seat *shseat;

	if (wl_list_length(&shell->seat_list) > 0) {
		weston_log("WARNING: multiple seats detected. kiosk-shell "
			   "can not handle multiple seats!\n");
		return NULL;
	}

	shseat = zalloc(sizeof *shseat);
	if (!shseat) {
		weston_log("no memory to allocate shell seat\n");
		return NULL;
	}

	shseat->seat = seat;

	shseat->seat_destroy_listener.notify = kiosk_shell_seat_handle_destroy;
	wl_signal_add(&seat->destroy_signal, &shseat->seat_destroy_listener);

	wl_list_insert(&shell->seat_list, &shseat->link);

	return shseat;
}

/*
 * kiosk_shell_output
 */

static void
kiosk_shell_output_set_active_surface_tree(struct kiosk_shell_output *shoutput,
					   struct kiosk_shell_surface *shroot)

{
	struct kiosk_shell *shell = shoutput->shell;
	struct kiosk_shell_surface *s;

	/* Remove the previous active surface tree (i.e., move the tree to
	 * WESTON_LAYER_POSITION_HIDDEN) */
	if (shoutput->active_surface_tree) {
		wl_list_for_each_reverse(s, shoutput->active_surface_tree, surface_tree_link) {
			weston_view_move_to_layer(s->view,
						  &shell->inactive_layer.view_list);
		}
	}

	if (shroot) {
		wl_list_for_each_reverse(s, &shroot->surface_tree_list, surface_tree_link) {
			weston_view_move_to_layer(s->view,
						  &shell->normal_layer.view_list);
		}
	}

	shoutput->active_surface_tree = shroot ?
					&shroot->surface_tree_list :
					NULL;
}

/* Raises the subtree originating at the specified 'shroot' of the output's
 * active surface tree to the top of the visible layer. */
static void
kiosk_shell_output_raise_surface_subtree(struct kiosk_shell_output *shoutput,
					 struct kiosk_shell_surface *shroot)
{
	struct kiosk_shell *shell = shroot->shell;
	struct wl_list tmp_list;
	struct kiosk_shell_surface *s, *tmp_s;

	wl_list_init(&tmp_list);

	if (!shoutput->active_surface_tree)
		return;

	/* Move all shell surfaces in the active surface tree starting at
	 * shroot to the tmp_list while maintaining the relative order. */
	wl_list_for_each_reverse_safe(s, tmp_s,
				      shoutput->active_surface_tree, surface_tree_link) {
		if (kiosk_shell_surface_is_descendant_of(s, shroot)) {
			active_surface_tree_move_element_to_top(&tmp_list,
								&s->surface_tree_link);
		}
	}

	/* Now insert the views corresponding to the shell surfaces stored to
	 * the top of the layer in the proper order.
	 * Also remove the shell surface from tmp_list and insert it at the top
	 * of the output's active surface tree. */
	wl_list_for_each_reverse_safe(s, tmp_s, &tmp_list, surface_tree_link) {
		weston_view_move_to_layer(s->view, &shell->normal_layer.view_list);

		active_surface_tree_move_element_to_top(shoutput->active_surface_tree,
							&s->surface_tree_link);
	}
}

static int
kiosk_shell_background_surface_get_label(struct weston_surface *surface,
					 char *buf, size_t len)
{
	return snprintf(buf, len, "kiosk shell background surface");
}

static void
kiosk_shell_output_recreate_background(struct kiosk_shell_output *shoutput)
{
	struct kiosk_shell *shell = shoutput->shell;
	struct weston_compositor *ec = shell->compositor;
	struct weston_output *output = shoutput->output;
	struct weston_config_section *shell_section = NULL;
	uint32_t bg_color = 0x0;
	struct weston_curtain_params curtain_params = {};

	if (shoutput->curtain)
		weston_shell_utils_curtain_destroy(shoutput->curtain);

	if (!output)
		return;

	if (shell->config)
		shell_section = weston_config_get_section(shell->config, "shell", NULL, NULL);
	if (shell_section)
		weston_config_section_get_color(shell_section, "background-color",
						&bg_color, 0x00000000);

	curtain_params.r = ((bg_color >> 16) & 0xff) / 255.0;
	curtain_params.g = ((bg_color >> 8) & 0xff) / 255.0;
	curtain_params.b = ((bg_color >> 0) & 0xff) / 255.0;
	curtain_params.a = 1.0;

	curtain_params.pos = output->pos;
	curtain_params.width = output->width;
	curtain_params.height = output->height;

	curtain_params.capture_input = true;

	curtain_params.get_label = kiosk_shell_background_surface_get_label;
	curtain_params.surface_committed = NULL;
	curtain_params.surface_private = NULL;

	shoutput->curtain = weston_shell_utils_curtain_create(ec, &curtain_params);

	weston_surface_set_role(shoutput->curtain->view->surface,
				"kiosk-shell-background", NULL, 0);

	shoutput->curtain->view->surface->output = output;

	weston_view_move_to_layer(shoutput->curtain->view,
				  &shell->background_layer.view_list);
	weston_view_set_output(shoutput->curtain->view, output);
}

static void
kiosk_shell_output_destroy(struct kiosk_shell_output *shoutput)
{
	shoutput->output = NULL;
	shoutput->output_destroy_listener.notify = NULL;

	if (shoutput->curtain)
		weston_shell_utils_curtain_destroy(shoutput->curtain);

	wl_list_remove(&shoutput->output_destroy_listener.link);
	wl_list_remove(&shoutput->link);

	free(shoutput->app_ids);
	free(shoutput->x11_wm_name_app_ids);
	free(shoutput->x11_wm_class_app_ids);

	free(shoutput);
}

static bool
kiosk_shell_output_has_app_id(char *config_app_ids, const char *app_id)
{
	char *cur;
	size_t app_id_len;

	if (!config_app_ids)
		return false;

	cur = config_app_ids;
	app_id_len = strlen(app_id);

	while ((cur = strstr(cur, app_id))) {
		/* Check whether we have found a complete match of app_id. */
		if ((cur[app_id_len] == ',' || cur[app_id_len] == '\0') &&
		    (cur == config_app_ids || cur[-1] == ','))
			return true;
		cur++;
	}

	return false;
}

static void
kiosk_shell_output_configure(struct kiosk_shell_output *shoutput)
{
	struct weston_config *wc = wet_get_config(shoutput->shell->compositor);
	struct weston_config_section *section =
		weston_config_get_section(wc, "output", "name", shoutput->output->name);

	assert(shoutput->app_ids == NULL);
	assert(shoutput->x11_wm_name_app_ids == NULL);
	assert(shoutput->x11_wm_class_app_ids == NULL);

	if (section) {
		weston_config_section_get_string(section, "app-ids",
						 &shoutput->app_ids, NULL);
		weston_config_section_get_string(section, "x11-wm-name",
						 &shoutput->x11_wm_name_app_ids, NULL);
		weston_config_section_get_string(section, "x11-wm-class",
						 &shoutput->x11_wm_class_app_ids, NULL);
	}
}

static void
kiosk_shell_output_notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct kiosk_shell_output *shoutput =
		container_of(listener,
			     struct kiosk_shell_output, output_destroy_listener);

	kiosk_shell_output_destroy(shoutput);
}

static struct kiosk_shell_output *
kiosk_shell_output_create(struct kiosk_shell *shell, struct weston_output *output)
{
	struct kiosk_shell_output *shoutput;

	shoutput = zalloc(sizeof *shoutput);
	if (shoutput == NULL)
		return NULL;

	shoutput->output = output;
	shoutput->shell = shell;

	shoutput->output_destroy_listener.notify =
		kiosk_shell_output_notify_output_destroy;
	wl_signal_add(&shoutput->output->destroy_signal,
		      &shoutput->output_destroy_listener);

	wl_list_insert(shell->output_list.prev, &shoutput->link);

	weston_output_set_shell_private(output, shoutput);

	kiosk_shell_output_recreate_background(shoutput);
	kiosk_shell_output_configure(shoutput);

	return shoutput;
}

/*
 * libweston-desktop
 */

static void
desktop_surface_added(struct weston_desktop_surface *desktop_surface,
		      void *data)
{
	struct kiosk_shell *shell = data;
	struct kiosk_shell_surface *shsurf;
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);

	shsurf = kiosk_shell_surface_create(shell, desktop_surface);
	if (!shsurf)
		return;

	weston_surface_set_label_func(surface, weston_shell_utils_surface_get_label);
	kiosk_shell_surface_set_fullscreen(shsurf, NULL);
}

/* Return the shell surface that should gain focus after the specified shsurf is
 * destroyed. We prefer the top remaining view from the same parent surface,
 * but if we can't find one we fall back to the top view regardless of
 * parentage.
 * First look for the successor in the normal layer, and if that
 * fails, look for it in the inactive layer, and if that also fails, then there
 * is no successor. */
static struct kiosk_shell_surface *
find_focus_successor(struct kiosk_shell_surface *shsurf,
		     struct weston_surface *focused_surface)
{
	struct kiosk_shell_surface *parent_root =
		kiosk_shell_surface_get_parent_root(shsurf);
	struct weston_view *top_view = NULL;
	struct kiosk_shell_surface *successor = NULL;
	struct wl_list *layers = &shsurf->shell->compositor->layer_list;
	struct weston_layer *layer;
	struct weston_view *view;

	if (!shsurf->output)
		return NULL;

	wl_list_for_each(layer, layers, link) {
		struct kiosk_shell *shell = shsurf->shell;

		if (layer != &shell->inactive_layer &&
		    layer != &shell->normal_layer) {
			continue;
		}
		wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
			struct kiosk_shell_surface *view_shsurf;
			struct kiosk_shell_surface *root;

			if (view == shsurf->view)
				continue;

			/* pick views only on the same output */
			if (view->output != shsurf->output->output)
				continue;

			view_shsurf = get_kiosk_shell_surface(view->surface);
			if (!view_shsurf)
				continue;

			if (!top_view)
				top_view = view;

			root = kiosk_shell_surface_get_parent_root(view_shsurf);
			if (root == parent_root) {
				top_view = view;
				break;
			}
		}
	}

	if (top_view)
		successor = get_kiosk_shell_surface(top_view->surface);

	return successor;
}

static void
desktop_surface_removed(struct weston_desktop_surface *desktop_surface,
			void *data)
{
	struct kiosk_shell *shell = data;
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	struct weston_seat *seat;
	struct kiosk_shell_seat* kiosk_seat;

	if (!shsurf)
		return;

	seat = get_kiosk_shell_first_seat(shell);
	kiosk_seat = get_kiosk_shell_seat(seat);

	/* Inform children about destruction of their parent, so that we can
	 * reparent them and potentially relink surface tree links before
	 * finding a focus successor and activating a new surface. */
	wl_signal_emit(&shsurf->parent_destroy_signal, shsurf);

	/* We need to take into account that the surface being destroyed it not
	 * always the same as the focused surface, which could result in picking
	 * and *activating* the wrong window.
	 *
	 * Apply that only on the same output to avoid incorrectly picking an
	 * invalid surface, which could happen if the view being destroyed
	 * is on a output different than the focused_surface output */
	if (seat && kiosk_seat && kiosk_seat->focused_surface &&
	    (kiosk_seat->focused_surface == surface ||
	    surface->output != kiosk_seat->focused_surface->output)) {
		struct kiosk_shell_surface *successor;
		struct kiosk_shell_output *shoutput;

		successor = find_focus_successor(shsurf,
						 kiosk_seat->focused_surface);
		shoutput = shsurf->output;
		if (shoutput && successor) {
			enum weston_layer_position succesor_view_layer_pos;

			succesor_view_layer_pos = weston_shell_utils_view_get_layer_position(successor->view);
			if (succesor_view_layer_pos == WESTON_LAYER_POSITION_HIDDEN) {
				struct kiosk_shell_surface *shroot =
					kiosk_shell_surface_get_parent_root(successor);

				kiosk_shell_output_set_active_surface_tree(shoutput,
									   shroot);
			}
			kiosk_shell_surface_activate(successor, kiosk_seat,
						     WESTON_ACTIVATE_FLAG_NONE);
		} else {
			kiosk_seat->focused_surface = NULL;
			if (shoutput)
				kiosk_shell_output_set_active_surface_tree(shoutput,
									   NULL);
		}
	}

	kiosk_shell_surface_destroy(shsurf);
}

static void
desktop_surface_committed(struct weston_desktop_surface *desktop_surface,
			  struct weston_coord_surface buf_offset, void *data)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(desktop_surface);
	const char *app_id =
		weston_desktop_surface_get_app_id(desktop_surface);
	bool is_resized;
	bool is_fullscreen;

	assert(shsurf);

	if (surface->width == 0)
		return;

	if (!shsurf->appid_output_assigned && app_id) {
		struct kiosk_shell_output *shoutput = NULL;

		/* reset previous output being set in _added() as the output is
		 * being cached */
		shsurf->output = NULL;
		shoutput = kiosk_shell_surface_find_best_output(shsurf);

		kiosk_shell_surface_set_output(shsurf, shoutput);
		weston_desktop_surface_set_size(shsurf->desktop_surface,
						shoutput->output->width,
						shoutput->output->height);
		/* even if we couldn't find an appid set for a particular
		 * output still flag the shsurf as to a avoid changing the
		 * output every time */
		shsurf->appid_output_assigned = true;
	}

	/* TODO: When the top-level surface is committed with a new size after an
	 * output resize, sometimes the view appears scaled. What state are we not
	 * updating?
	 */

	is_resized = surface->width != shsurf->last_width ||
		     surface->height != shsurf->last_height;
	is_fullscreen = weston_desktop_surface_get_maximized(desktop_surface) ||
			weston_desktop_surface_get_fullscreen(desktop_surface);

	if (!weston_surface_is_mapped(surface) || (is_resized && is_fullscreen)) {
		if (is_fullscreen || !shsurf->xwayland.is_set) {
			weston_shell_utils_center_on_output(shsurf->view,
							    shsurf->output->output);
		} else {
			struct weston_coord_surface offset;
			struct weston_geometry geometry =
				weston_desktop_surface_get_geometry(desktop_surface);

			offset = weston_coord_surface(-geometry.x, -geometry.y,
						      shsurf->view->surface);
			weston_view_set_position_with_offset(shsurf->view,
							     shsurf->xwayland.pos,
							     offset);
		}

		weston_view_update_transform(shsurf->view);
	}

	if (!weston_surface_is_mapped(surface)) {
		struct weston_seat *seat =
			get_kiosk_shell_first_seat(shsurf->shell);
		struct kiosk_shell_output *shoutput = shsurf->output;
		struct kiosk_shell_seat *kiosk_seat;

		weston_surface_map(surface);

		kiosk_seat = get_kiosk_shell_seat(seat);

		/* We are mapping a new surface tree root; set it active,
		 * replacing the previous one */
		if (!shsurf->parent) {
			kiosk_shell_output_set_active_surface_tree(shoutput,
								   shsurf);
		}

		if (seat && kiosk_seat)
			kiosk_shell_surface_activate(shsurf, kiosk_seat,
						     WESTON_ACTIVATE_FLAG_NONE);
	}

	if (!is_fullscreen && (buf_offset.c.x != 0 || buf_offset.c.y != 0)) {
		struct weston_coord_global pos;

		pos = weston_view_get_pos_offset_global(shsurf->view);
		weston_view_set_position_with_offset(shsurf->view,
						     pos, buf_offset);
		weston_view_update_transform(shsurf->view);
	}

	shsurf->last_width = surface->width;
	shsurf->last_height = surface->height;
}

static void
desktop_surface_move(struct weston_desktop_surface *desktop_surface,
		     struct weston_seat *seat, uint32_t serial, void *shell)
{
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct weston_touch *touch = weston_seat_get_touch(seat);
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct weston_surface *surface =
		weston_desktop_surface_get_surface(shsurf->desktop_surface);
	struct weston_surface *focus;

	if (pointer &&
	    pointer->focus &&
	    pointer->button_count > 0 &&
	    pointer->grab_serial == serial) {
		focus = weston_surface_get_main_surface(pointer->focus->surface);
		if ((focus == surface) &&
		    (kiosk_shell_grab_start_for_pointer_move(shsurf, pointer) ==
			KIOSK_SHELL_GRAB_RESULT_ERROR))
			wl_resource_post_no_memory(surface->resource);
	}
	else if (touch &&
		 touch->focus &&
		 touch->grab_serial == serial) {
		focus = weston_surface_get_main_surface(touch->focus->surface);
		if ((focus == surface) &&
		    (kiosk_shell_grab_start_for_touch_move(shsurf, touch) ==
			KIOSK_SHELL_GRAB_RESULT_ERROR))
			wl_resource_post_no_memory(surface->resource);
	}
}

static void
desktop_surface_resize(struct weston_desktop_surface *desktop_surface,
		       struct weston_seat *seat, uint32_t serial,
		       enum weston_desktop_surface_edge edges, void *shell)
{
}

static void
desktop_surface_set_parent(struct weston_desktop_surface *desktop_surface,
			   struct weston_desktop_surface *parent,
			   void *shell)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct kiosk_shell_surface *shsurf_parent =
		parent ? weston_desktop_surface_get_user_data(parent) : NULL;

	kiosk_shell_surface_set_parent(shsurf, shsurf_parent);
}

static void
desktop_surface_fullscreen_requested(struct weston_desktop_surface *desktop_surface,
				     bool fullscreen,
				     struct weston_output *output, void *shell)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);
	struct kiosk_shell_output *shoutput = NULL;

	if (output)
		shoutput = weston_output_get_shell_private(output);

	/* We should normally be able to ignore fullscreen requests for
	 * top-level surfaces, since we set them as fullscreen at creation
	 * time. However, xwayland surfaces set their internal WM state
	 * regardless of what the shell wants, so they may remove fullscreen
	 * state before informing weston-desktop of this request. Since we
	 * always want top-level surfaces to be fullscreen, we need to reapply
	 * the fullscreen state to force the correct xwayland WM state.
	 *
	 * TODO: Explore a model where the XWayland WM doesn't set the internal
	 * WM surface state itself, rather letting the shell make the decision.
	 */

	if (!shsurf->parent || fullscreen)
		kiosk_shell_surface_set_fullscreen(shsurf, shoutput);
	else
		kiosk_shell_surface_set_normal(shsurf);
}

static void
desktop_surface_maximized_requested(struct weston_desktop_surface *desktop_surface,
				    bool maximized, void *shell)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	/* Since xwayland surfaces may have already applied the max/min states
	 * internally, reapply fullscreen to force the correct xwayland WM state.
	 * Also see comment in desktop_surface_fullscreen_requested(). */
	if (!shsurf->parent)
		kiosk_shell_surface_set_fullscreen(shsurf, NULL);
	else if (maximized)
		kiosk_shell_surface_set_maximized(shsurf);
	else
		kiosk_shell_surface_set_normal(shsurf);
}

static void
desktop_surface_minimized_requested(struct weston_desktop_surface *desktop_surface,
				    void *shell)
{
}

static void
desktop_surface_ping_timeout(struct weston_desktop_client *desktop_client,
			     void *shell_)
{
}

static void
desktop_surface_pong(struct weston_desktop_client *desktop_client,
		     void *shell_)
{
}

static void
desktop_surface_set_xwayland_position(struct weston_desktop_surface *desktop_surface,
				      struct weston_coord_global pos, void *shell)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	shsurf->xwayland.pos = pos;
	shsurf->xwayland.is_set = true;
}

static void
desktop_surface_get_position(struct weston_desktop_surface *desktop_surface,
			     int32_t *x, int32_t *y, void *shell)
{
	struct kiosk_shell_surface *shsurf =
		weston_desktop_surface_get_user_data(desktop_surface);

	*x = shsurf->view->geometry.pos_offset.x;
	*y = shsurf->view->geometry.pos_offset.y;
}

static const struct weston_desktop_api kiosk_shell_desktop_api = {
	.struct_size = sizeof(struct weston_desktop_api),
	.surface_added = desktop_surface_added,
	.surface_removed = desktop_surface_removed,
	.committed = desktop_surface_committed,
	.move = desktop_surface_move,
	.resize = desktop_surface_resize,
	.set_parent = desktop_surface_set_parent,
	.fullscreen_requested = desktop_surface_fullscreen_requested,
	.maximized_requested = desktop_surface_maximized_requested,
	.minimized_requested = desktop_surface_minimized_requested,
	.ping_timeout = desktop_surface_ping_timeout,
	.pong = desktop_surface_pong,
	.set_xwayland_position = desktop_surface_set_xwayland_position,
	.get_position = desktop_surface_get_position,
};

/*
 * kiosk_shell
 */

static void
kiosk_shell_activate_view(struct kiosk_shell *shell,
			  struct weston_view *view,
			  struct weston_seat *seat,
			  uint32_t flags)
{
	struct weston_surface *main_surface =
		weston_surface_get_main_surface(view->surface);
	struct kiosk_shell_surface *shsurf =
		get_kiosk_shell_surface(main_surface);
	struct kiosk_shell_seat *kiosk_seat =
		get_kiosk_shell_seat(seat);

	if (!shsurf || !kiosk_seat)
		return;

	kiosk_shell_surface_activate(shsurf, kiosk_seat, flags);
}

static void
kiosk_shell_click_to_activate_binding(struct weston_pointer *pointer,
				      const struct timespec *time,
				      uint32_t button, void *data)
{
	struct kiosk_shell *shell = data;

	if (pointer->grab != &pointer->default_grab)
		return;
	if (pointer->focus == NULL)
		return;

	kiosk_shell_activate_view(shell, pointer->focus, pointer->seat,
				  WESTON_ACTIVATE_FLAG_CLICKED);
}

static void
kiosk_shell_touch_to_activate_binding(struct weston_touch *touch,
				      const struct timespec *time,
				      void *data)
{
	struct kiosk_shell *shell = data;

	if (touch->grab != &touch->default_grab)
		return;
	if (touch->focus == NULL)
		return;

	kiosk_shell_activate_view(shell, touch->focus, touch->seat,
				  WESTON_ACTIVATE_FLAG_NONE);
}

static void
kiosk_shell_add_bindings(struct kiosk_shell *shell)
{
	uint32_t mod = 0;

	mod = weston_config_get_binding_modifier(shell->config, MODIFIER_SUPER);

	weston_compositor_add_button_binding(shell->compositor, BTN_LEFT, 0,
					     kiosk_shell_click_to_activate_binding,
					     shell);
	weston_compositor_add_button_binding(shell->compositor, BTN_RIGHT, 0,
					     kiosk_shell_click_to_activate_binding,
					     shell);
	weston_compositor_add_touch_binding(shell->compositor, 0,
					    kiosk_shell_touch_to_activate_binding,
					    shell);

	weston_install_debug_key_binding(shell->compositor, mod);
}

static void
kiosk_shell_handle_output_created(struct wl_listener *listener, void *data)
{
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, output_created_listener);
	struct weston_output *output = data;

	kiosk_shell_output_create(shell, output);
}

static void
kiosk_shell_handle_output_resized(struct wl_listener *listener, void *data)
{
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, output_resized_listener);
	struct weston_output *output = data;
	struct kiosk_shell_output *shoutput =
		weston_output_get_shell_private(output);
	struct weston_view *view;

	kiosk_shell_output_recreate_background(shoutput);

	wl_list_for_each(view, &shell->normal_layer.view_list.link,
			 layer_link.link) {
		struct kiosk_shell_surface *shsurf;
		if (view->output != output)
			continue;
		shsurf = get_kiosk_shell_surface(view->surface);
		if (!shsurf)
			continue;
		kiosk_shell_surface_reconfigure_for_output(shsurf);
	}
}

static void
kiosk_shell_handle_output_moved(struct wl_listener *listener, void *data)
{
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, output_moved_listener);
	struct weston_output *output = data;
	struct weston_view *view;

	wl_list_for_each(view, &shell->background_layer.view_list.link,
			 layer_link.link) {
		struct weston_coord_global pos;

		if (view->output != output)
			continue;

		pos = weston_coord_global_add(
		      weston_view_get_pos_offset_global(view),
		      output->move);
		weston_view_set_position(view, pos);
	}

	wl_list_for_each(view, &shell->normal_layer.view_list.link,
			 layer_link.link) {
		struct weston_coord_global pos;

		if (view->output != output)
			continue;

		pos = weston_coord_global_add(
		      weston_view_get_pos_offset_global(view),
		      output->move);
		weston_view_set_position(view, pos);
	}
}

static void
kiosk_shell_handle_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, seat_created_listener);
	kiosk_shell_seat_create(shell, seat);
}

static void
kiosk_shell_destroy_surfaces_on_layer(struct weston_layer *layer)
{
       struct weston_view *view, *view_next;

       wl_list_for_each_safe(view, view_next, &layer->view_list.link, layer_link.link) {
               struct kiosk_shell_surface *shsurf =
                       get_kiosk_shell_surface(view->surface);
               assert(shsurf);
               kiosk_shell_surface_destroy(shsurf);
       }

       weston_layer_fini(layer);
}

static void
kiosk_shell_destroy(struct wl_listener *listener, void *data)
{
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, destroy_listener);
	struct kiosk_shell_output *shoutput, *tmp;
	struct kiosk_shell_seat *shseat, *shseat_next;

	wl_list_remove(&shell->destroy_listener.link);
	wl_list_remove(&shell->output_created_listener.link);
	wl_list_remove(&shell->output_resized_listener.link);
	wl_list_remove(&shell->output_moved_listener.link);
	wl_list_remove(&shell->seat_created_listener.link);
	wl_list_remove(&shell->transform_listener.link);
	wl_list_remove(&shell->session_listener.link);

	wl_list_for_each_safe(shoutput, tmp, &shell->output_list, link) {
		kiosk_shell_output_destroy(shoutput);
	}

	/* bg layer doesn't contain a weston_desktop_surface, and
	 * kiosk_shell_output_destroy() takes care of destroying it, we're just
	 * doing a weston_layer_fini() here as there might be multiple bg views */
	weston_layer_fini(&shell->background_layer);
	kiosk_shell_destroy_surfaces_on_layer(&shell->normal_layer);
	kiosk_shell_destroy_surfaces_on_layer(&shell->inactive_layer);

	wl_list_for_each_safe(shseat, shseat_next, &shell->seat_list, link) {
		kiosk_shell_seat_destroy(shseat);
	}

	weston_desktop_destroy(shell->desktop);

	free(shell);
}

static void
kiosk_shell_notify_session(struct wl_listener *listener, void *data)
{
	struct kiosk_shell *shell =
		container_of(listener, struct kiosk_shell, session_listener);
	struct kiosk_shell_seat *k_seat;
	struct weston_compositor *compositor = data;
	struct weston_seat *seat = get_kiosk_shell_first_seat(shell);


	if (!compositor->session_active || !seat)
		return;

	k_seat = get_kiosk_shell_seat(seat);
	if (k_seat->focused_surface) {
		struct kiosk_shell_surface *current_focus =
			get_kiosk_shell_surface(k_seat->focused_surface);

		weston_view_activate_input(current_focus->view,
					   k_seat->seat,
					   WESTON_ACTIVATE_FLAG_NONE);
	}

}

WL_EXPORT int
wet_shell_init(struct weston_compositor *ec,
	       int *argc, char *argv[])
{
	struct kiosk_shell *shell;
	struct weston_seat *seat;
	struct weston_output *output;
	const char *config_file;

	shell = zalloc(sizeof *shell);
	if (shell == NULL)
		return -1;

	shell->compositor = ec;

	if (!weston_compositor_add_destroy_listener_once(ec,
							 &shell->destroy_listener,
							 kiosk_shell_destroy)) {
		free(shell);
		return 0;
	}

	shell->transform_listener.notify = transform_handler;
	wl_signal_add(&ec->transform_signal, &shell->transform_listener);

	config_file = weston_config_get_name_from_env();
	shell->config = weston_config_parse(config_file);

	weston_layer_init(&shell->background_layer, ec);
	weston_layer_init(&shell->normal_layer, ec);
	weston_layer_init(&shell->inactive_layer, ec);

	weston_layer_set_position(&shell->background_layer,
				  WESTON_LAYER_POSITION_BACKGROUND);
	weston_layer_set_position(&shell->inactive_layer,
				  WESTON_LAYER_POSITION_HIDDEN);
	/* We use the NORMAL layer position, so that xwayland surfaces, which
	 * are placed at NORMAL+1, are visible.  */
	weston_layer_set_position(&shell->normal_layer,
				  WESTON_LAYER_POSITION_NORMAL);

	shell->desktop = weston_desktop_create(ec, &kiosk_shell_desktop_api,
					       shell);
	if (!shell->desktop)
		return -1;

	wl_list_init(&shell->seat_list);
	wl_list_for_each(seat, &ec->seat_list, link)
		kiosk_shell_seat_create(shell, seat);
	shell->seat_created_listener.notify = kiosk_shell_handle_seat_created;
	wl_signal_add(&ec->seat_created_signal, &shell->seat_created_listener);

	wl_list_init(&shell->output_list);
	wl_list_for_each(output, &ec->output_list, link)
		kiosk_shell_output_create(shell, output);

	shell->output_created_listener.notify = kiosk_shell_handle_output_created;
	wl_signal_add(&ec->output_created_signal, &shell->output_created_listener);

	shell->output_resized_listener.notify = kiosk_shell_handle_output_resized;
	wl_signal_add(&ec->output_resized_signal, &shell->output_resized_listener);

	shell->output_moved_listener.notify = kiosk_shell_handle_output_moved;
	wl_signal_add(&ec->output_moved_signal, &shell->output_moved_listener);

	shell->session_listener.notify = kiosk_shell_notify_session;
	wl_signal_add(&ec->session_signal, &shell->session_listener);
	screenshooter_create(ec);

	kiosk_shell_add_bindings(shell);

	return 0;
}
