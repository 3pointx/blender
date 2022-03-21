void dfdx_v3(vec3 v, out vec3 dy)
{
#ifdef GPU_FRAGMENT_SHADER
  dy = v + DFDX_SIGN * dFdx(v);
#else
  dy = v;
#endif
}

void dfdy_v3(vec3 v, out vec3 dy)
{
#ifdef GPU_FRAGMENT_SHADER
  dy = v + DFDY_SIGN * dFdy(v);
#else
  dy = v;
#endif
}

void node_bump(float strength,
               float dist,
               float height,
               float height_dx,
               float height_dy,
               vec3 N,
               float invert,
               out vec3 result)
{
  N = normalize(N);
  dist *= FrontFacing ? invert : -invert;

#ifdef GPU_FRAGMENT_SHADER
  vec3 dPdx = dFdx(g_data.P);
  vec3 dPdy = dFdy(g_data.P);

  /* Get surface tangents from normal. */
  vec3 Rx = cross(dPdy, N);
  vec3 Ry = cross(N, dPdx);

  /* Compute surface gradient and determinant. */
  float det = dot(dPdx, Rx);

  float dHdx = height_dx - height;
  float dHdy = height_dy - height;
  vec3 surfgrad = dHdx * Rx + dHdy * Ry;

  strength = max(strength, 0.0);

  result = normalize(abs(det) * N - dist * sign(det) * surfgrad);
  result = normalize(mix(N, result, strength));
#else
  result = N;
#endif
}
