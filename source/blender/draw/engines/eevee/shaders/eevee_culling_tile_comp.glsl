
/**
 * 2D Culling pass for lights.
 * We iterate over all items and check if they intersect with the tile frustum.
 * Dispatch one thread per word.
 */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_geom_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_light_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_culling_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_culling_iter_lib.glsl)

void main(void)
{
  uint word_idx = gl_GlobalInvocationID.x % culling.tile_word_len;
  uint tile_idx = gl_GlobalInvocationID.x / culling.tile_word_len;
  uvec2 tile_co = uvec2(tile_idx % culling.tile_x_len, tile_idx / culling.tile_x_len);

  if (tile_co.y >= culling.tile_y_len) {
    return;
  }

  /* TODO(fclem): We could stop the tile at the HiZ depth. */
  CullingTile tile = culling_tile_get(culling, tile_co);

  uint l_idx = max(word_idx * 32u, culling.items_no_cull_count);
  uint l_end = min(l_idx + 32u, culling.visible_count + culling.items_no_cull_count);
  uint word = 0u;

  for (; l_idx < l_end; l_idx++) {
    LightData light = lights_buf[l_idx];

    bool intersect_tile;
    switch (light.type) {
      case LIGHT_SPOT:
        /* TODO cone culling. */
      case LIGHT_RECT:
      case LIGHT_ELLIPSE:
      case LIGHT_POINT:
        Sphere sphere = Sphere(light._position, light.influence_radius_max);
        intersect_tile = culling_sphere_tile_isect(sphere, tile);
        break;
    }

    if (intersect_tile) {
      word |= 1u << (l_idx & 0x1Fu);
    }
  }

  culling_words[gl_GlobalInvocationID.x] = word;
}