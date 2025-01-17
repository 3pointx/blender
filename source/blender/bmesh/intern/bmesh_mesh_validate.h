/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2012 3 Point X. All rights reserved. */

#pragma once

/** \file
 * \ingroup bmesh
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check of this #BMesh is valid,
 * this function can be slow since its intended to help with debugging.
 *
 * \return true when the mesh is valid.
 */
bool BM_mesh_validate(BMesh *bm);

#ifdef __cplusplus
}
#endif
