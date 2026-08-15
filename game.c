// HyperMatch 3D C port from ../hyper-match-3d/index.js
// Uses the Tela unity build from src/index.c

// gcc -O3 -march=native -ffast-math -fopenmp -o app game.c -lSDL2 -lm && ./app

#include "src/index.c"

typedef enum {
	GAME_START = 0,
	GAME_LOOP = 1,
	GAME_END = 2,
} GameState;

typedef enum {
	RENDER_RAYTRACE = 0,
	RENDER_RASTER = 1,
} RenderMode;

typedef struct {
	// Keep RasterSphereProps-compatible prefix for renderer casts.
	Color color;
	Vec2 tex_coord;
	Tela* texture;
	Material* material;

	u32 id;
} GameSphereProps;

typedef struct {
	Array ids;  // u32 neighbors
} AdjList;

typedef struct {
	Window* window;
	Tela* tela;
	Tela* background_image;
	Tela* font_image;
	AudioTrack* music_loop;
	Scene scene;
	Camera camera;

	Mesh mesh;
	Sphere* spheres;
	GameSphereProps** props;
	u8* alive;
	AdjList* adjacency;
	u32 vertex_count;

	u8* color_index;
	u32* neighbors;
	u32 neighbors_count;

	u32 selected[2];
	u32 selected_count;

	bool right_click;
	Vec2 mouse;
	bool has_mouse;

	f32 percentage_to_win;
	u32 vertex_to_win;
	u32 vertex_matched;
	f32 final_time;

	f32 min_camera_radius;
	f32 max_camera_radius;
	f32 mouse_wheel_force;

	RenderMode render_mode;
	bool render_parallel;
	Material sphere_material;
	bool has_start_click;

	u64* cache_keys;
	Color* cache_colors;
	u32* cache_samples;
	u32 cache_size;

	GameState game_state;
	Loop* main_loop;
	bool running;
} Game;

static const Color COLOR_PALETTE[] = {
	{ 0.96f, 0.34f, 0.26f, 1.0f },
	{ 0.25f, 0.72f, 0.98f, 1.0f },
	{ 0.26f, 0.84f, 0.49f, 1.0f },
	{ 0.99f, 0.81f, 0.23f, 1.0f },
	{ 0.75f, 0.48f, 0.95f, 1.0f },
	{ 0.98f, 0.53f, 0.21f, 1.0f },
};

static const u32 COLOR_PALETTE_LEN = sizeof(COLOR_PALETTE) / sizeof(Color);

typedef struct {
	char c;
	Vec2 grid;
} FontCell;

static const FontCell FONT_MAP[] = {
	{ 'a', { 0, 12 } }, { 'b', { 1, 12 } }, { 'c', { 2, 12 } },
	{ 'd', { 0, 11 } }, { 'e', { 1, 11 } }, { 'f', { 2, 11 } },
	{ 'g', { 0, 10 } }, { 'h', { 1, 10 } }, { 'i', { 2, 10 } },
	{ 'j', { 0, 9 } },  { 'k', { 1, 9 } },  { 'l', { 2, 9 } },
	{ 'm', { 0, 8 } },  { 'n', { 1, 8 } },  { 'o', { 2, 8 } },
	{ 'p', { 0, 7 } },  { 'q', { 1, 7 } },  { 'r', { 2, 7 } },
	{ 's', { 0, 6 } },  { 't', { 1, 6 } },  { 'u', { 2, 6 } },
	{ 'v', { 0, 5 } },  { 'w', { 1, 5 } },  { 'x', { 2, 5 } },
	{ 'y', { 0, 4 } },  { 'z', { 1, 4 } },  { '0', { 2, 4 } },
	{ '1', { 0, 3 } },  { '2', { 1, 3 } },  { '3', { 2, 3 } },
	{ '4', { 0, 2 } },  { '5', { 1, 2 } },  { '6', { 2, 2 } },
	{ '7', { 0, 1 } },  { '8', { 1, 1 } },  { '9', { 2, 1 } },
	{ ':', { 0, 0 } },  { '!', { 1, 0 } },  { '.', { 1, 0 } },
};

static const u32 FONT_MAP_LEN = sizeof(FONT_MAP) / sizeof(FontCell);
static const f32 TRACE_CACHE_GRID = 0.005f;

static inline char to_lower_ascii(char c) {
	if (c >= 'A' && c <= 'Z')
		return (char)('a' + (c - 'A'));
	return c;
}

static inline bool get_font_grid(char c, Vec2* out_grid) {
	char lower = to_lower_ascii(c);
	for (u32 i = 0; i < FONT_MAP_LEN; i++) {
		if (FONT_MAP[i].c == lower) {
			*out_grid = FONT_MAP[i].grid;
			return true;
		}
	}
	return false;
}

static inline bool sample_text_image(
		Game* game,
		const char* text,
		f32 u,
		f32 v,
		Color* out_color
) {
	if (!game->font_image || !text || !out_color)
		return false;

	u32 len = (u32)strlen(text);
	if (len == 0)
		return false;
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return false;

	f32 fx = u * (f32)len;
	u32 char_index = (u32)floorf(fx);
	if (char_index >= len)
		char_index = len - 1;

	char c = text[char_index];
	if (c == ' ')
		return false;

	Vec2 grid = { 0 };
	if (!get_font_grid(c, &grid))
		return false;

	const f32 grid_x = 3.0f;
	const f32 grid_y = 13.0f;
	const f32 epsilon = 0.09f;
	f32 delta_x = (f32)game->font_image->width / grid_x;
	f32 delta_y = (f32)game->font_image->height / grid_y;

	f32 px = fx - (f32)char_index;
	f32 py = v;

	f32 box_min_x = (grid.x + epsilon) * delta_x;
	f32 box_min_y = (grid.y + epsilon) * delta_y;
	f32 box_w = (1.0f - 2.0f * epsilon) * delta_x;
	f32 box_h = (1.0f - 2.0f * epsilon) * delta_y;

	f32 sx = box_min_x + box_w * px;
	f32 sy = box_min_y + box_h * py;

	Color sampled = get_pxl_tela(game->font_image, (u32)sx, (u32)sy);
	if (sampled.red >= 0.999f)
		return false;

	*out_color =
			(Color){ 1.0f - sampled.red, 1.0f - sampled.green, 1.0f - sampled.blue, 1.0f };
	return true;
}

static inline void draw_text_from_font_image(
		Game* game,
		const char* text,
		u32 min_x,
		u32 min_y,
		u32 max_x,
		u32 max_y,
		Color tint
) {
	if (!text || max_x <= min_x || max_y <= min_y)
		return;

	f32 w = (f32)(max_x - min_x);
	f32 h = (f32)(max_y - min_y);
	for (u32 y = min_y; y < max_y; y++) {
		for (u32 x = min_x; x < max_x; x++) {
			f32 u = ((f32)(x - min_x)) / w;
			f32 v = ((f32)(y - min_y)) / h;
			Color text_color;
			if (!sample_text_image(game, text, u, v, &text_color))
				continue;
			set_pxl_tela(game->tela, x, y, mul_color(text_color, tint));
		}
	}
}

static inline bool is_neighbor_of(Game* game, u32 src, u32 target) {
	if (src >= game->vertex_count || target >= game->vertex_count)
		return false;
	AdjList* list = &game->adjacency[src];
	for (u32 i = 0; i < list->ids.length; i++) {
		u32* id = (u32*)get_array_element(&list->ids, i);
		if (id && *id == target) {
			return true;
		}
	}
	return false;
}

static inline void clear_neighbors(Game* game) {
	game->neighbors_count = 0;
}

static inline void add_neighbor_unique(AdjList* list, u32 id) {
	for (u32 i = 0; i < list->ids.length; i++) {
		u32* existing = (u32*)get_array_element(&list->ids, i);
		if (existing && *existing == id) {
			return;
		}
	}
	push_array(&list->ids, &id);
}

static inline void build_adjacency(Game* game) {
	game->adjacency = (AdjList*)calloc(game->vertex_count, sizeof(AdjList));
	for (u32 i = 0; i < game->vertex_count; i++) {
		game->adjacency[i].ids = new_array(8, sizeof(u32));
	}

	for (u32 i = 0; i < game->mesh.faces.length; i++) {
		Face* fi = (Face*)get_array_element(&game->mesh.faces, i);
		if (!fi)
			continue;
		for (u32 j = i + 1; j < game->mesh.faces.length; j++) {
			Face* fj = (Face*)get_array_element(&game->mesh.faces, j);
			if (!fj)
				continue;
			u32 common = 0;
			for (u32 a = 0; a < 3; a++) {
				for (u32 b = 0; b < 3; b++) {
					if (fi->vertex_indices[a] == fj->vertex_indices[b]) {
						common++;
					}
				}
			}
			if (common >= 2) {
				add_neighbor_unique(&game->adjacency[i], j);
				add_neighbor_unique(&game->adjacency[j], i);
			}
		}
	}
}

static inline void normalize_vertices_like_js(Array* vertices) {
	if (!vertices || vertices->length == 0)
		return;

	Vec3 min_v = vec3(INFINITY, INFINITY, INFINITY);
	Vec3 max_v = vec3(-INFINITY, -INFINITY, -INFINITY);
	for (u32 i = 0; i < vertices->length; i++) {
		Vec3* v = (Vec3*)get_array_element(vertices, i);
		if (!v)
			continue;
		min_v.x = fminf(min_v.x, v->x);
		min_v.y = fminf(min_v.y, v->y);
		min_v.z = fminf(min_v.z, v->z);
		max_v.x = fmaxf(max_v.x, v->x);
		max_v.y = fmaxf(max_v.y, v->y);
		max_v.z = fmaxf(max_v.z, v->z);
	}

	Vec3 diagonal = sub_vec3(max_v, min_v);
	Vec3 center = add_vec3(min_v, scale_vec3(diagonal, 0.5f));
	f32 scale = fmaxf(diagonal.x, fmaxf(diagonal.y, diagonal.z));
	if (scale <= 1e-8f)
		scale = 1.0f;

	for (u32 i = 0; i < vertices->length; i++) {
		Vec3* v = (Vec3*)get_array_element(vertices, i);
		if (!v)
			continue;
		Vec3 n = scale_vec3(sub_vec3(*v, center), 1.0f / scale);
		// Match JS normalizeVertices: Vec3(-v.x, v.z, v.y)
		*v = vec3(-n.x, n.z, n.y);
	}
}

static inline f32 color_distance(Color a, Color b) {
	f32 dr = a.red - b.red;
	f32 dg = a.green - b.green;
	f32 db = a.blue - b.blue;
	return sqrtf(dr * dr + dg * dg + db * db);
}

static inline u32 closest_palette_color(Color c) {
	u32 idx = 0;
	f32 best = INFINITY;
	for (u32 i = 0; i < COLOR_PALETTE_LEN; i++) {
		f32 d = color_distance(c, COLOR_PALETTE[i]);
		if (d < best) {
			best = d;
			idx = i;
		}
	}
	return idx;
}

static inline void apply_visual_state(Game* game) {
	for (u32 i = 0; i < game->vertex_count; i++) {
		GameSphereProps* p = game->props[i];
		if (!p)
			continue;

		Color base = COLOR_PALETTE[game->color_index[i] % COLOR_PALETTE_LEN];
		p->color = base;
		if (!game->alive[i]) {
			p->color = COLOR_BLACK;
		}
	}

	for (u32 i = 0; i < game->neighbors_count; i++) {
		u32 id = game->neighbors[i];
		if (id >= game->vertex_count || !game->alive[id])
			continue;
		game->props[id]->color = lerp_color(game->props[id]->color, COLOR_WHITE, 0.4f);
	}

	for (u32 i = 0; i < game->selected_count; i++) {
		u32 id = game->selected[i];
		if (id >= game->vertex_count || !game->alive[id])
			continue;
		game->props[id]->color = COLOR_WHITE;
	}
}

static inline void rebuild_scene_from_alive(Game* game) {
	clear_scene_elems_scene(&game->scene);
	for (u32 i = 0; i < game->vertex_count; i++) {
		if (!game->alive[i])
			continue;
		SceneElem elem = build_scene_elem_sphere(game->spheres[i]);
		add_scene_elem_scene(&game->scene, elem);
	}
	game->scene.vtable->rebuild_scene(&game->scene);
}

static inline void set_neighbors_from_selected(Game* game, u32 id) {
	clear_neighbors(game);
	if (id >= game->vertex_count)
		return;

	AdjList* list = &game->adjacency[id];
	for (u32 i = 0; i < list->ids.length; i++) {
		u32* n = (u32*)get_array_element(&list->ids, i);
		if (!n || *n >= game->vertex_count)
			continue;
		if (!game->alive[*n])
			continue;
		game->neighbors[game->neighbors_count++] = *n;
	}
}

static inline void reset_exposure(Game* game);

static inline void reset_selection(Game* game) {
	game->selected_count = 0;
	clear_neighbors(game);
	reset_exposure(game);
}

static inline void reset_exposure(Game* game) {
	if (game && game->tela) {
		game->tela->iterations = 1;
	}
}

static inline Vec2 window_to_canvas_coords(Game* game, i32 x, i32 y) {
	if (!game || !game->window || !game->tela) {
		return vec2((f32)x, (f32)y);
	}
	f32 sx = (f32)game->tela->width / (f32)game->window->width;
	f32 sy = (f32)game->tela->height / (f32)game->window->height;
	f32 cx = clamp((f32)x * sx, 0.0f, (f32)game->tela->width - 1.0f);
	f32 cy = clamp((f32)y * sy, 0.0f, (f32)game->tela->height - 1.0f);
	return vec2(cx, cy);
}

static inline f32 get_min_camera_radius(Game* game) {
	const u32 iterations = 50;
	u32 samples = 0;
	f32 w = (f32)game->tela->width;
	f32 h = (f32)game->tela->height;
	f32 alpha = 2.0f;
	f32 range_x = w / alpha;
	f32 range_y = h / alpha;
	f32 center_x = w / 2.0f;
	f32 center_y = h / 2.0f;
	f32 camera_to_surface_avg_distance = 0.0f;

	for (u32 i = 0; i < iterations; i++) {
		f32 x = center_x + (f32)random_double() * range_x;
		f32 y = center_y + (f32)random_double() * range_y;
		Ray ray = ray_from_tela_camera(
				&game->camera,
				game->tela,
				(u32)clamp(x, 0.0f, w - 1.0f),
				(u32)clamp(y, 0.0f, h - 1.0f)
		);
		SceneHit hit = intersect_scene(&game->scene, ray);
		if (hit.hit) {
			camera_to_surface_avg_distance += hit.t;
			samples++;
		}
	}

	if (samples == 0) {
		return 0.01f;
	}

	camera_to_surface_avg_distance /= (f32)samples;
	f32 radius = length_vec3(game->camera.position);
	f32 distance_to_surface = radius - camera_to_surface_avg_distance;
	f32 golden_ratio = 1.618033988749f;
	return fmaxf(0.01f, golden_ratio * distance_to_surface);
}

static inline void swap_selected_colors(Game* game) {
	if (game->selected_count != 2)
		return;
	u32 a = game->selected[0];
	u32 b = game->selected[1];
	u8 tmp = game->color_index[a];
	game->color_index[a] = game->color_index[b];
	game->color_index[b] = tmp;
}

static inline u32 find_match(Game* game, u32 start_id, u32* out_ids) {
	if (start_id >= game->vertex_count || !game->alive[start_id])
		return 0;

	u8 base_color = game->color_index[start_id];
	u8* visited = (u8*)calloc(game->vertex_count, sizeof(u8));
	u32* stack = (u32*)malloc(game->vertex_count * sizeof(u32));
	u32 stack_size = 0;
	u32 out_count = 0;

	visited[start_id] = 1;
	out_ids[out_count++] = start_id;

	AdjList* root_neighbors = &game->adjacency[start_id];
	for (u32 i = 0; i < root_neighbors->ids.length; i++) {
		u32* n = (u32*)get_array_element(&root_neighbors->ids, i);
		if (n && *n < game->vertex_count) {
			stack[stack_size++] = *n;
		}
	}

	while (stack_size > 0) {
		u32 id = stack[--stack_size];
		if (id >= game->vertex_count || visited[id])
			continue;
		visited[id] = 1;
		if (!game->alive[id])
			continue;
		if (game->color_index[id] != base_color)
			continue;

		out_ids[out_count++] = id;

		AdjList* nlist = &game->adjacency[id];
		for (u32 i = 0; i < nlist->ids.length; i++) {
			u32* n = (u32*)get_array_element(&nlist->ids, i);
			if (!n || *n >= game->vertex_count)
				continue;
			if (!visited[*n]) {
				stack[stack_size++] = *n;
			}
		}
	}

	free(visited);
	free(stack);

	if (out_count < 3)
		return 0;
	return out_count;
}

static inline void remove_matches_for_selected(Game* game) {
	u32* matched = (u32*)malloc(game->vertex_count * sizeof(u32));
	for (u32 i = 0; i < game->selected_count; i++) {
		u32 id = game->selected[i];
		u32 count = find_match(game, id, matched);
		for (u32 k = 0; k < count; k++) {
			u32 rid = matched[k];
			if (rid >= game->vertex_count || !game->alive[rid])
				continue;
			game->alive[rid] = 0;
			game->vertex_matched++;
		}
	}
	free(matched);
}

static Color render_background_ray(Ray ray, void* context) {
	Tela* background = (Tela*)context;
	if (!background)
		return COLOR_BLACK;
	Vec3 d = normalize_vec3(ray.dir);
	f32 u = atan2f(d.y, d.x) / (2.0f * PI) + 0.5f;
	f32 v = acosf(-clamp(d.z, -1.0f, 1.0f)) / PI;
	return get_tex_color(background, vec2(u, v));
}

typedef struct {
	Game* game;
	u32 bounces;
} FastTraceCtx;

static inline u64 hash_position(Vec3 p) {
	i32 xi = (i32)floorf(p.x / TRACE_CACHE_GRID);
	i32 yi = (i32)floorf(p.y / TRACE_CACHE_GRID);
	i32 zi = (i32)floorf(p.z / TRACE_CACHE_GRID);
	u64 h = (u64)(xi * 92837111) ^ (u64)(yi * 689287499) ^ (u64)(zi * 283923481);
	return h;
}

static inline bool cache_get_color(Game* game, Vec3 p, Color* out) {
	if (!game->cache_keys || !game->cache_colors || game->cache_size == 0)
		return false;
	if (random_double() >= 0.5)
		return false;

	u64 h = hash_position(p);
	u32 index = (u32)(h & (game->cache_size - 1));
	bool found = false;
#pragma omp critical (trace_cache)
	{
		if (game->cache_keys[index] == h) {
			*out = game->cache_colors[index];
			found = true;
		}
	}
	return found;
}

static inline void cache_set_color(Game* game, Vec3 p, Color c) {
	if (!game->cache_keys || !game->cache_colors || !game->cache_samples ||
			game->cache_size == 0)
		return;

	u64 h = hash_position(p);
	u32 index = (u32)(h & (game->cache_size - 1));
	bool updated = false;
#pragma omp critical (trace_cache)
	{
		u32 samples = game->cache_samples[index];
		if (game->cache_keys[index] == h && samples > 0) {
			u32 new_samples = samples < 100000 ? samples + 1 : samples;
			Color old = game->cache_colors[index];
			game->cache_colors[index] =
					(Color){ old.red + (c.red - old.red) / (f32)new_samples,
						  old.green + (c.green - old.green) / (f32)new_samples,
						  old.blue + (c.blue - old.blue) / (f32)new_samples,
						  1.0f };
			game->cache_samples[index] = new_samples;
			updated = true;
		} else {
			game->cache_keys[index] = h;
			game->cache_colors[index] = c;
			game->cache_samples[index] = 1;
		}
	}
	(void)updated;
}

static inline bool is_selected_id(Game* game, u32 id) {
	for (u32 i = 0; i < game->selected_count; i++) {
		if (game->selected[i] == id)
			return true;
	}
	return false;
}

static inline bool is_neighbor_id(Game* game, u32 id) {
	for (u32 i = 0; i < game->neighbors_count; i++) {
		if (game->neighbors[i] == id)
			return true;
	}
	return false;
}

static Color trace_fast_ray(Ray ray, FastTraceCtx* ctx, i32 bounces) {
	if (bounces < 0)
		return COLOR_BLACK;

	Game* game = ctx->game;
	SceneHit hit = intersect_scene(&game->scene, ray);
	if (!hit.hit) {
		return render_background_ray(ray, game->background_image);
	}

	if (hit.scene_elem->geometry_type != SPHERE) {
		return COLOR_BLACK;
	}

	Sphere* s = &hit.scene_elem->as.sphere;
	GameSphereProps* p = (GameSphereProps*)s->props;
	Color color = p ? p->color : COLOR_BLACK;
	u32 id = p ? p->id : 0;

	if (p && is_selected_id(game, id)) {
		Vec3 r = normalize_vec3(sub_vec3(hit.position, s->position));
		f32 d = fabsf(dot_vec3(ray.dir, r));
		d = d * d;
		return (Color){ (1.0f - d) + d * color.red,
						  (1.0f - d) + d * color.green,
						  (1.0f - d) + d * color.blue,
						  1.0f };
	}

	if (p && is_neighbor_id(game, id)) {
		return color;
	}

	Color cached;
	if (cache_get_color(game, hit.position, &cached)) {
		return cached;
	}

	Vec3 normal = normal_to_point_sphere(s, hit.position);
	Vec3 random_sphere_vec = random_point_in_sphere();
	Vec3 origin = add_vec3(hit.position, scale_vec3(normal, 1e-2f));
	Ray scattered = dot_vec3(random_sphere_vec, normal) >= 0
						? build_ray(origin, random_sphere_vec)
						: build_ray(origin, scale_vec3(random_sphere_vec, -1.0f));

	Color final_c = trace_fast_ray(scattered, ctx, bounces - 1);
	Color final_color = (Color){ final_c.red + final_c.red * color.red,
					  final_c.green + final_c.green * color.green,
					  final_c.blue + final_c.blue * color.blue,
					  1.0f };
	cache_set_color(game, hit.position, final_color);
	return final_color;
}

static Color trace_fast_lambda(Ray ray, void* context) {
	FastTraceCtx* ctx = (FastTraceCtx*)context;
	return trace_fast_ray(ray, ctx, (i32)ctx->bounces);
}

static inline void render_start(Game* game, f32 time) {
	ray_map_camera(
			&game->camera, game->tela, render_background_ray, game->background_image
	);
	const u32 panel_min_x = game->tela->width / 10;
	const u32 panel_max_x = 9 * game->tela->width / 10;
	const u32 panel_min_y = game->tela->height / 9;
	const u32 panel_max_y = 7 * game->tela->height / 9;
	f32 pulse = 0.15f + 0.15f * (sinf(2.0f * time) + 1.0f);
	Color overlay = { pulse, pulse, pulse, 1.0f };

	for (u32 y = panel_min_y; y < panel_max_y; y++) {
		for (u32 x = panel_min_x; x < panel_max_x; x++) {
			Color c = get_pxl_tela(game->tela, x, y);
			set_pxl_tela(game->tela, x, y, lerp_color(c, overlay, 0.35f));
		}
	}

	Color title_tint = COLOR_WHITE;
	Color btn_tint = { 0.1f, fabsf(sinf(time)), fabsf(cosf(time)), 1.0f };
	if (game->has_start_click) {
		btn_tint = (Color){ 0.9f, 0.8f, 0.1f, 1.0f };
	}

	draw_text_from_font_image(
			game,
			"HyperMatch 3D",
			game->tela->width / 10,
			5 * game->tela->height / 9,
			9 * game->tela->width / 10,
			7 * game->tela->height / 9,
			title_tint
	);
	draw_text_from_font_image(
			game,
			"Start Demo",
			3 * game->tela->width / 10,
			game->tela->height / 9,
			7 * game->tela->width / 10,
			3 * game->tela->height / 9,
			btn_tint
	);

	paint_window(game->window, game->tela);
	set_window_title(game->window, "HyperMatch 3D | Click or Enter to Start");
}

static inline u32 compute_score(Game* game) {
	if (game->final_time <= 0.0f)
		return 0;
	f32 ratio = (f32)game->vertex_matched / (f32)game->vertex_to_win;
	return (u32)floorf(1000000.0f * ratio / game->final_time);
}

static inline void render_end(Game* game) {
	ray_map_camera(
			&game->camera, game->tela, render_background_ray, game->background_image
	);
	draw_text_from_font_image(
			game,
			"Finished!",
			game->tela->width / 10,
			5 * game->tela->height / 9,
			9 * game->tela->width / 10,
			7 * game->tela->height / 9,
			COLOR_WHITE
	);
	char score_text[64];
	snprintf(score_text, sizeof(score_text), "Score: %u", compute_score(game));
	draw_text_from_font_image(
			game,
			score_text,
			0,
			game->tela->height / 9,
			game->tela->width,
			3 * game->tela->height / 9,
			COLOR_WHITE
	);
	paint_window(game->window, game->tela);
	char* title = format_string(
			"HyperMatch 3D | Finished | Score: %u | Press Enter to Restart",
			compute_score(game)
	);
	if (title) {
		set_window_title(game->window, title);
		free(title);
	}
}

static inline void update_title(Game* game) {
	char* title = format_string(
			"HyperMatch 3D | Vertex Matched: %u/%u | Time: %u",
			game->vertex_matched,
			game->vertex_to_win,
			(u32)floorf(game->final_time)
	);
	if (title) {
		set_window_title(game->window, title);
		free(title);
	}
}

static inline void start_or_restart(Game* game) {
	game->final_time = 0.0f;
	game->vertex_matched = 0;
	reset_selection(game);
	reset_exposure(game);

	for (u32 i = 0; i < game->vertex_count; i++) {
		game->alive[i] = 1;
		game->color_index[i] = (u8)(rand() % COLOR_PALETTE_LEN);
	}

	rebuild_scene_from_alive(game);
	reset_exposure(game);
	game->game_state = GAME_LOOP;
}

static void on_close(Window* window, void* context) {
	(void)window;
	Game* game = (Game*)context;
	game->running = false;
	if (game->main_loop) {
		stop_loop(game->main_loop);
	}
}

static void on_mouse_down(Window* window, i32 x, i32 y, u32 button, void* context) {
	(void)window;
	Game* game = (Game*)context;
	Vec2 canvas_mouse = window_to_canvas_coords(game, x, y);
	i32 cx = (i32)canvas_mouse.x;
	i32 cy = (i32)canvas_mouse.y;

	game->mouse = vec2((f32)x, (f32)y);
	game->has_mouse = true;
	if (button == SDL_BUTTON_RIGHT) {
		game->right_click = true;
		return;
	}

	if (game->game_state == GAME_START) {
		i32 min_x = (i32)(3 * game->tela->width / 10);
		i32 max_x = (i32)(7 * game->tela->width / 10);
		i32 min_y = (i32)(game->tela->height / 9);
		i32 max_y = (i32)(3 * game->tela->height / 9);
		game->has_start_click =
				cx >= min_x && cx <= max_x && cy >= min_y && cy <= max_y;
		if (game->has_start_click || button == SDL_BUTTON_LEFT) {
			start_or_restart(game);
		}
		return;
	}

	if (game->game_state == GAME_END) {
		return;
	}

	Ray ray = ray_from_tela_camera(&game->camera, game->tela, (u32)cx, (u32)cy);
	SceneHit hit = intersect_scene(&game->scene, ray);

	if (!hit.hit || hit.scene_elem->geometry_type != SPHERE) {
		reset_selection(game);
		reset_exposure(game);
		return;
	}

	GameSphereProps* props = (GameSphereProps*)hit.scene_elem->as.sphere.props;
	if (!props || props->id >= game->vertex_count || !game->alive[props->id]) {
		reset_selection(game);
		reset_exposure(game);
		return;
	}

	u32 hit_id = props->id;

	if (game->selected_count == 0) {
		game->selected[0] = hit_id;
		game->selected_count = 1;
		set_neighbors_from_selected(game, hit_id);
		reset_exposure(game);
		return;
	}

	if (game->selected_count == 1) {
		if (hit_id == game->selected[0]) {
			reset_selection(game);
			reset_exposure(game);
			return;
		}
		if (is_neighbor_of(game, game->selected[0], hit_id)) {
			game->selected[1] = hit_id;
			game->selected_count = 2;
			reset_exposure(game);
			return;
		}
		game->selected[0] = hit_id;
		game->selected_count = 1;
		set_neighbors_from_selected(game, hit_id);
		reset_exposure(game);
	}
}

static void on_mouse_up(Window* window, i32 x, i32 y, u32 button, void* context) {
	(void)window;
	(void)x;
	(void)y;
	(void)button;
	Game* game = (Game*)context;
	game->right_click = false;
	game->has_mouse = false;
}

static void on_mouse_move(Window* window, i32 x, i32 y, void* context) {
	Game* game = (Game*)context;
	if (!game->right_click) {
		return;
	}

	Vec2 new_mouse = vec2((f32)x, (f32)y);
	if (!game->has_mouse) {
		game->mouse = new_mouse;
		game->has_mouse = true;
		return;
	}

	if (equals_vec2(new_mouse, game->mouse)) {
		return;
	}

	Vec2 delta = sub_vec2(new_mouse, game->mouse);
	Vec3 orbit = get_camera_orbit(&game->camera);

	f32 dtheta = -2.0f * PI * (delta.x / (f32)window->width);
	f32 dphi = -2.0f * PI * (delta.y / (f32)window->height);

	f32 min_camera_radius = get_min_camera_radius(game);
	f32 radius = clamp(orbit.x, min_camera_radius, game->max_camera_radius);
	f32 theta = orbit.y + dtheta;
	f32 phi = clamp(orbit.z + dphi, -1.45f, 1.45f);
	set_orbit_camera(&game->camera, radius, theta, phi);
	reset_exposure(game);

	game->mouse = new_mouse;
}

static void on_mouse_scroll(Window* window, i32 scroll_y, void* context) {
	(void)window;
	Game* game = (Game*)context;
	Vec3 orbit = get_camera_orbit(&game->camera);
	f32 min_camera_radius = get_min_camera_radius(game);
	f32 radius = orbit.x + (f32)scroll_y * game->mouse_wheel_force;
	radius = clamp(radius, min_camera_radius, game->max_camera_radius);
	set_orbit_camera(&game->camera, radius, orbit.y, orbit.z);
	reset_exposure(game);
}

static void on_key_down(Window* window, u32 keycode, void* context) {
	(void)window;
	Game* game = (Game*)context;

	if (keycode == SDLK_ESCAPE) {
		game->running = false;
		if (game->main_loop) {
			stop_loop(game->main_loop);
		}
		return;
	}

	if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
		if (game->game_state != GAME_LOOP) {
			start_or_restart(game);
		}
	}
}

static void tick(f32 dt, f32 time, void* context) {
	Game* game = (Game*)context;
	(void)time;
	audio_track_update(game->music_loop);

	if (!game->running) {
		stop_loop(game->main_loop);
		return;
	}

	if (game->game_state == GAME_START) {
		render_start(game, time);
		return;
	}

	if (game->game_state == GAME_END) {
		render_end(game);
		return;
	}

	game->final_time += dt;

	if (game->selected_count == 1) {
		set_neighbors_from_selected(game, game->selected[0]);
	}

	if (game->selected_count == 2) {
		swap_selected_colors(game);
		remove_matches_for_selected(game);
		rebuild_scene_from_alive(game);
		reset_selection(game);
	}

	if (game->vertex_matched > game->vertex_to_win) {
		game->game_state = GAME_END;
		render_end(game);
		return;
	}

	apply_visual_state(game);
	if (game->render_mode == RENDER_RAYTRACE) {
		FastTraceCtx params = { .game = game, .bounces = 1 };
		if (game->render_parallel) {
			ray_map_camera_exposed_parallel(
					&game->camera,
					game->tela,
					trace_fast_lambda,
					&params
			);
		} else {
			ray_map_camera_exposed(&game->camera, game->tela, trace_fast_lambda, &params);
		}
	} else {
		ray_map_camera(
				&game->camera,
				game->tela,
				render_background_ray,
				game->background_image
		);
		raster_scene(
				&game->scene,
				(RasterParams){
						.clear_screen = false,
						.camera = &game->camera,
						.tela = game->tela,
				}
		);
	}
	paint_window(game->window, game->tela);
	update_title(game);
}

static inline void free_game(Game* game) {
	if (!game)
		return;

	if (game->music_loop) {
		free_audio_track(game->music_loop);
		game->music_loop = NULL;
	}

	if (game->main_loop) {
		free_loop(game->main_loop);
		game->main_loop = NULL;
	}

	if (game->props) {
		for (u32 i = 0; i < game->vertex_count; i++) {
			if (game->props[i]) {
				free(game->props[i]);
			}
		}
		free(game->props);
	}

	if (game->adjacency) {
		for (u32 i = 0; i < game->vertex_count; i++) {
			free_array(&game->adjacency[i].ids);
		}
		free(game->adjacency);
	}

	free(game->neighbors);
	free(game->alive);
	free(game->color_index);
	free(game->spheres);
	free(game->cache_keys);
	free(game->cache_colors);
	free(game->cache_samples);

	free_array(&game->mesh.vertices);
	free_array(&game->mesh.tex_coords);
	free_array(&game->mesh.normals);
	free_array(&game->mesh.faces);
	if (game->mesh.colors.data)
		free_array(&game->mesh.colors);
	if (game->mesh.materials.data)
		free_array(&game->mesh.materials);

	if (game->background_image) {
		free_tela(game->background_image);
	}
	if (game->font_image) {
		free_tela(game->font_image);
	}
	if (game->tela) {
		free_tela(game->tela);
	}
	if (game->window) {
		free_window(game->window);
	}

	if (game->scene.vtable) {
		free_scene(&game->scene);
	}
}

int main(int argc, char** argv) {
	srand((unsigned int)time(NULL));

	Game game = { 0 };
	game.percentage_to_win = 0.10f;
	game.max_camera_radius = 2.0f;
	game.mouse_wheel_force = 0.05f;
	game.running = true;
	game.game_state = GAME_START;
	game.render_mode = RENDER_RAYTRACE;
	game.render_parallel = true;
	game.sphere_material = build_diffuse_material();
	game.has_start_click = false;
	game.cache_size = 1u << 20;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) {
			game.render_mode = RENDER_RASTER;
		} else if (strcmp(argv[i], "-s") == 0) {
			game.render_parallel = false;
		}
	}

	const i32 render_width = 640;
	const i32 render_height = 480;
	const i32 window_scale = 2;
	const i32 window_width = render_width * window_scale;
	const i32 window_height = render_height * window_scale;

	game.window = new_window(window_width, window_height, "HyperMatch 3D");
	if (!game.window || !game.window->sdl_window) {
		fprintf(stderr, "Failed to initialize window.\n");
		return 1;
	}

	game.tela = new_tela((u32)render_width, (u32)render_height);
	if (!game.tela) {
		fprintf(stderr, "Failed to create Tela canvas.\n");
		free_window(game.window);
		return 1;
	}

	game.background_image = io_read_image("./assets/index.ppm");
	if (!game.background_image) {
		game.background_image = io_read_image("./assets/index.jpg");
	}
	game.font_image = io_read_image("./assets/fonts.ppm");
	if (!game.font_image) {
		game.font_image = io_read_image("./assets/fonts.png");
	}

	game.music_loop = audio_track_from_wav("./assets/index.wav", true);
	if (game.music_loop) {
		audio_track_play(game.music_loop);
	} else {
		fprintf(stderr, "Warning: could not load background music WAV.\n");
	}

	String obj_str = io_read_file("./assets/index.obj");
	if (!obj_str.data) {
		fprintf(stderr, "Could not load OBJ file: ./assets/index.obj\n");
		free_game(&game);
		return 1;
	}
	game.mesh = read_obj_mesh(obj_str, "mesh");
	free(obj_str.data);
	normalize_vertices_like_js(&game.mesh.vertices);

	game.vertex_count = game.mesh.faces.length;
	if (game.vertex_count == 0) {
		fprintf(stderr, "OBJ has no faces.\n");
		free_game(&game);
		return 1;
	}

	AABB bbox = get_bounding_box_mesh(&game.mesh);
	(void)bbox;
	f32 sphere_radius = 0.025f;
	game.min_camera_radius = 0.01f;

	game.spheres = (Sphere*)malloc(game.vertex_count * sizeof(Sphere));
	game.props = (GameSphereProps**)malloc(game.vertex_count * sizeof(GameSphereProps*));
	game.alive = (u8*)malloc(game.vertex_count * sizeof(u8));
	game.color_index = (u8*)malloc(game.vertex_count * sizeof(u8));
	game.neighbors = (u32*)malloc(game.vertex_count * sizeof(u32));
	game.cache_keys = (u64*)calloc(game.cache_size, sizeof(u64));
	game.cache_colors = (Color*)calloc(game.cache_size, sizeof(Color));
	game.cache_samples = (u32*)calloc(game.cache_size, sizeof(u32));

	if (!game.spheres || !game.props || !game.alive || !game.color_index ||
			!game.neighbors || !game.cache_keys || !game.cache_colors ||
			!game.cache_samples) {
		fprintf(stderr, "Out of memory while allocating game arrays.\n");
		free_game(&game);
		return 1;
	}

	for (u32 i = 0; i < game.vertex_count; i++) {
		Face* face = (Face*)get_array_element(&game.mesh.faces, i);
		Vec3 center = vec3(0, 0, 0);
		if (face) {
			Vec3* v0 =
					(Vec3*)get_array_element(&game.mesh.vertices, face->vertex_indices[0]);
			Vec3* v1 =
					(Vec3*)get_array_element(&game.mesh.vertices, face->vertex_indices[1]);
			Vec3* v2 =
					(Vec3*)get_array_element(&game.mesh.vertices, face->vertex_indices[2]);
			Vec3 p0 = v0 ? *v0 : vec3(0, 0, 0);
			Vec3 p1 = v1 ? *v1 : vec3(0, 0, 0);
			Vec3 p2 = v2 ? *v2 : vec3(0, 0, 0);
			center = scale_vec3(add_vec3(add_vec3(p0, p1), p2), 1.0f / 3.0f);
		}
		game.spheres[i] = build_sphere(center, sphere_radius);

		GameSphereProps* p = (GameSphereProps*)malloc(sizeof(GameSphereProps));
		if (!p) {
			fprintf(stderr, "Out of memory for sphere properties.\n");
			free_game(&game);
			return 1;
		}
		p->id = i;
		p->tex_coord = vec2(0, 0);
		p->texture = NULL;
		p->material = &game.sphere_material;

		// If mesh already has colors, preserve rough palette class; otherwise random.
		if (game.mesh.colors.length > i) {
			Color* c = (Color*)get_array_element(&game.mesh.colors, i);
			if (c) {
				p->color = *c;
				game.color_index[i] = (u8)closest_palette_color(*c);
			} else {
				game.color_index[i] = (u8)(rand() % COLOR_PALETTE_LEN);
				p->color = COLOR_PALETTE[game.color_index[i]];
			}
		} else {
			game.color_index[i] = (u8)(rand() % COLOR_PALETTE_LEN);
			p->color = COLOR_PALETTE[game.color_index[i]];
		}

		game.props[i] = p;
		game.spheres[i].props = p;
		game.alive[i] = 1;
	}

	build_adjacency(&game);

	game.scene = new_kscene(64);
	rebuild_scene_from_alive(&game);

	game.camera = create_camera(vec3(0, 0, 2), vec3(0, 0, 0), 1.0f);
	set_orbit_camera(&game.camera, 2.0f, 0, 0);

	game.vertex_to_win = (u32)floorf(game.percentage_to_win * game.vertex_count);
	if (game.vertex_to_win == 0)
		game.vertex_to_win = 1;

	on_close_window(game.window, on_close, &game);
	on_mouse_down_window(game.window, on_mouse_down, &game);
	on_mouse_up_window(game.window, on_mouse_up, &game);
	on_mouse_move_window(game.window, on_mouse_move, &game);
	on_mouse_scroll_window(game.window, on_mouse_scroll, &game);
	on_key_down_window(game.window, on_key_down, &game);

	game.main_loop = loop(tick, &game);
	play_loop(game.main_loop);

	free_game(&game);
	return 0;
}
