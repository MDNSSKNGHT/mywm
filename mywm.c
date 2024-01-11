/*
 * mywm.c
 *
 * This window manager was made using tinywl
 * as reference.
 */
#include <assert.h>
#include <getopt.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <xkbcommon/xkbcommon.h>

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define NOTIFY(n, f) (n).notify = f
#define LISTEN(p, e, n) wl_signal_add(&(p)->events.e, n)

struct mywm_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_listener new_xdg_decoration;

	struct wl_list clients;
	struct mywm_client* active_client;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_list keyboards;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
};

struct mywm_output {
	struct wl_list link;
	struct mywm_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct mywm_client {
	struct wl_list link;
	struct mywm_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	int width, height;
	int x, y;
};

struct mywm_keyboard {
	struct wl_list link;
	struct mywm_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

void output_frame(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output;
	struct timespec now;
	struct mywm_server *server;
	struct mywm_output *output;

	output = wl_container_of(listener, output, frame);
	server = output->server;
	scene_output = wlr_scene_get_scene_output(server->scene,
			output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

void output_request_state(struct wl_listener *listener, void *data) {
	struct wlr_output_event_request_state *event;
	struct mywm_output *output;

	event = data;
	output = wl_container_of(listener, output, request_state);

	wlr_output_commit_state(output->wlr_output, event->state);
}

void output_destroy(struct wl_listener *listener, void *data) {
	struct mywm_output *output;

	output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	free(output);
}

void new_output(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	struct wlr_output_layout_output *layout_output;
	struct wlr_scene_output *scene_output;
	struct mywm_server *server;
	struct mywm_output *output;

	wlr_output = data;
	server = wl_container_of(listener, server, new_output);

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	output = malloc(sizeof(struct mywm_output));
	output->server = server;
	output->wlr_output = wlr_output;

	NOTIFY(output->frame, output_frame);
	LISTEN(wlr_output, frame, &output->frame);

	NOTIFY(output->request_state, output_request_state);
	LISTEN(wlr_output, request_state, &output->request_state);

	NOTIFY(output->destroy, output_destroy);
	LISTEN(wlr_output, destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	layout_output = wlr_output_layout_add_auto(server->output_layout,
			wlr_output);
	scene_output = wlr_scene_output_create(server->scene, wlr_output);

	wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
			scene_output);
}

void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct wlr_seat *seat;
	struct wlr_keyboard_key_event *event;
	struct mywm_keyboard *keyboard;

	event = data;
	keyboard = wl_container_of(listener, keyboard, modifiers);
	seat = keyboard->server->seat;

	wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(seat,
			&keyboard->wlr_keyboard->modifiers);
}

void focus_client(struct mywm_client *client, struct wlr_surface *surface);

bool keyboard_keybinding(struct mywm_server *server, xkb_keysym_t sym) {
	struct mywm_client *prev_client;
	struct mywm_client *next_client;

	switch(sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_j:
		if (wl_list_length(&server->clients) < 2) {
			break;
		}
		if (server->active_client->link.next == &server->clients) {
			break;
		}
		next_client = wl_container_of(server->active_client->link.next,
				next_client, link);
		focus_client(next_client, next_client->xdg_surface->surface);
		break;
	case XKB_KEY_k:
		if (wl_list_length(&server->clients) < 2) {
			break;
		}
		if (server->active_client->link.next == &server->clients) {
			break;
		}
		prev_client = wl_container_of(server->active_client->link.prev,
				prev_client, link);
		focus_client(prev_client, prev_client->xdg_surface->surface);
		break;
	default:
		return false;
	}
	return true;
}

void keyboard_key(struct wl_listener *listener, void *data) {
	struct wlr_seat *seat;
	struct wlr_keyboard_key_event *event;
	const xkb_keysym_t *syms;
	struct mywm_keyboard *keyboard;
	unsigned int keycode;
	unsigned int modifiers;
	bool handled = false;
	int nsyms;

	event = data;
	keyboard = wl_container_of(listener, keyboard, key);
	seat = keyboard->server->seat;

	keycode = event->keycode + 8;
	nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
			keycode, &syms);
	modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if ((modifiers & WLR_MODIFIER_LOGO) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			handled = keyboard_keybinding(keyboard->server, syms[i]);
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
				event->keycode, event->state);
	}
}

void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct mywm_keyboard *keyboard;

	keyboard = wl_container_of(listener, keyboard, destroy);

	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);

	free(keyboard);
}

void input_new_keyboard(struct mywm_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard;
	struct xkb_context *context;
	struct xkb_rule_names rules = {0};
	struct xkb_keymap *keymap;
	struct mywm_keyboard *keyboard;

	wlr_keyboard = wlr_keyboard_from_input_device(device);
	keyboard = malloc(sizeof(struct mywm_keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	rules.layout = "latam";
	rules.variant = "deadtilde";
	keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	NOTIFY(keyboard->modifiers, keyboard_modifiers);
	LISTEN(wlr_keyboard, modifiers, &keyboard->modifiers);

	NOTIFY(keyboard->key, keyboard_key);
	LISTEN(wlr_keyboard, key, &keyboard->key);

	NOTIFY(keyboard->destroy, keyboard_destroy);
	LISTEN(device, destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);

	wl_list_insert(&server->keyboards, &keyboard->link);
}

void input_new_pointer(struct mywm_server *server,
		struct wlr_input_device *device) {}

void new_input(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device;
	struct mywm_server *server;
	unsigned int caps;

	device = data;
	server = wl_container_of(listener, server, new_input);

	switch(device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		input_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		input_new_pointer(server, device);
		break;
	default:
		break;
	}

	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

void resize_client(struct mywm_client *client, int x, int y,
		int width, int height) {
	wlr_xdg_toplevel_set_size(client->xdg_surface->toplevel, width, height);
	wlr_scene_node_set_position(&client->scene_tree->node, x, y);
	client->width = width;
	client->height = height;
	client->x = x;
	client->y = y;
}

void focus_client(struct mywm_client *client, struct wlr_surface *surface) {
	struct wlr_seat *seat;
	struct wlr_surface *prev_surface;
	struct wlr_xdg_toplevel *prev_toplevel;
	struct wlr_keyboard *keyboard;
	struct mywm_server *server;

	if (client == NULL) {
		return;
	}

	server = client->server;
	seat = server->seat;

	prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface != NULL) {
		prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}

	wlr_scene_node_raise_to_top(&client->scene_tree->node);

	wlr_xdg_toplevel_set_activated(client->xdg_surface->toplevel, true);

	keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, client->xdg_surface->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
	}
	server->active_client = client;
}

void tile(struct wlr_output_layout *output_layout, struct wl_list *clients) {
	struct wlr_output *wlr_output;
	struct mywm_client *client;
	unsigned int i, n, h, mw, my, ty;

	n = wl_list_length(clients);
	if (n == 0) {
		return;
	}

	wlr_output = wlr_output_layout_get_center_output(output_layout);

	if (n > 1) {
		mw = wlr_output->width * 0.55;
	} else {
		mw = wlr_output->width;
	}
	i = my = ty = 0;

	wl_list_for_each(client, clients, link) {
		if (i < 1) {
			h = (wlr_output->height - my) / (MIN(n, 1) - i);
			resize_client(client, 0, my, mw, h);
			if (my + client->height < wlr_output->height) {
				my += client->height;
			}
		} else {
			h = (wlr_output->height - ty) / (n - i);
			resize_client(client, mw, ty, wlr_output->width - mw, h);
			if (ty + client->height < wlr_output->height) {
				ty += client->height;
			}
		}
		i++;
	}
}

void surface_map(struct wl_listener *listener, void *data) {
	struct mywm_client *client;
	struct mywm_server *server;

	client = wl_container_of(listener, client, map);
	server = client->server;

	wl_list_insert(&server->clients, &client->link);

	tile(server->output_layout, &server->clients);

	focus_client(client, client->xdg_surface->surface);
}

void surface_unmap(struct wl_listener *listener, void *data) {
	struct mywm_client *prev_client, *client;
	struct mywm_server *server;

	client = wl_container_of(listener, client, unmap);
	server = client->server;

	wl_list_remove(&client->link);

	tile(server->output_layout, &server->clients);

	if (!wl_list_empty(&server->clients)) {
		prev_client = wl_container_of(server->clients.next, prev_client, link);
		focus_client(prev_client, prev_client->xdg_surface->surface);
	}
}

void surface_destroy(struct wl_listener *listener, void *data) {
	struct mywm_client *client;

	client = wl_container_of(listener, client, destroy);

	wl_list_remove(&client->map.link);
	wl_list_remove(&client->unmap.link);
	wl_list_remove(&client->destroy.link);

	free(client);
}

void new_xdg_surface(struct wl_listener *listener, void *data) {
	struct wlr_xdg_surface *xdg_surface;
	struct mywm_client *client;
	struct mywm_server *server;

	xdg_surface = data;
	server = wl_container_of(listener, server, new_xdg_surface);

	client = malloc(sizeof(struct mywm_client));
	client->server = server;
	client->xdg_surface = xdg_surface;
	client->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree,
			xdg_surface);
	client->scene_tree->node.data = client;
	xdg_surface->data = client->scene_tree;

	NOTIFY(client->map, surface_map);
	LISTEN(xdg_surface->surface, map, &client->map);

	NOTIFY(client->unmap, surface_unmap);
	LISTEN(xdg_surface->surface, unmap, &client->unmap);

	NOTIFY(client->destroy, surface_destroy);
	LISTEN(xdg_surface->surface, destroy, &client->destroy);
}

void new_xdg_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration;

	decoration = data;

	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

int main(int argc, char *argv[]) {
	struct mywm_server server = {0};
	const char *socket;
	const char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	server.wl_display = wl_display_create();

	server.backend = wlr_backend_autocreate(server.wl_display, NULL);
	assert(server.backend);

	server.renderer = wlr_renderer_autocreate(server.backend);
	assert(server.renderer);

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	assert(server.allocator);

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create();

	wl_list_init(&server.outputs);
	NOTIFY(server.new_output, new_output);
	LISTEN(server.backend, new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
			server.output_layout);

	wl_list_init(&server.clients);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	NOTIFY(server.new_xdg_surface, new_xdg_surface);
	LISTEN(server.xdg_shell, new_surface, &server.new_xdg_surface);

	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(server.wl_display),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server.xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(
			server.wl_display);
	NOTIFY(server.new_xdg_decoration, new_xdg_decoration);
	LISTEN(server.xdg_decoration_mgr, new_toplevel_decoration,
			&server.new_xdg_decoration);

	wl_list_init(&server.keyboards);

	NOTIFY(server.new_input, new_input);
	LISTEN(server.backend, new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");

	socket = wl_display_add_socket_auto(server.wl_display);
	assert(socket);

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd != NULL) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
		}
	}

	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wl_display_destroy(server.wl_display);
	return 0;
}
