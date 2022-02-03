
/**
 * Select the visible items inside the active view and put them inside the sorting buffer.
 */

#pragma BLENDER_REQUIRE(common_debug_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_geom_lib.glsl)
#pragma BLENDER_REQUIRE(common_intersection_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_light_lib.glsl)

void main()
{
  uint l_idx = gl_GlobalInvocationID.x;
  if (l_idx >= culling.items_count) {
    return;
  }

  LightData light = lights_buf[l_idx];

  /* Sun lights are packed at the start of the array. */
  if (light.type == LIGHT_SUN) {
    keys[l_idx] = l_idx;
    return;
  }

  Sphere sphere;
  switch (light.type) {
    case LIGHT_SPOT:
      /* TODO cone culling. */
    case LIGHT_RECT:
    case LIGHT_ELLIPSE:
    case LIGHT_POINT:
      sphere = Sphere(light._position, light.influence_radius_max);
      break;
  }

  if (intersect_view(sphere)) {
    uint index = culling.items_no_cull_count + atomicAdd(culling.visible_count, 1u);
    keys[index] = l_idx;
  }
}
