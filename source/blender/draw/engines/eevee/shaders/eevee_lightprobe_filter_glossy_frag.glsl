
/**
 * Outputs pre-convolved glossy BSDF irradiance from an input radiance cubemap.
 * Input radiance has its mipmaps updated so we can use filtered importance sampling.
 *
 * Follows the principle of:
 * https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling
 */

#pragma BLENDER_REQUIRE(eevee_sampling_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_bsdf_sampling_lib.glsl)

void main()
{
  vec3 N, T, B, V;
  vec3 R = normalize(interp.coord);

  /* Isotropic assumption. */
  N = V = R;

  make_orthonormal_basis(N, T, B);

  /* Integrating Envmap. */
  float weight = 0.0;
  vec3 radiance = vec3(0.0);
  for (float i = 0; i < filter_buf.sample_count; i++) {
    vec3 Xi = sample_cylinder(hammersley_2d(i, filter_buf.sample_count));

    float pdf;
    vec3 L = sample_ggx_reflect(Xi, filter_buf.roughness, V, N, T, B, pdf);

    if (pdf > 0.0) {
      /* Microfacet normal. */
      vec3 H = normalize(V + L);
      float NL = dot(N, L);
      float NH = max(1e-8, dot(N, H));

      /* Coarse Approximation of the mapping distortion.
       * Unit Sphere -> Cubemap Face. */
      const float dist = 4.0 * M_PI / 6.0;
      /* Equation 13. */
      float lod = filter_buf.lod_bias - 0.5 * log2(pdf * dist);

      vec3 l_col = textureLod(radiance_tx, L, lod).rgb;

      /* Clamped brightness. */
      float luma = max(1e-8, max_v3(l_col));
      l_col *= 1.0 - max(0.0, luma - filter_buf.luma_max) / luma;

      radiance += l_col * NL;
      weight += NL;
    }
  }

  out_irradiance = vec4(filter_buf.instensity_fac * radiance / weight, 1.0);
}
