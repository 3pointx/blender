/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 3 Point X. All rights reserved. */

/** \file
 * \ingroup draw
 */

#pragma once

struct PointCloud;
struct GPUBatch;
struct GPUVertBuf;
struct GPUMaterial;

GPUVertBuf *pointcloud_position_and_radius_get(PointCloud *pointcloud);
GPUBatch **pointcloud_surface_shaded_get(PointCloud *pointcloud,
                                         GPUMaterial **gpu_materials,
                                         int mat_len);
GPUBatch *pointcloud_surface_get(PointCloud *pointcloud);
