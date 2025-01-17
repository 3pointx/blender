/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 3 Point X. All rights reserved. */

#include "intern/track_region.h"
#include "intern/image.h"
#include "intern/utildefines.h"
#include "libmv/image/image.h"
#include "libmv/logging/logging.h"
#include "libmv/tracking/track_region.h"

/* define this to generate PNG images with content of search areas
   tracking between which failed */
#undef DUMP_FAILURE

/* define this to generate PNG images with content of search areas
   on every iteration of tracking */
#undef DUMP_ALWAYS

using libmv::FloatImage;
using libmv::TrackRegion;
using libmv::TrackRegionOptions;
using libmv::TrackRegionResult;

namespace {

TrackRegionOptions::Direction convertDirection(
    libmv_TrackRegionDirection direction) {
  switch (direction) {
    case LIBMV_TRACK_REGION_FORWARD: return TrackRegionOptions::FORWARD;
    case LIBMV_TRACK_REGION_BACKWARD: return TrackRegionOptions::BACKWARD;
  }

  LOG(FATAL) << "Unhandled tracking direction " << direction
             << ", should never happen.";

  return TrackRegionOptions::FORWARD;
}

TrackRegionOptions::Mode convertMotionModelToMode(int motion_model) {
  switch (motion_model) {
#define LIBMV_CONVERT(the_model)                                               \
  case TrackRegionOptions::the_model: return TrackRegionOptions::the_model;

    LIBMV_CONVERT(TRANSLATION)
    LIBMV_CONVERT(TRANSLATION_ROTATION)
    LIBMV_CONVERT(TRANSLATION_SCALE)
    LIBMV_CONVERT(TRANSLATION_ROTATION_SCALE)
    LIBMV_CONVERT(AFFINE)
    LIBMV_CONVERT(HOMOGRAPHY)

#undef LIBMV_CONVERT
  }

  LOG(FATAL) << "Unhandled motion model " << motion_model
             << ", should never happen.";

  return TrackRegionOptions::TRANSLATION;
}

}  // namespace

void libmv_configureTrackRegionOptions(
    const libmv_TrackRegionOptions& options,
    TrackRegionOptions* track_region_options) {
  track_region_options->direction = convertDirection(options.direction);
  track_region_options->mode = convertMotionModelToMode(options.motion_model);
  track_region_options->minimum_correlation = options.minimum_correlation;
  track_region_options->max_iterations = options.num_iterations;
  track_region_options->sigma = options.sigma;
  track_region_options->num_extra_points = 1;
  track_region_options->image1_mask = NULL;
  track_region_options->use_brute_initialization = options.use_brute;
  /* TODO(keir): This will make some cases better, but may be a regression until
   * the motion model is in. Since this is on trunk, enable it for now.
   *
   * TODO(sergey): This gives much worse results on mango footage (see 04_2e)
   * so disabling for now for until proper prediction model is landed.
   *
   * The thing is, currently blender sends input coordinates as the guess to
   * region tracker and in case of fast motion such an early out ruins the
   * track.
   */
  track_region_options->attempt_refine_before_brute = false;
  track_region_options->use_normalized_intensities = options.use_normalization;
}

void libmv_regionTrackergetResult(const TrackRegionResult& track_region_result,
                                  libmv_TrackRegionResult* result) {
  result->termination = (int)track_region_result.termination;
  result->termination_reason = "";
  result->correlation = track_region_result.correlation;
}

int libmv_trackRegion(const libmv_TrackRegionOptions* options,
                      const float* image1,
                      int image1_width,
                      int image1_height,
                      const float* image2,
                      int image2_width,
                      int image2_height,
                      const double* x1,
                      const double* y1,
                      libmv_TrackRegionResult* /*result*/,
                      double* x2,
                      double* y2) {
  double xx1[5], yy1[5];
  double xx2[5], yy2[5];
  bool tracking_result = false;

  // Convert to doubles for the libmv api. The four corners and the center.
  for (int i = 0; i < 5; ++i) {
    xx1[i] = x1[i];
    yy1[i] = y1[i];
    xx2[i] = x2[i];
    yy2[i] = y2[i];
  }

  TrackRegionOptions track_region_options;
  FloatImage image1_mask;

  libmv_configureTrackRegionOptions(*options, &track_region_options);
  if (options->image1_mask) {
    libmv_floatBufferToFloatImage(
        options->image1_mask, image1_width, image1_height, 1, &image1_mask);

    track_region_options.image1_mask = &image1_mask;
  }

  // Convert from raw float buffers to libmv's FloatImage.
  FloatImage old_patch, new_patch;
  libmv_floatBufferToFloatImage(
      image1, image1_width, image1_height, 1, &old_patch);
  libmv_floatBufferToFloatImage(
      image2, image2_width, image2_height, 1, &new_patch);

  TrackRegionResult track_region_result;
  TrackRegion(old_patch,
              new_patch,
              xx1,
              yy1,
              track_region_options,
              xx2,
              yy2,
              &track_region_result);

  // Convert to floats for the blender api.
  for (int i = 0; i < 5; ++i) {
    x2[i] = xx2[i];
    y2[i] = yy2[i];
  }

  // TODO(keir): Update the termination string with failure details.
  if (track_region_result.termination == TrackRegionResult::CONVERGENCE ||
      track_region_result.termination == TrackRegionResult::NO_CONVERGENCE) {
    tracking_result = true;
  }

  // Debug dump of patches.
#if defined(DUMP_FAILURE) || defined(DUMP_ALWAYS)
  bool need_dump = !tracking_result;

#  ifdef DUMP_ALWAYS
  need_dump = true;
#  endif

  if (need_dump) {
    libmv_saveImage(old_patch, "old_patch", x1[4], y1[4]);
    libmv_saveImage(new_patch, "new_patch", x2[4], y2[4]);
    if (options->image1_mask) {
      libmv_saveImage(image1_mask, "mask", x2[4], y2[4]);
    }
  }
#endif

  return tracking_result;
}
