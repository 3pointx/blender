
/**
 * Sort the lights by their Z distance to the camera.
 * Outputs ordered light buffer and associated zbins.
 * We split the work in CULLING_BATCH_SIZE and iterate to cover all zbins.
 * One thread process one Light entity.
 */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_culling_iter_lib.glsl)

/* Fits the limit of 32KB. */
shared int zbin_max[CULLING_ZBIN_COUNT];
shared int zbin_min[CULLING_ZBIN_COUNT];

void main()
{
  uint src_index = gl_GlobalInvocationID.x;
  bool valid_thread = true;

  uint items_count_total = lights_cull_buf.items_no_cull_count + lights_cull_buf.visible_count;

  if (src_index >= items_count_total) {
    /* Do not return because we use barriers later on (which need uniform control flow).
     * Just process the same last item but avoid insertion. */
    src_index = items_count_total - 1;
    valid_thread = false;
  }

  uint key = keys_buf[src_index];
  LightData light = lights_buf[key];

  if (light.type == LIGHT_SUN) {
    valid_thread = false;
  }

  if (!lights_cull_buf.enable_specular) {
    light.specular_power = 0.0;
  }

  int index = 0;
  int contenders = 0;

  vec3 lP = light._position;
  float radius = light.influence_radius_max;
  float z_dist = dot(cameraForward, lP) - dot(cameraForward, cameraPos);

  int z_min = culling_z_to_zbin(
      lights_cull_buf.zbin_scale, lights_cull_buf.zbin_bias, z_dist + radius);
  int z_max = culling_z_to_zbin(
      lights_cull_buf.zbin_scale, lights_cull_buf.zbin_bias, z_dist - radius);
  z_min = clamp(z_min, 0, CULLING_ZBIN_COUNT - 1);
  z_max = clamp(z_max, 0, CULLING_ZBIN_COUNT - 1);

  /* Compilers do not release shared memory from early declaration.
   * So we are forced to reuse the same variables in another form. */
#define z_dists zbin_max
#define contender_table zbin_min

  /**
   * Find how many values are before the local value.
   * This finds the first possible destination index.
   */
  z_dists[gl_LocalInvocationID.x] = floatBitsToInt(z_dist);
  barrier();

  int batch_start = int(gl_WorkGroupID.x) * CULLING_BATCH_SIZE;
  int i_start = max(batch_start, int(lights_cull_buf.items_no_cull_count));
  int i_max = min(CULLING_BATCH_SIZE, int(items_count_total) - batch_start);
  for (int i = i_start; i < i_max; i++) {
    float ref = intBitsToFloat(z_dists[i]);
    if (ref > z_dist) {
      index++;
    }
    else if (ref == z_dist) {
      contenders++;
    }
  }

  if (valid_thread) {
    atomicExchange(contender_table[index], contenders);
  }
  barrier();

  if (valid_thread) {
    /**
     * For each clashing index (where two lights have exactly the same z distances)
     * we use an atomic counter to know how much to offset from the disputed index.
     */
    index += atomicAdd(contender_table[index], -1) - 1;
    index += i_start;
    out_lights_buf[index] = light;
  }
  else if (light.type == LIGHT_SUN) {
    /* Directional lights are just copied to the same index. */
    out_lights_buf[key] = light;
  }
  barrier();

  const uint iter = uint(CULLING_ZBIN_COUNT / CULLING_BATCH_SIZE);
  const uint zbin_local = gl_LocalInvocationID.x * iter;
  const uint zbin_global = gl_WorkGroupID.x * CULLING_ZBIN_COUNT + zbin_local;

  for (uint i = 0u, l = zbin_local; i < iter; i++, l++) {
    zbin_max[l] = 0x0000;
    zbin_min[l] = 0xFFFF;
  }
  barrier();

  /* Register to Z bins. */
  if (valid_thread) {
    for (int z = z_min; z <= z_max; z++) {
      atomicMin(zbin_min[z], index);
      atomicMax(zbin_max[z], index);
    }
  }
  barrier();

  /* Write result to zbins buffer. */
  for (uint i = 0u, g = zbin_global, l = zbin_local; i < iter; i++, g++, l++) {
    /* Pack min & max into 1 uint. */
    lights_zbin_buf[g] = (uint(zbin_max[l]) << 16u) | uint(zbin_min[l]);
  }
}
