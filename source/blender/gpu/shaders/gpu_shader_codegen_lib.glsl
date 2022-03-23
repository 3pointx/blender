
/* Assumes GPU_VEC4 is color data. So converting to luminance like cycles. */
#define float_from_vec4(v) dot(v.rgb, vec3(0.2126, 0.7152, 0.0722))
#define float_from_vec3(v) avg(v.rgb)
#define float_from_vec2(v) v.r

#define vec2_from_vec4(v) vec2(avg(v.rgb), v.a)
#define vec2_from_vec3(v) vec2(avg(v.rgb), 1.0)
#define vec2_from_float(v) vec2(v)

#define vec3_from_vec4(v) v.rgb
#define vec3_from_vec2(v) v.rrr
#define vec3_from_float(v) vec3(v)

#define vec4_from_vec3(v) vec4(v, 1.0)
#define vec4_from_vec2(v) v.rrrg
#define vec4_from_float(v) vec4(vec3(v), 1.0)

#define RAY_TYPE_CAMERA 0
#define RAY_TYPE_SHADOW 1
#define RAY_TYPE_DIFFUSE 2
#define RAY_TYPE_GLOSSY 3

#ifdef GPU_FRAGMENT_SHADER
#  define FrontFacing gl_FrontFacing
#else
#  define FrontFacing true
#endif

struct ClosureDiffuse {
  vec3 color;
  vec3 N;
  vec3 sss_radius;
  uint sss_id;
};

struct ClosureReflection {
  vec3 color;
  vec3 N;
  float roughness;
};

struct ClosureRefraction {
  vec3 color;
  vec3 N;
  float roughness;
  float ior;
};

struct ClosureVolume {
  vec3 emission;
  vec3 scattering;
  vec3 transmittance;
  float anisotropy;
};

struct ClosureEmission {
  vec3 emission;
};

struct ClosureTransparency {
  vec3 transmittance;
  float holdout;
};
