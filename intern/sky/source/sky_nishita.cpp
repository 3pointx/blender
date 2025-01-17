/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2020 3 Point X. */

/** \file
 * \ingroup intern_sky_modal
 */

#include "sky_float3.h"
#include "sky_model.h"

/* Constants */
static const float rayleigh_scale = 8e3f;        // Rayleigh scale height (m)
static const float mie_scale = 1.2e3f;           // Mie scale height (m)
static const float mie_coeff = 2e-5f;            // Mie scattering coefficient (m^-1)
static const float mie_G = 0.76f;                // aerosols anisotropy
static const float sqr_G = mie_G * mie_G;        // squared aerosols anisotropy
static const float earth_radius = 6360e3f;       // radius of Earth (m)
static const float atmosphere_radius = 6420e3f;  // radius of atmosphere (m)
static const int steps = 32;                     // segments of primary ray
static const int num_wavelengths = 21;           // number of wavelengths
static const int min_wavelength = 380;           // lowest sampled wavelength (nm)
static const int max_wavelength = 780;           // highest sampled wavelength (nm)
// step between each sampled wavelength (nm)
static const float step_lambda = (max_wavelength - min_wavelength) / (num_wavelengths - 1);
/* Sun irradiance on top of the atmosphere (W*m^-2*nm^-1) */
static const float irradiance[] = {
    1.45756829855592995315f, 1.56596305559738380175f, 1.65148449067670455293f,
    1.71496242737209314555f, 1.75797983805020541226f, 1.78256407885924539336f,
    1.79095108475838560302f, 1.78541550133410664714f, 1.76815554864306845317f,
    1.74122069647250410362f, 1.70647127164943679389f, 1.66556087452739887134f,
    1.61993437242451854274f, 1.57083597368892080581f, 1.51932335059305478886f,
    1.46628494965214395407f, 1.41245852740172450623f, 1.35844961970384092709f,
    1.30474913844739281998f, 1.25174963272610817455f, 1.19975998755420620867f};
/* Rayleigh scattering coefficient (m^-1) */
static const float rayleigh_coeff[] = {
    0.00005424820087636473f, 0.00004418549866505454f, 0.00003635151910165377f,
    0.00003017929012024763f, 0.00002526320226989157f, 0.00002130859310621843f,
    0.00001809838025320633f, 0.00001547057129129042f, 0.00001330284977336850f,
    0.00001150184784075764f, 0.00000999557429990163f, 0.00000872799973630707f,
    0.00000765513700977967f, 0.00000674217203751443f, 0.00000596134125832052f,
    0.00000529034598065810f, 0.00000471115687557433f, 0.00000420910481110487f,
    0.00000377218381260133f, 0.00000339051255477280f, 0.00000305591531679811f};
/* Ozone absorption coefficient (m^-1) */
static const float ozone_coeff[] = {
    0.00000000325126849861f, 0.00000000585395365047f, 0.00000001977191155085f,
    0.00000007309568762914f, 0.00000020084561514287f, 0.00000040383958096161f,
    0.00000063551335912363f, 0.00000096707041180970f, 0.00000154797400424410f,
    0.00000209038647223331f, 0.00000246128056164565f, 0.00000273551299461512f,
    0.00000215125863128643f, 0.00000159051840791988f, 0.00000112356197979857f,
    0.00000073527551487574f, 0.00000046450130357806f, 0.00000033096079921048f,
    0.00000022512612292678f, 0.00000014879129266490f, 0.00000016828623364192f};
/* CIE XYZ color matching functions */
static const float cmf_xyz[][3] = {{0.00136800000f, 0.00003900000f, 0.00645000100f},
                                   {0.01431000000f, 0.00039600000f, 0.06785001000f},
                                   {0.13438000000f, 0.00400000000f, 0.64560000000f},
                                   {0.34828000000f, 0.02300000000f, 1.74706000000f},
                                   {0.29080000000f, 0.06000000000f, 1.66920000000f},
                                   {0.09564000000f, 0.13902000000f, 0.81295010000f},
                                   {0.00490000000f, 0.32300000000f, 0.27200000000f},
                                   {0.06327000000f, 0.71000000000f, 0.07824999000f},
                                   {0.29040000000f, 0.95400000000f, 0.02030000000f},
                                   {0.59450000000f, 0.99500000000f, 0.00390000000f},
                                   {0.91630000000f, 0.87000000000f, 0.00165000100f},
                                   {1.06220000000f, 0.63100000000f, 0.00080000000f},
                                   {0.85444990000f, 0.38100000000f, 0.00019000000f},
                                   {0.44790000000f, 0.17500000000f, 0.00002000000f},
                                   {0.16490000000f, 0.06100000000f, 0.00000000000f},
                                   {0.04677000000f, 0.01700000000f, 0.00000000000f},
                                   {0.01135916000f, 0.00410200000f, 0.00000000000f},
                                   {0.00289932700f, 0.00104700000f, 0.00000000000f},
                                   {0.00069007860f, 0.00024920000f, 0.00000000000f},
                                   {0.00016615050f, 0.00006000000f, 0.00000000000f},
                                   {0.00004150994f, 0.00001499000f, 0.00000000000f}};

/* Parameters for optical depth quadrature.
 * See the comment in ray_optical_depth for more detail.
 * Computed using sympy and following Python code:
 * # from sympy.integrals.quadrature import gauss_laguerre
 * # from sympy import exp
 * # x, w = gauss_laguerre(8, 50)
 * # xend = 25
 * # print([(xi / xend).evalf(10) for xi in x])
 * # print([(wi * exp(xi) / xend).evalf(10) for xi, wi in zip(x, w)])
 */
static const int quadrature_steps = 8;
static const float quadrature_nodes[] = {0.006811185292f,
                                         0.03614807107f,
                                         0.09004346519f,
                                         0.1706680068f,
                                         0.2818362161f,
                                         0.4303406404f,
                                         0.6296271457f,
                                         0.9145252695f};
static const float quadrature_weights[] = {0.01750893642f,
                                           0.04135477391f,
                                           0.06678839063f,
                                           0.09507698807f,
                                           0.1283416365f,
                                           0.1707430204f,
                                           0.2327233347f,
                                           0.3562490486f};

static float3 geographical_to_direction(float lat, float lon)
{
  return make_float3(cosf(lat) * cosf(lon), cosf(lat) * sinf(lon), sinf(lat));
}

static float3 spec_to_xyz(const float *spectrum)
{
  float3 xyz = make_float3(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < num_wavelengths; i++) {
    xyz.x += cmf_xyz[i][0] * spectrum[i];
    xyz.y += cmf_xyz[i][1] * spectrum[i];
    xyz.z += cmf_xyz[i][2] * spectrum[i];
  }
  return xyz * step_lambda;
}

/* Atmosphere volume models */
static float density_rayleigh(float height)
{
  return expf(-height / rayleigh_scale);
}

static float density_mie(float height)
{
  return expf(-height / mie_scale);
}

static float density_ozone(float height)
{
  float den = 0.0f;
  if (height >= 10000.0f && height < 25000.0f) {
    den = 1.0f / 15000.0f * height - 2.0f / 3.0f;
  }
  else if (height >= 25000 && height < 40000) {
    den = -(1.0f / 15000.0f * height - 8.0f / 3.0f);
  }
  return den;
}

static float phase_rayleigh(float mu)
{
  return 3.0f / (16.0f * M_PI_F) * (1.0f + sqr(mu));
}

static float phase_mie(float mu)
{
  return (3.0f * (1.0f - sqr_G) * (1.0f + sqr(mu))) /
         (8.0f * M_PI_F * (2.0f + sqr_G) * powf((1.0f + sqr_G - 2.0f * mie_G * mu), 1.5));
}

/* Intersection helpers */
static bool surface_intersection(float3 pos, float3 dir)
{
  if (dir.z >= 0) {
    return false;
  }
  float b = -2.0f * dot(dir, -pos);
  float c = len_squared(pos) - sqr(earth_radius);
  float t = b * b - 4.0f * c;
  if (t >= 0.0f) {
    return true;
  }
  return false;
}

static float3 atmosphere_intersection(float3 pos, float3 dir)
{
  float b = -2.0f * dot(dir, -pos);
  float c = len_squared(pos) - sqr(atmosphere_radius);
  float t = (-b + sqrtf(b * b - 4.0f * c)) / 2.0f;
  return make_float3(pos.x + dir.x * t, pos.y + dir.y * t, pos.z + dir.z * t);
}

static float3 ray_optical_depth(float3 ray_origin, float3 ray_dir)
{
  /* This function computes the optical depth along a ray.
   * Instead of using classic ray marching, the code is based on Gauss-Laguerre quadrature,
   * which is designed to compute the integral of f(x)*exp(-x) from 0 to infinity.
   * This works well here, since the optical depth along the ray tends to decrease exponentially.
   * By setting f(x) = g(x) exp(x), the exponentials cancel out and we get the integral of g(x).
   * The nodes and weights used here are the standard n=6 Gauss-Laguerre values, except that
   * the exp(x) scaling factor is already included in the weights.
   * The parametrization along the ray is scaled so that the last quadrature node is still within
   * the atmosphere. */
  float3 ray_end = atmosphere_intersection(ray_origin, ray_dir);
  float ray_length = distance(ray_origin, ray_end);

  float3 segment = ray_length * ray_dir;

  /* instead of tracking the transmission spectrum across all wavelengths directly,
   * we use the fact that the density always has the same spectrum for each type of
   * scattering, so we split the density into a constant spectrum and a factor and
   * only track the factors */
  float3 optical_depth = make_float3(0.0f, 0.0f, 0.0f);

  for (int i = 0; i < quadrature_steps; i++) {
    float3 P = ray_origin + quadrature_nodes[i] * segment;

    /* height above sea level */
    float height = len(P) - earth_radius;

    float3 density = make_float3(
        density_rayleigh(height), density_mie(height), density_ozone(height));
    optical_depth += density * quadrature_weights[i];
  }

  return optical_depth * ray_length;
}

static void single_scattering(float3 ray_dir,
                              float3 sun_dir,
                              float3 ray_origin,
                              float air_density,
                              float dust_density,
                              float ozone_density,
                              float *r_spectrum)
{
  /* this code computes single-inscattering along a ray through the atmosphere */
  float3 ray_end = atmosphere_intersection(ray_origin, ray_dir);
  float ray_length = distance(ray_origin, ray_end);

  /* to compute the inscattering, we step along the ray in segments and accumulate
   * the inscattering as well as the optical depth along each segment */
  float segment_length = ray_length / steps;
  float3 segment = segment_length * ray_dir;

  /* instead of tracking the transmission spectrum across all wavelengths directly,
   * we use the fact that the density always has the same spectrum for each type of
   * scattering, so we split the density into a constant spectrum and a factor and
   * only track the factors */
  float3 optical_depth = make_float3(0.0f, 0.0f, 0.0f);

  /* zero out light accumulation */
  for (int wl = 0; wl < num_wavelengths; wl++) {
    r_spectrum[wl] = 0.0f;
  }

  /* phase function for scattering and the density scale factor */
  float mu = dot(ray_dir, sun_dir);
  float3 phase_function = make_float3(phase_rayleigh(mu), phase_mie(mu), 0.0f);
  float3 density_scale = make_float3(air_density, dust_density, ozone_density);

  /* the density and in-scattering of each segment is evaluated at its middle */
  float3 P = ray_origin + 0.5f * segment;

  for (int i = 0; i < steps; i++) {
    /* height above sea level */
    float height = len(P) - earth_radius;

    /* evaluate and accumulate optical depth along the ray */
    float3 density = density_scale * make_float3(density_rayleigh(height),
                                                 density_mie(height),
                                                 density_ozone(height));
    optical_depth += segment_length * density;

    /* if the Earth isn't in the way, evaluate inscattering from the sun */
    if (!surface_intersection(P, sun_dir)) {
      float3 light_optical_depth = density_scale * ray_optical_depth(P, sun_dir);
      float3 total_optical_depth = optical_depth + light_optical_depth;

      /* attenuation of light */
      for (int wl = 0; wl < num_wavelengths; wl++) {
        float3 extinction_density = total_optical_depth * make_float3(rayleigh_coeff[wl],
                                                                      1.11f * mie_coeff,
                                                                      ozone_coeff[wl]);
        float attenuation = expf(-reduce_add(extinction_density));

        float3 scattering_density = density * make_float3(rayleigh_coeff[wl], mie_coeff, 0.0f);

        /* the total inscattered radiance from one segment is:
         * Tr(A<->B) * Tr(B<->C) * sigma_s * phase * L * segment_length
         *
         * These terms are:
         * Tr(A<->B): Transmission from start to scattering position (tracked in optical_depth)
         * Tr(B<->C): Transmission from scattering position to light (computed in
         * ray_optical_depth) sigma_s: Scattering density phase: Phase function of the scattering
         * type (Rayleigh or Mie) L: Radiance coming from the light source segment_length: The
         * length of the segment
         *
         * The code here is just that, with a bit of additional optimization to not store full
         * spectra for the optical depth
         */
        r_spectrum[wl] += attenuation * reduce_add(phase_function * scattering_density) *
                          irradiance[wl] * segment_length;
      }
    }

    /* advance along ray */
    P += segment;
  }
}

void SKY_nishita_skymodel_precompute_texture(float *pixels,
                                             int stride,
                                             int start_y,
                                             int end_y,
                                             int width,
                                             int height,
                                             float sun_elevation,
                                             float altitude,
                                             float air_density,
                                             float dust_density,
                                             float ozone_density)
{
  /* calculate texture pixels */
  float spectrum[num_wavelengths];
  int half_width = width / 2;
  float3 cam_pos = make_float3(0, 0, earth_radius + altitude);
  float3 sun_dir = geographical_to_direction(sun_elevation, 0.0f);

  float latitude_step = M_PI_2_F / height;
  float longitude_step = M_2PI_F / width;
  float half_lat_step = latitude_step / 2.0f;

  for (int y = start_y; y < end_y; y++) {
    /* sample more pixels toward the horizon */
    float latitude = (M_PI_2_F + half_lat_step) * sqr(float(y) / height);

    float *pixel_row = pixels + (y * width * stride);
    for (int x = 0; x < half_width; x++) {
      float longitude = longitude_step * x - M_PI_F;

      float3 dir = geographical_to_direction(latitude, longitude);
      single_scattering(dir, sun_dir, cam_pos, air_density, dust_density, ozone_density, spectrum);
      float3 xyz = spec_to_xyz(spectrum);

      /* store pixels */
      int pos_x = x * stride;
      pixel_row[pos_x] = xyz.x;
      pixel_row[pos_x + 1] = xyz.y;
      pixel_row[pos_x + 2] = xyz.z;
      /* mirror sky */
      int mirror_x = (width - x - 1) * stride;
      pixel_row[mirror_x] = xyz.x;
      pixel_row[mirror_x + 1] = xyz.y;
      pixel_row[mirror_x + 2] = xyz.z;
    }
  }
}

/*********** Sun ***********/
static void sun_radiation(float3 cam_dir,
                          float altitude,
                          float air_density,
                          float dust_density,
                          float solid_angle,
                          float *r_spectrum)
{
  float3 cam_pos = make_float3(0, 0, earth_radius + altitude);
  float3 optical_depth = ray_optical_depth(cam_pos, cam_dir);

  /* compute final spectrum */
  for (int i = 0; i < num_wavelengths; i++) {
    /* combine spectra and the optical depth into transmittance */
    float transmittance = rayleigh_coeff[i] * optical_depth.x * air_density +
                          1.11f * mie_coeff * optical_depth.y * dust_density;
    r_spectrum[i] = irradiance[i] * expf(-transmittance) / solid_angle;
  }
}

void SKY_nishita_skymodel_precompute_sun(float sun_elevation,
                                         float angular_diameter,
                                         float altitude,
                                         float air_density,
                                         float dust_density,
                                         float *r_pixel_bottom,
                                         float *r_pixel_top)
{
  /* definitions */
  float half_angular = angular_diameter / 2.0f;
  float solid_angle = M_2PI_F * (1.0f - cosf(half_angular));
  float spectrum[num_wavelengths];
  float bottom = sun_elevation - half_angular;
  float top = sun_elevation + half_angular;
  float elevation_bottom, elevation_top;
  float3 pix_bottom, pix_top, sun_dir;

  /* compute 2 pixels for sun disc */
  elevation_bottom = (bottom > 0.0f) ? bottom : 0.0f;
  elevation_top = (top > 0.0f) ? top : 0.0f;
  sun_dir = geographical_to_direction(elevation_bottom, 0.0f);
  sun_radiation(sun_dir, altitude, air_density, dust_density, solid_angle, spectrum);
  pix_bottom = spec_to_xyz(spectrum);
  sun_dir = geographical_to_direction(elevation_top, 0.0f);
  sun_radiation(sun_dir, altitude, air_density, dust_density, solid_angle, spectrum);
  pix_top = spec_to_xyz(spectrum);

  /* store pixels */
  r_pixel_bottom[0] = pix_bottom.x;
  r_pixel_bottom[1] = pix_bottom.y;
  r_pixel_bottom[2] = pix_bottom.z;
  r_pixel_top[0] = pix_top.x;
  r_pixel_top[1] = pix_top.y;
  r_pixel_top[2] = pix_top.z;
}
