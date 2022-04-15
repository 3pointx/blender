/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_threads.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.h"
#include "BKE_bvhutils.h"
#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_remesh_voxel.h" /* own include */
#include "BKE_mesh_runtime.h"
#include "BKE_paint.h"

#include "bmesh_tools.h"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/MeshToVolume.h>
#  include <openvdb/tools/VolumeToMesh.h>
#endif

#ifdef WITH_QUADRIFLOW
#  include "quadriflow_capi.hpp"
#endif

#ifdef WITH_INSTANT_MESHES
#  include "instant_meshes_c_api.h"
#endif

using blender::Array;
using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Span;

#ifdef WITH_QUADRIFLOW
static Mesh *remesh_quadriflow(const Mesh *input_mesh,
                               int target_faces,
                               int seed,
                               bool preserve_sharp,
                               bool preserve_boundary,
                               bool adaptive_scale,
                               void (*update_cb)(void *, float progress, int *cancel),
                               void *update_cb_data)
{
  /* Ensure that the triangulated mesh data is up to data */
  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(input_mesh);
  MeshElemMap *epmap = nullptr;
  int *epmem = nullptr;

  BKE_mesh_edge_poly_map_create(&epmap,
                                &epmem,
                                input_mesh->medge,
                                input_mesh->totedge,
                                input_mesh->mpoly,
                                input_mesh->totpoly,
                                input_mesh->mloop,
                                input_mesh->totloop);

  /* Gather the required data for export to the internal quadriflow mesh format. */
  MVertTri *verttri = (MVertTri *)MEM_callocN(
      sizeof(*verttri) * BKE_mesh_runtime_looptri_len(input_mesh), "remesh_looptri");
  BKE_mesh_runtime_verttri_from_looptri(
      verttri, input_mesh->mloop, looptri, BKE_mesh_runtime_looptri_len(input_mesh));

  const int totfaces = BKE_mesh_runtime_looptri_len(input_mesh);
  const int totverts = input_mesh->totvert;
  Array<float3> verts(totverts);
  Array<QuadriflowFace> faces(totfaces);

  for (const int i : IndexRange(totverts)) {
    verts[i] = input_mesh->mvert[i].co;
  }

  int *fsets = (int *)CustomData_get_layer(&input_mesh->pdata, CD_SCULPT_FACE_SETS);

  for (const int i : IndexRange(totfaces)) {
    MVertTri &vt = verttri[i];
    faces[i].eflag[0] = faces[i].eflag[1] = faces[i].eflag[2] = 0;

    faces[i].v[0] = vt.tri[0];
    faces[i].v[1] = vt.tri[1];
    faces[i].v[2] = vt.tri[2];

    for (const int j : IndexRange(3)) {
      MLoop *l = input_mesh->mloop + looptri[i].tri[j];
      MEdge *e = input_mesh->medge + l->e;

      if (e->flag & ME_SHARP) {
        faces[i].eflag[j] |= QFLOW_CONSTRAINED;
        continue;
      }

      MeshElemMap *melem = epmap + looptri[i].poly;

      if (melem->count == 1) {
        faces[i].eflag[j] |= QFLOW_CONSTRAINED;
        continue;
      }

      int fset = 0;
      int mat_nr = 0;

      for (int k : IndexRange(melem->count)) {
        MPoly *p = input_mesh->mpoly + melem->indices[k];

        if (k > 0 && p->mat_nr != mat_nr) {
          faces[i].eflag[j] |= QFLOW_CONSTRAINED;
          continue;
        }

        mat_nr = (int)p->mat_nr;

        if (fsets) {
          int fset2 = fsets[melem->indices[k]];

          if (k > 0 && abs(fset) != abs(fset2)) {
            faces[i].eflag[j] |= QFLOW_CONSTRAINED;
            break;
          }

          fset = fset2;
        }
      }
    }
  }

  /* Fill out the required input data */
  QuadriflowRemeshData qrd;

  qrd.totfaces = totfaces;
  qrd.totverts = totverts;
  qrd.verts = (float *)verts.data();
  qrd.faces = faces.data();
  qrd.target_faces = target_faces;

  qrd.preserve_sharp = preserve_sharp;
  qrd.preserve_boundary = preserve_boundary;
  qrd.adaptive_scale = adaptive_scale;
  qrd.minimum_cost_flow = false;
  qrd.aggresive_sat = false;
  qrd.rng_seed = seed;

  qrd.out_faces = nullptr;

  /* Run the remesher */
  QFLOW_quadriflow_remesh(&qrd, update_cb, update_cb_data);

  MEM_freeN(verttri);

  if (qrd.out_faces == nullptr) {
    /* The remeshing was canceled */
    return nullptr;
  }

  if (qrd.out_totfaces == 0) {
    /* Meshing failed */
    MEM_freeN(qrd.out_faces);
    MEM_freeN(qrd.out_verts);
    return nullptr;
  }

  /* Construct the new output mesh */
  Mesh *mesh = BKE_mesh_new_nomain(qrd.out_totverts, 0, 0, qrd.out_totfaces * 4, qrd.out_totfaces);

  for (const int i : IndexRange(qrd.out_totverts)) {
    copy_v3_v3(mesh->mvert[i].co, &qrd.out_verts[i * 3]);
  }

  for (const int i : IndexRange(qrd.out_totfaces)) {
    MPoly &poly = mesh->mpoly[i];
    const int loopstart = i * 4;
    poly.loopstart = loopstart;
    poly.totloop = 4;
    mesh->mloop[loopstart].v = qrd.out_faces[loopstart];
    mesh->mloop[loopstart + 1].v = qrd.out_faces[loopstart + 1];
    mesh->mloop[loopstart + 2].v = qrd.out_faces[loopstart + 2];
    mesh->mloop[loopstart + 3].v = qrd.out_faces[loopstart + 3];
  }

  BKE_mesh_calc_edges(mesh, false, false);
  BKE_mesh_calc_normals(mesh);

  MEM_freeN(qrd.out_faces);
  MEM_freeN(qrd.out_verts);

  if (epmap) {
    MEM_freeN((void *)epmap);
  }

  if (epmem) {
    MEM_freeN((void *)epmem);
  }

  return mesh;
}
#endif

struct HashEdge {
  std::size_t operator()(std::tuple<int, int> const &edge) const noexcept
  {
    int v1 = std::get<0>(edge);
    int v2 = std::get<1>(edge);

    if (v1 > v2) {
      std::swap(v1, v2);
    }

    return std::size_t(v1);
  }
};

// TODO: move from sculpt_smooth.c to blenlib or somewhere
extern "C" int closest_vec_to_perp(
    float dir[3], float r_dir2[3], float no[3], float *buckets, float w);

Mesh *BKE_mesh_remesh_instant_meshes(const Mesh *input_mesh,
                                     int target_faces,
                                     int iterations,
                                     void (*update_cb)(void *, float progress, int *cancel),
                                     void *update_cb_data)
{
#ifdef WITH_INSTANT_MESHES
  /* Ensure that the triangulated mesh data is up to data */
  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(input_mesh);
  MeshElemMap *epmap = nullptr;
  int *epmem = nullptr;

  instant_meshes_set_number_of_threads(BLI_system_thread_count());

  BKE_mesh_edge_poly_map_create(&epmap,
                                &epmem,
                                input_mesh->medge,
                                input_mesh->totedge,
                                input_mesh->mpoly,
                                input_mesh->totpoly,
                                input_mesh->mloop,
                                input_mesh->totloop);

  /* Gather the required data for export to the internal quadriflow mesh format. */
  MVertTri *verttri = (MVertTri *)MEM_callocN(
      sizeof(*verttri) * BKE_mesh_runtime_looptri_len(input_mesh), "remesh_looptri");
  BKE_mesh_runtime_verttri_from_looptri(
      verttri, input_mesh->mloop, looptri, BKE_mesh_runtime_looptri_len(input_mesh));

  MPropCol *dirs = NULL;

  int idx = CustomData_get_named_layer_index(&input_mesh->vdata, CD_PROP_COLOR, "_rake_temp");
  if (idx >= 0) {
    dirs = (MPropCol *)input_mesh->vdata.layers[idx].data;
  }

  const int totfaces = BKE_mesh_runtime_looptri_len(input_mesh);
  const int totverts = input_mesh->totvert;

  std::unordered_map<std::tuple<int, int>, int, HashEdge> eflags;
  auto make_edge_pair = [](int v1, int v2) {
    return std::tuple<int, int>(std::min(v1, v2), std::max(v1, v2));
  };

  Array<RemeshVertex> verts(totverts);
  Array<RemeshTri> faces(totfaces);
  std::vector<RemeshEdge> edges;
  const float(*normals)[3] = BKE_mesh_vertex_normals_ensure(input_mesh);

  for (int i : IndexRange(totverts)) {
    copy_v3_v3(verts[i].co, input_mesh->mvert[i].co);
    copy_v3_v3(verts[i].no, normals[i]);
  }

  int *fsets = (int *)CustomData_get_layer(&input_mesh->pdata, CD_SCULPT_FACE_SETS);
  
  for (const int i : IndexRange(input_mesh->totedge)) {
    MEdge *me = input_mesh->medge + i;
    MeshElemMap *mep = epmap + i;

    bool ok = mep->count == 1;
    ok = ok || (me->flag & (ME_SEAM | ME_SHARP));

    if (fsets && !ok) {
      int last_fset;

      // try face sets
      for (int j = 0; j < mep->count; j++) {
        int fset = abs(fsets[mep->indices[j]]);

        if (j > 0 && last_fset != fset) {
          ok = true;
          break;
        }

        last_fset = fset;
      }
    }

    if (ok || dirs) {
      RemeshEdge e;

      e.flag = ok ? REMESH_EDGE_BOUNDARY : 0;
      e.v1 = me->v1;
      e.v2 = me->v2;

      if (dirs && !ok) {
        e.flag |= REMESH_EDGE_USE_DIR;
        float d1[3], d2[3], vec[3], no1[3], no2[3];

        // get rake directions from verts
        copy_v3_v3(d1, dirs[me->v1].color);
        copy_v3_v3(d2, dirs[me->v2].color);

        // edge vec
        sub_v3_v3v3(vec, input_mesh->mvert[me->v2].co, input_mesh->mvert[me->v1].co);
        normalize_v3(vec);

        // build edge normal
        copy_v3_v3(no1, normals[me->v1]);
        copy_v3_v3(no2, normals[me->v2]);

        add_v3_v3(no1, no2);
        normalize_v3(no1);

        float buckets[8];

        // find closest of four 90 degree rotations to vec for d1, d2
        closest_vec_to_perp(vec, d1, no1, buckets, 1.0f);
        closest_vec_to_perp(vec, d2, no1, buckets, 1.0f);

        // build final direction
        add_v3_v3(d1, d2);
        normalize_v3(d1);

        copy_v3_v3(e.dir, d1);
      }

      eflags[make_edge_pair(me->v1, me->v2)] = i;
      edges.push_back(e);
    }
  }

  for (int i : IndexRange(totfaces)) {
    MVertTri *mtri = verttri + i;

    faces[i].v1 = mtri->tri[0];
    faces[i].v2 = mtri->tri[1];
    faces[i].v3 = mtri->tri[2];

    for (int j = 0; j < 3; j++) {
      int v1 = mtri->tri[j];
      int v2 = mtri->tri[(j + 1) % 3];

      auto item = eflags.find(make_edge_pair(v1, v2));
      if (item != eflags.end()) {
        int flag = item->second;
        faces[i].eflags[j] = flag;
      }
    }
  }

  RemeshMesh remesh;

  remesh.goal_faces = target_faces;
  remesh.tris = faces.data();
  remesh.iterations = iterations;
  remesh.tottri = (int)faces.size();

  remesh.verts = verts.data();
  remesh.edges = edges.data();
  remesh.totedge = (int)edges.size();
  remesh.totvert = (int)verts.size();

  instant_meshes_run(&remesh);

  int totloop = 0;
  for (int i : IndexRange(remesh.totoutface)) {
    totloop += remesh.outfaces[i].totvert;
  }

  Mesh *mesh = BKE_mesh_new_nomain(remesh.totoutvert, 0, 0, totloop, remesh.totoutface);

#  ifdef INSTANT_MESHES_VIS_COLOR
  MPropCol *cols = (MPropCol *)CustomData_add_layer(
      &mesh->vdata, CD_PROP_COLOR, CD_DEFAULT, NULL, remesh.totoutvert);
#  endif

  float(*newNormals)[3] = BKE_mesh_vertex_normals_for_write(mesh);

  for (int i : IndexRange(remesh.totoutvert)) {
    MVert *mv = mesh->mvert + i;
    RemeshVertex *v = remesh.outverts + i;

    copy_v3_v3(mv->co, v->co);
    copy_v3_v3(newNormals[i], v->no);

#  ifdef INSTANT_MESHES_VIS_COLOR
    copy_v3_v3(cols[i].color, v->viscolor);
    cols[i].color[3] = 1.0f;
#  endif
  }

  int li = 0;
  MLoop *ml = mesh->mloop;

  for (int i : IndexRange(remesh.totoutface)) {
    RemeshOutFace *f = remesh.outfaces + i;
    MPoly *mp = mesh->mpoly + i;

    mp->loopstart = li;
    mp->totloop = f->totvert;

    for (int j = 0; j < f->totvert; j++, ml++, li++) {
      ml->v = f->verts[j];
    }
  }

  instant_meshes_finish(&remesh);

  BKE_mesh_calc_edges(mesh, false, false);
  BKE_mesh_calc_normals(mesh);

  // C++ doesn't seem to like the MEM_SAFE_FREE macro
  if (epmap) {
    MEM_freeN((void *)epmap);
  }

  if (epmem) {
    MEM_freeN((void *)epmem);
  }

  return mesh;
#  if 0
 
  /* Construct the new output mesh */
  Mesh *mesh = BKE_mesh_new_nomain(qrd.out_totverts, 0, 0, qrd.out_totfaces * 4, qrd.out_totfaces);

  for (const int i : IndexRange(qrd.out_totverts)) {
    copy_v3_v3(mesh->mvert[i].co, &qrd.out_verts[i * 3]);
  }

  for (const int i : IndexRange(qrd.out_totfaces)) {
    MPoly &poly = mesh->mpoly[i];
    const int loopstart = i * 4;
    poly.loopstart = loopstart;
    poly.totloop = 4;
    mesh->mloop[loopstart].v = qrd.out_faces[loopstart];
    mesh->mloop[loopstart + 1].v = qrd.out_faces[loopstart + 1];
    mesh->mloop[loopstart + 2].v = qrd.out_faces[loopstart + 2];
    mesh->mloop[loopstart + 3].v = qrd.out_faces[loopstart + 3];
  }

  BKE_mesh_calc_edges(mesh, false, false);
  BKE_mesh_calc_normals(mesh);

  MEM_freeN(qrd.out_faces);
  MEM_freeN(qrd.out_verts);

  if (epmap) {
    MEM_freeN((void *)epmap);
  }

  if (epmem) {
    MEM_freeN((void *)epmem);
  }
  return mesh;
#  endif
#endif
  return nullptr;
}

Mesh *BKE_mesh_remesh_quadriflow(const Mesh *mesh,
                                 int target_faces,
                                 int seed,
                                 bool preserve_sharp,
                                 bool preserve_boundary,
                                 bool adaptive_scale,
                                 void (*update_cb)(void *, float progress, int *cancel),
                                 void *update_cb_data)
{
#ifdef WITH_QUADRIFLOW
  if (target_faces <= 0) {
    target_faces = -1;
  }
  return remesh_quadriflow(mesh,
                           target_faces,
                           seed,
                           preserve_sharp,
                           preserve_boundary,
                           adaptive_scale,
                           update_cb,
                           update_cb_data);
#else
  UNUSED_VARS(mesh,
              target_faces,
              seed,
              preserve_sharp,
              preserve_boundary,
              adaptive_scale,
              update_cb,
              update_cb_data);
  return nullptr;
#endif
}

#ifdef WITH_OPENVDB
static openvdb::FloatGrid::Ptr remesh_voxel_level_set_create(const Mesh *mesh,
                                                             const float voxel_size)
{
  Span<MLoop> mloop{mesh->mloop, mesh->totloop};
  Span<MLoopTri> looptris{BKE_mesh_runtime_looptri_ensure(mesh),
                          BKE_mesh_runtime_looptri_len(mesh)};

  std::vector<openvdb::Vec3s> points(mesh->totvert);
  std::vector<openvdb::Vec3I> triangles(looptris.size());

  for (const int i : IndexRange(mesh->totvert)) {
    const float3 co = mesh->mvert[i].co;
    points[i] = openvdb::Vec3s(co.x, co.y, co.z);
  }

  for (const int i : IndexRange(looptris.size())) {
    const MLoopTri &loop_tri = looptris[i];
    triangles[i] = openvdb::Vec3I(
        mloop[loop_tri.tri[0]].v, mloop[loop_tri.tri[1]].v, mloop[loop_tri.tri[2]].v);
  }

  openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(
      voxel_size);
  openvdb::FloatGrid::Ptr grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
      *transform, points, triangles, 1.0f);

  return grid;
}

static Mesh *remesh_voxel_volume_to_mesh(const openvdb::FloatGrid::Ptr level_set_grid,
                                         const float isovalue,
                                         const float adaptivity,
                                         const bool relax_disoriented_triangles)
{
  std::vector<openvdb::Vec3s> vertices;
  std::vector<openvdb::Vec4I> quads;
  std::vector<openvdb::Vec3I> tris;
  openvdb::tools::volumeToMesh<openvdb::FloatGrid>(
      *level_set_grid, vertices, tris, quads, isovalue, adaptivity, relax_disoriented_triangles);

  Mesh *mesh = BKE_mesh_new_nomain(
      vertices.size(), 0, 0, quads.size() * 4 + tris.size() * 3, quads.size() + tris.size());
  MutableSpan<MVert> mverts{mesh->mvert, mesh->totvert};
  MutableSpan<MLoop> mloops{mesh->mloop, mesh->totloop};
  MutableSpan<MPoly> mpolys{mesh->mpoly, mesh->totpoly};

  for (const int i : mverts.index_range()) {
    copy_v3_v3(mverts[i].co, float3(vertices[i].x(), vertices[i].y(), vertices[i].z()));
  }

  for (const int i : IndexRange(quads.size())) {
    MPoly &poly = mpolys[i];
    const int loopstart = i * 4;
    poly.loopstart = loopstart;
    poly.totloop = 4;
    mloops[loopstart].v = quads[i][0];
    mloops[loopstart + 1].v = quads[i][3];
    mloops[loopstart + 2].v = quads[i][2];
    mloops[loopstart + 3].v = quads[i][1];
  }

  const int triangle_loop_start = quads.size() * 4;
  for (const int i : IndexRange(tris.size())) {
    MPoly &poly = mpolys[quads.size() + i];
    const int loopstart = triangle_loop_start + i * 3;
    poly.loopstart = loopstart;
    poly.totloop = 3;
    mloops[loopstart].v = tris[i][2];
    mloops[loopstart + 1].v = tris[i][1];
    mloops[loopstart + 2].v = tris[i][0];
  }

  BKE_mesh_calc_edges(mesh, false, false);
  BKE_mesh_normals_tag_dirty(mesh);

  return mesh;
}
#endif

Mesh *BKE_mesh_remesh_voxel(const Mesh *mesh,
                            const float voxel_size,
                            const float adaptivity,
                            const float isovalue)
{
#ifdef WITH_OPENVDB
  openvdb::FloatGrid::Ptr level_set = remesh_voxel_level_set_create(mesh, voxel_size);
  return remesh_voxel_volume_to_mesh(level_set, isovalue, adaptivity, false);
#else
  UNUSED_VARS(mesh, voxel_size, adaptivity, isovalue);
  return nullptr;
#endif
}

void BKE_mesh_remesh_reproject_paint_mask(Mesh *target, Mesh *source)
{
  BVHTreeFromMesh bvhtree = {nullptr};
  BKE_bvhtree_from_mesh_get(&bvhtree, source, BVHTREE_FROM_VERTS, 2);
  MVert *target_verts = (MVert *)CustomData_get_layer(&target->vdata, CD_MVERT);

  float *target_mask;
  if (CustomData_has_layer(&target->vdata, CD_PAINT_MASK)) {
    target_mask = (float *)CustomData_get_layer(&target->vdata, CD_PAINT_MASK);
  }
  else {
    target_mask = (float *)CustomData_add_layer(
        &target->vdata, CD_PAINT_MASK, CD_CALLOC, nullptr, target->totvert);
  }

  float *source_mask;
  if (CustomData_has_layer(&source->vdata, CD_PAINT_MASK)) {
    source_mask = (float *)CustomData_get_layer(&source->vdata, CD_PAINT_MASK);
  }
  else {
    source_mask = (float *)CustomData_add_layer(
        &source->vdata, CD_PAINT_MASK, CD_CALLOC, nullptr, source->totvert);
  }

  for (int i = 0; i < target->totvert; i++) {
    float from_co[3];
    BVHTreeNearest nearest;
    nearest.index = -1;
    nearest.dist_sq = FLT_MAX;
    copy_v3_v3(from_co, target_verts[i].co);
    BLI_bvhtree_find_nearest(bvhtree.tree, from_co, &nearest, bvhtree.nearest_callback, &bvhtree);
    if (nearest.index != -1) {
      target_mask[i] = source_mask[nearest.index];
    }
  }
  free_bvhtree_from_mesh(&bvhtree);
}

void BKE_mesh_remesh_sculpt_array_update(Object *ob, Mesh *target, Mesh *source)
{

  SculptSession *ss = ob->sculpt;

  if (!ss) {
    return;
  }

  SculptArray *array = ss->array;
  if (!array) {
    return;
  }

  BVHTreeFromMesh bvhtree = {nullptr};
  bvhtree.nearest_callback = nullptr;
  BKE_bvhtree_from_mesh_get(&bvhtree, source, BVHTREE_FROM_VERTS, 2);
  MVert *target_verts = (MVert *)CustomData_get_layer(&target->vdata, CD_MVERT);

  const int target_totvert = target->totvert;

  int *target_copy_index = (int *)MEM_malloc_arrayN(
      sizeof(int), target_totvert, "target_copy_index");
  int *target_symmertry = (int *)MEM_malloc_arrayN(
      sizeof(int), target_totvert, "target_copy_index");
  float(*target_orco)[3] = (float(*)[3])MEM_malloc_arrayN(
      target->totvert, sizeof(float) * 3, "array orco");

  for (int i = 0; i < target_totvert; i++) {
    target_copy_index[i] = -1;
    target_symmertry[i] = 0;
    copy_v3_v3(target_orco[i], target->mvert[i].co);
  }

  for (int i = 0; i < target->totvert; i++) {
    float from_co[3];
    BVHTreeNearest nearest;
    nearest.index = -1;
    nearest.dist_sq = FLT_MAX;
    copy_v3_v3(from_co, target_verts[i].co);
    BLI_bvhtree_find_nearest(bvhtree.tree, from_co, &nearest, bvhtree.nearest_callback, &bvhtree);
    if (nearest.index != -1) {
      target_copy_index[i] = array->copy_index[nearest.index];
      target_symmertry[i] = array->symmetry_pass[nearest.index];
    }
  }
  free_bvhtree_from_mesh(&bvhtree);

  MEM_freeN(array->copy_index);
  MEM_freeN(array->symmetry_pass);
  MEM_freeN(array->orco);

  array->copy_index = target_copy_index;
  array->symmetry_pass = target_symmertry;
  array->orco = target_orco;

  for (int i = 0; i < target->totvert; i++) {
    int array_index = target_copy_index[i];
    int array_symm_pass = target_symmertry[i];
    if (array_index == -1) {
      continue;
    }
    SculptArrayCopy *copy = &array->copies[array_symm_pass][array_index];
    float co[3];
    float source_origin_symm[3];
    copy_v3_v3(co, target->mvert[i].co);
    /* TODO: MAke symmetry work here. */
    // flip_v3_v3(source_origin_symm, array->source_origin, array_symm_pass);
    mul_v3_m4v3(co, array->source_imat, co);
    mul_v3_m4v3(co, copy->imat, co);
    sub_v3_v3v3(co, co, source_origin_symm);
    copy_v3_v3(array->orco[i], co);
  }
}

void BKE_remesh_reproject_sculpt_face_sets(Mesh *target, Mesh *source)
{
  BVHTreeFromMesh bvhtree = {nullptr};

  const MPoly *target_polys = (const MPoly *)CustomData_get_layer(&target->pdata, CD_MPOLY);
  const MVert *target_verts = (const MVert *)CustomData_get_layer(&target->vdata, CD_MVERT);
  const MLoop *target_loops = (const MLoop *)CustomData_get_layer(&target->ldata, CD_MLOOP);

  int *target_face_sets;
  if (CustomData_has_layer(&target->pdata, CD_SCULPT_FACE_SETS)) {
    target_face_sets = (int *)CustomData_get_layer(&target->pdata, CD_SCULPT_FACE_SETS);
  }
  else {
    target_face_sets = (int *)CustomData_add_layer(
        &target->pdata, CD_SCULPT_FACE_SETS, CD_CALLOC, nullptr, target->totpoly);
  }

  const int *source_face_sets;
  if (CustomData_has_layer(&source->pdata, CD_SCULPT_FACE_SETS)) {
    source_face_sets = (const int *)CustomData_get_layer(&source->pdata, CD_SCULPT_FACE_SETS);
  }
  else {
    source_face_sets = (const int *)CustomData_add_layer(
        &source->pdata, CD_SCULPT_FACE_SETS, CD_CALLOC, nullptr, source->totpoly);
  }

  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(source);
  BKE_bvhtree_from_mesh_get(&bvhtree, source, BVHTREE_FROM_LOOPTRI, 2);

  for (int i = 0; i < target->totpoly; i++) {
    float from_co[3];
    BVHTreeNearest nearest;
    nearest.index = -1;
    nearest.dist_sq = FLT_MAX;
    const MPoly *mpoly = &target_polys[i];
    BKE_mesh_calc_poly_center(mpoly, &target_loops[mpoly->loopstart], target_verts, from_co);
    BLI_bvhtree_find_nearest(bvhtree.tree, from_co, &nearest, bvhtree.nearest_callback, &bvhtree);
    if (nearest.index != -1) {
      target_face_sets[i] = source_face_sets[looptri[nearest.index].poly];
    }
    else {
      target_face_sets[i] = 1;
    }
  }
  free_bvhtree_from_mesh(&bvhtree);
}

void BKE_remesh_reproject_materials(Mesh *target, Mesh *source)
{
  BVHTreeFromMesh bvhtree = {nullptr};
  bvhtree.nearest_callback = nullptr;

  const MPoly *target_polys = (MPoly *)CustomData_get_layer(&target->pdata, CD_MPOLY);
  const MVert *target_verts = (MVert *)CustomData_get_layer(&target->vdata, CD_MVERT);
  const MLoop *target_loops = (MLoop *)CustomData_get_layer(&target->ldata, CD_MLOOP);

  const MLoopTri *looptri = BKE_mesh_runtime_looptri_ensure(source);
  BKE_bvhtree_from_mesh_get(&bvhtree, source, BVHTREE_FROM_LOOPTRI, 2);

  for (int i = 0; i < target->totpoly; i++) {
    float from_co[3];
    BVHTreeNearest nearest;
    nearest.index = -1;
    nearest.dist_sq = FLT_MAX;
    const MPoly *mpoly = &target_polys[i];
    BKE_mesh_calc_poly_center(mpoly, &target_loops[mpoly->loopstart], target_verts, from_co);
    BLI_bvhtree_find_nearest(bvhtree.tree, from_co, &nearest, bvhtree.nearest_callback, &bvhtree);
    if (nearest.index != -1) {
      target->mpoly[i].mat_nr = source->mpoly[looptri[nearest.index].poly].mat_nr;
    }
  }
  free_bvhtree_from_mesh(&bvhtree);
}

void BKE_remesh_reproject_vertex_paint(Mesh *target, const Mesh *source)
{
  BVHTreeFromMesh bvhtree = {nullptr};
  BKE_bvhtree_from_mesh_get(&bvhtree, source, BVHTREE_FROM_VERTS, 2);

  int i = 0;
  const CustomDataLayer *layer;

  MeshElemMap *source_lmap = nullptr;
  int *source_lmap_mem = nullptr;
  MeshElemMap *target_lmap = nullptr;
  int *target_lmap_mem = nullptr;

  while ((layer = BKE_id_attribute_from_index(
              const_cast<ID *>(&source->id), i++, ATTR_DOMAIN_MASK_COLOR, CD_MASK_COLOR_ALL))) {
    AttributeDomain domain = BKE_id_attribute_domain(&source->id, layer);

    CustomData *target_cdata = domain == ATTR_DOMAIN_POINT ? &target->vdata : &target->ldata;
    const CustomData *source_cdata = domain == ATTR_DOMAIN_POINT ? &source->vdata : &source->ldata;

    /* Check attribute exists in target. */
    int layer_i = CustomData_get_named_layer_index(target_cdata, layer->type, layer->name);
    if (layer_i == -1) {
      int elem_num = domain == ATTR_DOMAIN_POINT ? target->totvert : target->totloop;

      CustomData_add_layer_named(
          target_cdata, layer->type, CD_CALLOC, nullptr, elem_num, layer->name);
      layer_i = CustomData_get_named_layer_index(target_cdata, layer->type, layer->name);
    }

    size_t data_size = CustomData_sizeof(layer->type);
    void *target_data = target_cdata->layers[layer_i].data;
    void *source_data = layer->data;
    MVert *target_verts = (MVert *)CustomData_get_layer(&target->vdata, CD_MVERT);

    if (domain == ATTR_DOMAIN_POINT) {
      for (int i = 0; i < target->totvert; i++) {
        BVHTreeNearest nearest;
        nearest.index = -1;
        nearest.dist_sq = FLT_MAX;
        BLI_bvhtree_find_nearest(
            bvhtree.tree, target_verts[i].co, &nearest, bvhtree.nearest_callback, &bvhtree);

        if (nearest.index != -1) {
          memcpy(POINTER_OFFSET(target_data, (size_t)i * data_size),
                 POINTER_OFFSET(source_data, (size_t)nearest.index * data_size),
                 data_size);
        }
      }
    }
    else {
      /* Lazily init vertex -> loop maps. */
      if (!source_lmap) {
        const MPoly *source_polys = (MPoly *)CustomData_get_layer(&source->pdata, CD_MPOLY);
        const MLoop *source_loops = (MLoop *)CustomData_get_layer(&source->ldata, CD_MLOOP);
        const MPoly *target_polys = (MPoly *)CustomData_get_layer(&target->pdata, CD_MPOLY);
        const MLoop *target_loops = (MLoop *)CustomData_get_layer(&target->ldata, CD_MLOOP);

        BKE_mesh_vert_loop_map_create(&source_lmap,
                                      &source_lmap_mem,
                                      source_polys,
                                      source_loops,
                                      source->totvert,
                                      source->totpoly,
                                      source->totloop);

        BKE_mesh_vert_loop_map_create(&target_lmap,
                                      &target_lmap_mem,
                                      target_polys,
                                      target_loops,
                                      target->totvert,
                                      target->totpoly,
                                      target->totloop);
      }

      for (int i = 0; i < target->totvert; i++) {
        BVHTreeNearest nearest;
        nearest.index = -1;
        nearest.dist_sq = FLT_MAX;
        BLI_bvhtree_find_nearest(
            bvhtree.tree, target_verts[i].co, &nearest, bvhtree.nearest_callback, &bvhtree);

        if (nearest.index == -1) {
          continue;
        }

        MeshElemMap *source_loops = source_lmap + nearest.index;
        MeshElemMap *target_loops = target_lmap + i;

        if (target_loops->count == 0 || source_loops->count == 0) {
          continue;
        }

        /*
         * Average color data for loops around the source vertex into
         * the first target loop around the target vertex
         */

        CustomData_interp(source_cdata,
                          target_cdata,
                          source_loops->indices,
                          nullptr,
                          nullptr,
                          source_loops->count,
                          target_loops->indices[0]);

        void *elem = POINTER_OFFSET(target_data, (size_t)target_loops->indices[0] * data_size);

        /* Copy to rest of target loops. */
        for (int j = 1; j < target_loops->count; j++) {
          memcpy(POINTER_OFFSET(target_data, (size_t)target_loops->indices[j] * data_size),
                 elem,
                 data_size);
        }
      }
    }
  }

  MEM_SAFE_FREE(source_lmap);
  MEM_SAFE_FREE(source_lmap_mem);
  MEM_SAFE_FREE(target_lmap);
  MEM_SAFE_FREE(target_lmap_mem);
  free_bvhtree_from_mesh(&bvhtree);

  /* Transfer active/render color attributes */

  CustomDataLayer *active_layer = BKE_id_attributes_active_color_get(&source->id);
  CustomDataLayer *render_layer = BKE_id_attributes_render_color_get(&source->id);

  if (active_layer) {
    BKE_id_attributes_active_color_set(
        &target->id, BKE_id_attributes_color_find(&target->id, active_layer->name));
  }

  if (render_layer) {
    BKE_id_attributes_render_color_set(
        &target->id, BKE_id_attributes_color_find(&target->id, render_layer->name));
  }
}

struct Mesh *BKE_mesh_remesh_voxel_fix_poles(const Mesh *mesh)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);

  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bmesh_create_params);

  BMeshFromMeshParams bmesh_from_mesh_params{};
  bmesh_from_mesh_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(nullptr, bm, mesh, &bmesh_from_mesh_params);

  BMVert *v;
  BMEdge *ed, *ed_next;
  BMFace *f, *f_next;
  BMIter iter_a, iter_b;

  /* Merge 3 edge poles vertices that exist in the same face */
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_ITER_MESH_MUTABLE (f, f_next, &iter_a, bm, BM_FACES_OF_MESH) {
    BMVert *v1, *v2;
    v1 = nullptr;
    v2 = nullptr;
    BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
      if (BM_vert_edge_count(v) == 3) {
        if (v1) {
          v2 = v;
        }
        else {
          v1 = v;
        }
      }
    }
    if (v1 && v2 && (v1 != v2) && !BM_edge_exists(v1, v2)) {
      BM_face_kill(bm, f);
      BMEdge *e = BM_edge_create(bm, v1, v2, nullptr, BM_CREATE_NOP);
      BM_elem_flag_set(e, BM_ELEM_TAG, true);
    }
  }

  BM_ITER_MESH_MUTABLE (ed, ed_next, &iter_a, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(ed, BM_ELEM_TAG)) {
      float co[3];
      mid_v3_v3v3(co, ed->v1->co, ed->v2->co);
      BMVert *vc = BM_edge_collapse(bm, ed, ed->v1, true, true, false, false, NULL);
      copy_v3_v3(vc->co, co);
    }
  }

  /* Delete faces with a 3 edge pole in all their vertices */
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_ITER_MESH (f, &iter_a, bm, BM_FACES_OF_MESH) {
    bool dissolve = true;
    BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
      if (BM_vert_edge_count(v) != 3) {
        dissolve = false;
      }
    }
    if (dissolve) {
      BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
        BM_elem_flag_set(v, BM_ELEM_TAG, true);
      }
    }
  }
  BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_VERTS);

  BM_ITER_MESH (ed, &iter_a, bm, BM_EDGES_OF_MESH) {
    if (BM_edge_face_count(ed) != 2) {
      BM_elem_flag_set(ed, BM_ELEM_TAG, true);
    }
  }
  BM_mesh_edgenet(bm, false, true);

  /* Smooth the result */
  for (int i = 0; i < 4; i++) {
    BM_ITER_MESH (v, &iter_a, bm, BM_VERTS_OF_MESH) {
      float co[3];
      zero_v3(co);
      BM_ITER_ELEM (ed, &iter_b, v, BM_EDGES_OF_VERT) {
        BMVert *vert = BM_edge_other_vert(ed, v);
        add_v3_v3(co, vert->co);
      }
      mul_v3_fl(co, 1.0f / (float)BM_vert_edge_count(v));
      mid_v3_v3v3(v->co, v->co, co);
    }
  }

  BM_mesh_normals_update(bm);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  BM_mesh_elem_hflag_enable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "recalc_face_normals faces=%hf",
               BM_ELEM_TAG);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMeshToMeshParams bmesh_to_mesh_params{};
  bmesh_to_mesh_params.calc_object_remap = false;
  Mesh *result = BKE_mesh_from_bmesh_nomain(bm, &bmesh_to_mesh_params, mesh);

  BM_mesh_free(bm);
  return result;
}
