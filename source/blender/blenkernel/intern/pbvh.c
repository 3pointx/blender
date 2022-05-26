/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLI_alloca.h"
#include "BLI_array.h"
#include "BLI_bitmap.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_string.h"
#include "BLI_task.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.h"
#include "BKE_ccg.h"
#include "BKE_main.h"
#include "BKE_mesh.h" /* for BKE_mesh_calc_normals */
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_subdiv_ccg.h"

#include "DEG_depsgraph_query.h"

#include "PIL_time.h"

#include "GPU_buffers.h"

#include "bmesh.h"

#include "atomic_ops.h"

#include "pbvh_intern.h"

#include <limits.h>

#define LEAF_LIMIT 4000
#define LEAF_DEPTH_LIMIT 28

//#define PERFCNTRS

#define STACK_FIXED_DEPTH 100

typedef struct PBVHStack {
  PBVHNode *node;
  bool revisiting;
} PBVHStack;

typedef struct PBVHIter {
  PBVH *pbvh;
  BKE_pbvh_SearchCallback scb;
  void *search_data;

  PBVHStack *stack;
  int stacksize;

  PBVHStack stackfixed[STACK_FIXED_DEPTH];
  int stackspace;
} PBVHIter;

void BB_reset(BB *bb)
{
  bb->bmin[0] = bb->bmin[1] = bb->bmin[2] = FLT_MAX;
  bb->bmax[0] = bb->bmax[1] = bb->bmax[2] = -FLT_MAX;
}

void BB_intersect(BB *r_out, BB *a, BB *b)
{
  for (int i = 0; i < 3; i++) {
    r_out->bmin[i] = max_ff(a->bmin[i], b->bmin[i]);
    r_out->bmax[i] = min_ff(a->bmax[i], b->bmax[i]);

    if (r_out->bmax[i] < r_out->bmin[i]) {
      r_out->bmax[i] = r_out->bmin[i] = 0.0f;
    }
  }
}

float BB_volume(const BB *bb)
{
  float dx = bb->bmax[0] - bb->bmin[0];
  float dy = bb->bmax[1] - bb->bmin[1];
  float dz = bb->bmax[2] - bb->bmin[2];

  return dx * dy * dz;
}

/* Expand the bounding box to include a new coordinate */
void BB_expand(BB *bb, const float co[3])
{
  for (int i = 0; i < 3; i++) {
    bb->bmin[i] = min_ff(bb->bmin[i], co[i]);
    bb->bmax[i] = max_ff(bb->bmax[i], co[i]);
  }
}

void BB_expand_with_bb(BB *bb, BB *bb2)
{
  for (int i = 0; i < 3; i++) {
    bb->bmin[i] = min_ff(bb->bmin[i], bb2->bmin[i]);
    bb->bmax[i] = max_ff(bb->bmax[i], bb2->bmax[i]);
  }
}

int BB_widest_axis(const BB *bb)
{
  float dim[3];

  for (int i = 0; i < 3; i++) {
    dim[i] = bb->bmax[i] - bb->bmin[i];
  }

  if (dim[0] > dim[1]) {
    if (dim[0] > dim[2]) {
      return 0;
    }

    return 2;
  }

  if (dim[1] > dim[2]) {
    return 1;
  }

  return 2;
}

void BBC_update_centroid(BBC *bbc)
{
  for (int i = 0; i < 3; i++) {
    bbc->bcentroid[i] = (bbc->bmin[i] + bbc->bmax[i]) * 0.5f;
  }
}

/* Not recursive */
static void update_node_vb(PBVH *pbvh, PBVHNode *node, int updateflag)
{
  if (!(updateflag & (PBVH_UpdateBB | PBVH_UpdateOriginalBB))) {
    return;
  }

  /* cannot clear flag here, causes leaky pbvh */
  // node->flag &= ~(updateflag & (PBVH_UpdateBB | PBVH_UpdateOriginalBB));

  BB vb;
  BB orig_vb;

  BB_reset(&vb);
  BB_reset(&orig_vb);

  bool do_orig = true;    // XXX updateflag & PBVH_UpdateOriginalBB;
  bool do_normal = true;  // XXX updateflag & PBVH_UpdateBB;

  if (node->flag & PBVH_Leaf) {
    PBVHVertexIter vd;

    BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_ALL) {
      if (do_normal) {
        BB_expand(&vb, vd.co);
      }

      if (do_orig) {
        MSculptVert *mv = pbvh->type == PBVH_BMESH ?
                              BM_ELEM_CD_GET_VOID_P(vd.bm_vert, pbvh->cd_sculpt_vert) :
                              pbvh->mdyntopo_verts + vd.index;

        if (mv->stroke_id != pbvh->stroke_id) {
          BB_expand(&orig_vb, vd.co);
        }
        else {
          BB_expand(&orig_vb, mv->origco);
        }
      }
    }
    BKE_pbvh_vertex_iter_end;
  }
  else {
    if (do_normal) {
      BB_expand_with_bb(&vb, &pbvh->nodes[node->children_offset].vb);
      BB_expand_with_bb(&vb, &pbvh->nodes[node->children_offset + 1].vb);
    }

    if (do_orig) {
      BB_expand_with_bb(&orig_vb, &pbvh->nodes[node->children_offset].orig_vb);
      BB_expand_with_bb(&orig_vb, &pbvh->nodes[node->children_offset + 1].orig_vb);
    }
  }

  if (do_normal) {
    node->vb = vb;
  }

  if (do_orig) {
#if 0
    float size[3];

    sub_v3_v3v3(size, orig_vb.bmax, orig_vb.bmin);
    mul_v3_fl(size, 0.05);

    sub_v3_v3(orig_vb.bmin, size);
    add_v3_v3(orig_vb.bmax, size);
#endif
    node->orig_vb = orig_vb;
  }
}

// void BKE_pbvh_node_BB_reset(PBVHNode *node)
//{
//  BB_reset(&node->vb);
//}
//
// void BKE_pbvh_node_BB_expand(PBVHNode *node, float co[3])
//{
//  BB_expand(&node->vb, co);
//}

static bool face_materials_match(const MPoly *f1, const MPoly *f2)
{
  return ((f1->flag & ME_SMOOTH) == (f2->flag & ME_SMOOTH) && (f1->mat_nr == f2->mat_nr));
}

static bool grid_materials_match(const DMFlagMat *f1, const DMFlagMat *f2)
{
  return ((f1->flag & ME_SMOOTH) == (f2->flag & ME_SMOOTH) && (f1->mat_nr == f2->mat_nr));
}

/* Adapted from BLI_kdopbvh.c */
/* Returns the index of the first element on the right of the partition */
static int partition_indices(int *prim_indices, int lo, int hi, int axis, float mid, BBC *prim_bbc)
{
  int i = lo, j = hi;
  for (;;) {
    for (; prim_bbc[prim_indices[i]].bcentroid[axis] < mid; i++) {
      /* pass */
    }
    for (; mid < prim_bbc[prim_indices[j]].bcentroid[axis]; j--) {
      /* pass */
    }

    if (!(i < j)) {
      return i;
    }

    SWAP(int, prim_indices[i], prim_indices[j]);
    i++;
  }
}

/* Returns the index of the first element on the right of the partition */
static int partition_indices_material(PBVH *pbvh, int lo, int hi)
{
  const MPoly *mpoly = pbvh->mpoly;
  const MLoopTri *looptri = pbvh->looptri;
  const DMFlagMat *flagmats = pbvh->grid_flag_mats;
  const int *indices = pbvh->prim_indices;
  const void *first;
  int i = lo, j = hi;

  if (pbvh->looptri) {
    first = &mpoly[looptri[pbvh->prim_indices[lo]].poly];
  }
  else {
    first = &flagmats[pbvh->prim_indices[lo]];
  }

  for (;;) {
    if (pbvh->looptri) {
      for (; face_materials_match(first, &mpoly[looptri[indices[i]].poly]); i++) {
        /* pass */
      }
      for (; !face_materials_match(first, &mpoly[looptri[indices[j]].poly]); j--) {
        /* pass */
      }
    }
    else {
      for (; grid_materials_match(first, &flagmats[indices[i]]); i++) {
        /* pass */
      }
      for (; !grid_materials_match(first, &flagmats[indices[j]]); j--) {
        /* pass */
      }
    }

    if (!(i < j)) {
      return i;
    }

    SWAP(int, pbvh->prim_indices[i], pbvh->prim_indices[j]);
    i++;
  }
}

void pbvh_grow_nodes(PBVH *pbvh, int totnode)
{
  if (UNLIKELY(totnode > pbvh->node_mem_count)) {
    pbvh->node_mem_count = pbvh->node_mem_count + (pbvh->node_mem_count / 3);
    if (pbvh->node_mem_count < totnode) {
      pbvh->node_mem_count = totnode;
    }
    pbvh->nodes = MEM_recallocN(pbvh->nodes, sizeof(PBVHNode) * pbvh->node_mem_count);
  }

  pbvh->totnode = totnode;

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!node->id) {
      node->id = ++pbvh->idgen;
    }
  }
}

/* Add a vertex to the map, with a positive value for unique vertices and
 * a negative value for additional vertices */
static int map_insert_vert(
    PBVH *pbvh, GHash *map, unsigned int *face_verts, unsigned int *uniq_verts, int vertex)
{
  void *key, **value_p;

  key = POINTER_FROM_INT(vertex);
  if (!BLI_ghash_ensure_p(map, key, &value_p)) {
    int value_i;
    if (!pbvh->vert_bitmap[vertex]) {
      pbvh->vert_bitmap[vertex] = true;
      value_i = *uniq_verts;
      (*uniq_verts)++;
    }
    else {
      value_i = ~(*face_verts);
      (*face_verts)++;
    }
    *value_p = POINTER_FROM_INT(value_i);
    return value_i;
  }

  return POINTER_AS_INT(*value_p);
}

/* Find vertices used by the faces in this node and update the draw buffers */
static void build_mesh_leaf_node(PBVH *pbvh, PBVHNode *node)
{
  bool has_visible = false;

  node->uniq_verts = node->face_verts = 0;
  const int totface = node->totprim;

  /* reserve size is rough guess */
  GHash *map = BLI_ghash_int_new_ex("build_mesh_leaf_node gh", 2 * totface);

  int(*face_vert_indices)[3] = MEM_mallocN(sizeof(int[3]) * totface, "bvh node face vert indices");

  node->face_vert_indices = (const int(*)[3])face_vert_indices;

  if (pbvh->respect_hide == false) {
    has_visible = true;
  }

  for (int i = 0; i < totface; i++) {
    const MLoopTri *lt = &pbvh->looptri[node->prim_indices[i]];
    for (int j = 0; j < 3; j++) {
      face_vert_indices[i][j] = map_insert_vert(
          pbvh, map, &node->face_verts, &node->uniq_verts, pbvh->mloop[lt->tri[j]].v);
    }

    if (has_visible == false) {
      if (!paint_is_face_hidden(lt, pbvh->verts, pbvh->mloop)) {
        has_visible = true;
      }
    }
  }

  int *vert_indices = MEM_callocN(sizeof(int) * (node->uniq_verts + node->face_verts),
                                  "bvh node vert indices");
  node->vert_indices = vert_indices;

  /* Build the vertex list, unique verts first */
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, map) {
    void *value = BLI_ghashIterator_getValue(&gh_iter);
    int ndx = POINTER_AS_INT(value);

    if (ndx < 0) {
      ndx = -ndx + node->uniq_verts - 1;
    }

    vert_indices[ndx] = POINTER_AS_INT(BLI_ghashIterator_getKey(&gh_iter));
  }

  for (int i = 0; i < totface; i++) {
    const int sides = 3;

    for (int j = 0; j < sides; j++) {
      if (face_vert_indices[i][j] < 0) {
        face_vert_indices[i][j] = -face_vert_indices[i][j] + node->uniq_verts - 1;
      }
    }
  }

  BKE_pbvh_node_mark_rebuild_draw(node);

  BKE_pbvh_node_fully_hidden_set(node, !has_visible);
  BKE_pbvh_node_mark_update_tri_area(node);

  BLI_ghash_free(map, NULL, NULL);
}

static void update_vb(PBVH *pbvh, PBVHNode *node, BBC *prim_bbc, int offset, int count)
{
  BB_reset(&node->vb);
  for (int i = offset + count - 1; i >= offset; i--) {
    BB_expand_with_bb(&node->vb, (BB *)(&prim_bbc[pbvh->prim_indices[i]]));
  }
  node->orig_vb = node->vb;
}

int BKE_pbvh_count_grid_quads(BLI_bitmap **grid_hidden,
                              const int *grid_indices,
                              int totgrid,
                              int gridsize)
{
  const int gridarea = (gridsize - 1) * (gridsize - 1);
  int totquad = 0;

  /* grid hidden layer is present, so have to check each grid for
   * visibility */

  for (int i = 0; i < totgrid; i++) {
    const BLI_bitmap *gh = grid_hidden[grid_indices[i]];

    if (gh) {
      /* grid hidden are present, have to check each element */
      for (int y = 0; y < gridsize - 1; y++) {
        for (int x = 0; x < gridsize - 1; x++) {
          if (!paint_is_grid_face_hidden(gh, gridsize, x, y)) {
            totquad++;
          }
        }
      }
    }
    else {
      totquad += gridarea;
    }
  }

  return totquad;
}

void BKE_pbvh_sync_face_sets_to_grids(PBVH *pbvh)
{
  const int gridsize = pbvh->gridkey.grid_size;
  for (int i = 0; i < pbvh->totgrid; i++) {
    BLI_bitmap *gh = pbvh->grid_hidden[i];
    const int face_index = BKE_subdiv_ccg_grid_to_face_index(pbvh->subdiv_ccg, i);
    if (!gh && pbvh->face_sets[face_index] < 0) {
      gh = pbvh->grid_hidden[i] = BLI_BITMAP_NEW(pbvh->gridkey.grid_area,
                                                 "partialvis_update_grids");
    }
    if (gh) {
      for (int y = 0; y < gridsize; y++) {
        for (int x = 0; x < gridsize; x++) {
          BLI_BITMAP_SET(gh, y * gridsize + x, pbvh->face_sets[face_index] < 0);
        }
      }
    }
  }
}

static void build_grid_leaf_node(PBVH *pbvh, PBVHNode *node)
{
  int totquads = BKE_pbvh_count_grid_quads(
      pbvh->grid_hidden, node->prim_indices, node->totprim, pbvh->gridkey.grid_size);
  BKE_pbvh_node_fully_hidden_set(node, (totquads == 0));
  BKE_pbvh_node_mark_rebuild_draw(node);
  BKE_pbvh_node_mark_update_tri_area(node);
}

static void build_leaf(PBVH *pbvh, int node_index, BBC *prim_bbc, int offset, int count)
{
  pbvh->nodes[node_index].flag |= PBVH_Leaf;

  pbvh->nodes[node_index].prim_indices = pbvh->prim_indices + offset;
  pbvh->nodes[node_index].totprim = count;

  /* Still need vb for searches */
  update_vb(pbvh, &pbvh->nodes[node_index], prim_bbc, offset, count);

  if (pbvh->looptri) {
    build_mesh_leaf_node(pbvh, pbvh->nodes + node_index);
  }
  else {
    build_grid_leaf_node(pbvh, pbvh->nodes + node_index);
  }
}

/* Return zero if all primitives in the node can be drawn with the
 * same material (including flat/smooth shading), non-zero otherwise */
static bool leaf_needs_material_split(PBVH *pbvh, int offset, int count)
{
  if (count <= 1) {
    return false;
  }

  if (pbvh->looptri) {
    const MLoopTri *first = &pbvh->looptri[pbvh->prim_indices[offset]];
    const MPoly *mp = &pbvh->mpoly[first->poly];

    for (int i = offset + count - 1; i > offset; i--) {
      int prim = pbvh->prim_indices[i];
      const MPoly *mp_other = &pbvh->mpoly[pbvh->looptri[prim].poly];
      if (!face_materials_match(mp, mp_other)) {
        return true;
      }
    }
  }
  else {
    const DMFlagMat *first = &pbvh->grid_flag_mats[pbvh->prim_indices[offset]];

    for (int i = offset + count - 1; i > offset; i--) {
      int prim = pbvh->prim_indices[i];
      if (!grid_materials_match(first, &pbvh->grid_flag_mats[prim])) {
        return true;
      }
    }
  }

  return false;
}

/* Recursively build a node in the tree
 *
 * vb is the voxel box around all of the primitives contained in
 * this node.
 *
 * cb is the bounding box around all the centroids of the primitives
 * contained in this node
 *
 * offset and start indicate a range in the array of primitive indices
 */

static void build_sub(
    PBVH *pbvh, int node_index, BB *cb, BBC *prim_bbc, int offset, int count, int depth)
{
  int end;
  BB cb_backing;

  /* Decide whether this is a leaf or not */
  const bool below_leaf_limit = count <= pbvh->leaf_limit || depth >= pbvh->depth_limit;

  if (below_leaf_limit) {
    if (!leaf_needs_material_split(pbvh, offset, count)) {
      build_leaf(pbvh, node_index, prim_bbc, offset, count);
      return;
    }
  }

  /* Add two child nodes */
  pbvh->nodes[node_index].children_offset = pbvh->totnode;
  pbvh_grow_nodes(pbvh, pbvh->totnode + 2);

  /* Update parent node bounding box */
  update_vb(pbvh, &pbvh->nodes[node_index], prim_bbc, offset, count);

  if (!below_leaf_limit) {
    /* Find axis with widest range of primitive centroids */
    if (!cb) {
      cb = &cb_backing;
      BB_reset(cb);
      for (int i = offset + count - 1; i >= offset; i--) {
        BB_expand(cb, prim_bbc[pbvh->prim_indices[i]].bcentroid);
      }
    }
    const int axis = BB_widest_axis(cb);

    /* Partition primitives along that axis */
    end = partition_indices(pbvh->prim_indices,
                            offset,
                            offset + count - 1,
                            axis,
                            (cb->bmax[axis] + cb->bmin[axis]) * 0.5f,
                            prim_bbc);
  }
  else {
    /* Partition primitives by material */
    end = partition_indices_material(pbvh, offset, offset + count - 1);
  }

  /* Build children */
  build_sub(pbvh,
            pbvh->nodes[node_index].children_offset,
            NULL,
            prim_bbc,
            offset,
            end - offset,
            depth + 1);
  build_sub(pbvh,
            pbvh->nodes[node_index].children_offset + 1,
            NULL,
            prim_bbc,
            end,
            offset + count - end,
            depth + 1);
}

static void pbvh_build(PBVH *pbvh, BB *cb, BBC *prim_bbc, int totprim)
{
  if (totprim != pbvh->totprim) {
    pbvh->totprim = totprim;
    if (pbvh->nodes) {
      MEM_freeN(pbvh->nodes);
    }
    if (pbvh->prim_indices) {
      MEM_freeN(pbvh->prim_indices);
    }
    pbvh->prim_indices = MEM_mallocN(sizeof(int) * totprim, "bvh prim indices");
    for (int i = 0; i < totprim; i++) {
      pbvh->prim_indices[i] = i;
    }
    pbvh->totnode = 0;
    if (pbvh->node_mem_count < 100) {
      pbvh->node_mem_count = 100;
      pbvh->nodes = MEM_callocN(sizeof(PBVHNode) * pbvh->node_mem_count, "bvh initial nodes");
    }
  }

  pbvh->totnode = 1;
  build_sub(pbvh, 0, cb, prim_bbc, 0, totprim, 0);
}

void BKE_pbvh_set_face_areas(PBVH *pbvh, float *face_areas)
{
  pbvh->face_areas = face_areas;
}

void BKE_pbvh_set_sculpt_verts(PBVH *pbvh, MSculptVert *sverts)
{
  pbvh->mdyntopo_verts = sverts;
}

void BKE_pbvh_build_mesh(PBVH *pbvh,
                         Mesh *mesh,
                         const MPoly *mpoly,
                         const MLoop *mloop,
                         MVert *verts,
                         MSculptVert *mdyntopo_verts,
                         int totvert,
                         struct CustomData *vdata,
                         struct CustomData *ldata,
                         struct CustomData *pdata,
                         const MLoopTri *looptri,
                         int looptri_num,
                         bool fast_draw,
                         float *face_areas,
                         SculptPMap *pmap)
{
  BBC *prim_bbc = NULL;
  BB cb;

  pbvh->pmap = pmap;
  pbvh->face_areas = face_areas;
  pbvh->mesh = mesh;
  pbvh->type = PBVH_FACES;
  pbvh->mpoly = mpoly;
  pbvh->mloop = mloop;
  pbvh->looptri = looptri;
  pbvh->verts = verts;
  pbvh->mdyntopo_verts = mdyntopo_verts;
  BKE_mesh_vertex_normals_ensure(mesh);
  pbvh->vert_normals = BKE_mesh_vertex_normals_for_write(mesh);
  pbvh->vert_bitmap = MEM_calloc_arrayN(totvert, sizeof(bool), "bvh->vert_bitmap");
  pbvh->totvert = totvert;
  pbvh->leaf_limit = LEAF_LIMIT;
  pbvh->depth_limit = LEAF_DEPTH_LIMIT;

  pbvh->vdata = vdata;
  pbvh->ldata = ldata;
  pbvh->pdata = pdata;

  pbvh->face_sets_color_seed = mesh->face_sets_color_seed;
  pbvh->face_sets_color_default = mesh->face_sets_color_default;

  BB_reset(&cb);

  /* For each face, store the AABB and the AABB centroid */
  prim_bbc = MEM_mallocN(sizeof(BBC) * looptri_num, "prim_bbc");

  for (int i = 0; i < looptri_num; i++) {
    const MLoopTri *lt = &looptri[i];
    const int sides = 3;
    BBC *bbc = prim_bbc + i;

    BB_reset((BB *)bbc);

    for (int j = 0; j < sides; j++) {
      BB_expand((BB *)bbc, verts[pbvh->mloop[lt->tri[j]].v].co);
    }

    BBC_update_centroid(bbc);

    BB_expand(&cb, bbc->bcentroid);
  }

  if (looptri_num) {
    pbvh_build(pbvh, &cb, prim_bbc, looptri_num);
  }

  if (fast_draw) {
    pbvh->flags |= PBVH_FAST_DRAW;
  }

  MEM_freeN(prim_bbc);

  /* Clear the bitmap so it can be used as an update tag later on. */
  memset(pbvh->vert_bitmap, 0, sizeof(bool) * totvert);

  BKE_pbvh_update_active_vcol(pbvh, mesh);
}

void BKE_pbvh_build_grids(PBVH *pbvh,
                          CCGElem **grids,
                          int totgrid,
                          CCGKey *key,
                          void **gridfaces,
                          DMFlagMat *flagmats,
                          BLI_bitmap **grid_hidden,
                          bool fast_draw,
                          float *face_areas)
{
  const int gridsize = key->grid_size;

  pbvh->face_areas = face_areas;
  pbvh->type = PBVH_GRIDS;
  pbvh->grids = grids;
  pbvh->gridfaces = gridfaces;
  pbvh->grid_flag_mats = flagmats;
  pbvh->totgrid = totgrid;
  pbvh->gridkey = *key;
  pbvh->grid_hidden = grid_hidden;
  pbvh->leaf_limit = max_ii(LEAF_LIMIT / (gridsize * gridsize), 1);
  pbvh->depth_limit = LEAF_DEPTH_LIMIT;

  BB cb;
  BB_reset(&cb);

  /* For each grid, store the AABB and the AABB centroid */
  BBC *prim_bbc = MEM_mallocN(sizeof(BBC) * totgrid, "prim_bbc");

  for (int i = 0; i < totgrid; i++) {
    CCGElem *grid = grids[i];
    BBC *bbc = prim_bbc + i;

    BB_reset((BB *)bbc);

    for (int j = 0; j < gridsize * gridsize; j++) {
      BB_expand((BB *)bbc, CCG_elem_offset_co(key, grid, j));
    }

    BBC_update_centroid(bbc);

    BB_expand(&cb, bbc->bcentroid);
  }

  if (totgrid) {
    pbvh_build(pbvh, &cb, prim_bbc, totgrid);
  }

  if (fast_draw) {
    pbvh->flags |= PBVH_FAST_DRAW;
  }

  MEM_freeN(prim_bbc);
}

PBVH *BKE_pbvh_new(void)
{
  PBVH *pbvh = MEM_callocN(sizeof(PBVH), "pbvh");
  pbvh->respect_hide = true;
  pbvh->draw_cache_invalid = true;
  return pbvh;
}

void BKE_pbvh_free(PBVH *pbvh)
{
  BKE_pbvh_cache_remove(pbvh);

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = &pbvh->nodes[i];

    if (node->flag & PBVH_Leaf) {
      pbvh_free_draw_buffers(pbvh, node);

      if (node->vert_indices) {
        MEM_freeN((void *)node->vert_indices);
      }
      if (node->loop_indices) {
        MEM_freeN(node->loop_indices);
      }
      if (node->face_vert_indices) {
        MEM_freeN((void *)node->face_vert_indices);
      }
      if (node->bm_faces) {
        BLI_table_gset_free(node->bm_faces, NULL);
      }
      if (node->bm_unique_verts) {
        BLI_table_gset_free(node->bm_unique_verts, NULL);
      }
      if (node->bm_other_verts) {
        BLI_table_gset_free(node->bm_other_verts, NULL);
      }

      if (node->tribuf || node->tri_buffers) {
        BKE_pbvh_bmesh_free_tris(pbvh, node);
      }

#ifdef PROXY_ADVANCED
      BKE_pbvh_free_proxyarray(pbvh, node);
#endif
      pbvh_pixels_free(node);
    }
  }

  if (pbvh->deformed) {
    if (pbvh->verts) {
      /* if pbvh was deformed, new memory was allocated for verts/faces -- free it */

      MEM_freeN((void *)pbvh->verts);
    }

    pbvh->verts = NULL;
  }

  if (pbvh->looptri) {
    MEM_freeN((void *)pbvh->looptri);
  }

  if (pbvh->nodes) {
    MEM_freeN(pbvh->nodes);
  }

  if (pbvh->prim_indices) {
    MEM_freeN(pbvh->prim_indices);
  }

  MEM_SAFE_FREE(pbvh->vert_bitmap);

  BKE_pbvh_pmap_release(pbvh->pmap);
  pbvh->pmap = NULL;

  pbvh->invalid = true;

  MEM_freeN(pbvh);
}

static void pbvh_iter_begin(PBVHIter *iter,
                            PBVH *pbvh,
                            BKE_pbvh_SearchCallback scb,
                            void *search_data)
{
  iter->pbvh = pbvh;
  iter->scb = scb;
  iter->search_data = search_data;

  iter->stack = iter->stackfixed;
  iter->stackspace = STACK_FIXED_DEPTH;

  iter->stack[0].node = pbvh->nodes;
  iter->stack[0].revisiting = false;
  iter->stacksize = 1;
}

static void pbvh_iter_end(PBVHIter *iter)
{
  if (iter->stackspace > STACK_FIXED_DEPTH) {
    MEM_freeN(iter->stack);
  }
}

static void pbvh_stack_push(PBVHIter *iter, PBVHNode *node, bool revisiting)
{
  if (UNLIKELY(iter->stacksize == iter->stackspace)) {
    iter->stackspace *= 2;
    if (iter->stackspace != (STACK_FIXED_DEPTH * 2)) {
      iter->stack = MEM_reallocN(iter->stack, sizeof(PBVHStack) * iter->stackspace);
    }
    else {
      iter->stack = MEM_mallocN(sizeof(PBVHStack) * iter->stackspace, "PBVHStack");
      memcpy(iter->stack, iter->stackfixed, sizeof(PBVHStack) * iter->stacksize);
    }
  }

  iter->stack[iter->stacksize].node = node;
  iter->stack[iter->stacksize].revisiting = revisiting;
  iter->stacksize++;
}

static PBVHNode *pbvh_iter_next(PBVHIter *iter)
{
  /* purpose here is to traverse tree, visiting child nodes before their
   * parents, this order is necessary for e.g. computing bounding boxes */

  while (iter->stacksize) {
    /* pop node */
    iter->stacksize--;
    PBVHNode *node = iter->stack[iter->stacksize].node;

    /* on a mesh with no faces this can happen
     * can remove this check if we know meshes have at least 1 face */
    if (node == NULL) {
      return NULL;
    }

    bool revisiting = iter->stack[iter->stacksize].revisiting;

    /* revisiting node already checked */
    if (revisiting) {
      return node;
    }

    if (iter->scb && !iter->scb(node, iter->search_data)) {
      continue; /* don't traverse, outside of search zone */
    }

    if (node->flag & PBVH_Leaf) {
      /* immediately hit leaf node */
      return node;
    }

    /* come back later when children are done */
    pbvh_stack_push(iter, node, true);

    /* push two child nodes on the stack */
    pbvh_stack_push(iter, iter->pbvh->nodes + node->children_offset + 1, false);
    pbvh_stack_push(iter, iter->pbvh->nodes + node->children_offset, false);
  }

  return NULL;
}

static PBVHNode *pbvh_iter_next_occluded(PBVHIter *iter)
{
  while (iter->stacksize) {
    /* pop node */
    iter->stacksize--;
    PBVHNode *node = iter->stack[iter->stacksize].node;

    /* on a mesh with no faces this can happen
     * can remove this check if we know meshes have at least 1 face */
    if (node == NULL) {
      return NULL;
    }

    float ff = dot_v3v3(node->vb.bmin, node->vb.bmax);
    if (isnan(ff) || !isfinite(ff)) {
      printf("%s: nan!\n", __func__);
    }

    if (iter->scb && !iter->scb(node, iter->search_data)) {
      continue; /* don't traverse, outside of search zone */
    }

    if (node->flag & PBVH_Leaf) {
      /* immediately hit leaf node */
      return node;
    }

    pbvh_stack_push(iter, iter->pbvh->nodes + node->children_offset + 1, false);
    pbvh_stack_push(iter, iter->pbvh->nodes + node->children_offset, false);
  }

  return NULL;
}

void BKE_pbvh_search_gather(
    PBVH *pbvh, BKE_pbvh_SearchCallback scb, void *search_data, PBVHNode ***r_array, int *r_tot)
{
  PBVHIter iter;
  PBVHNode **array = NULL, *node;
  int tot = 0, space = 0;

  pbvh_iter_begin(&iter, pbvh, scb, search_data);

  while ((node = pbvh_iter_next(&iter))) {
    if (node->flag & PBVH_Leaf) {
      if (UNLIKELY(tot == space)) {
        /* resize array if needed */
        space = (tot == 0) ? 32 : space * 2;
        array = MEM_recallocN_id(array, sizeof(PBVHNode *) * space, __func__);
      }

      array[tot] = node;
      tot++;
    }
  }

  pbvh_iter_end(&iter);

  if (tot == 0 && array) {
    MEM_freeN(array);
    array = NULL;
  }

  *r_array = array;
  *r_tot = tot;
}

void BKE_pbvh_search_callback(PBVH *pbvh,
                              BKE_pbvh_SearchCallback scb,
                              void *search_data,
                              BKE_pbvh_HitCallback hcb,
                              void *hit_data)
{
  PBVHIter iter;
  PBVHNode *node;

  pbvh_iter_begin(&iter, pbvh, scb, search_data);

  while ((node = pbvh_iter_next(&iter))) {
    if (node->flag & PBVH_Leaf) {
      hcb(node, hit_data);
    }
  }

  pbvh_iter_end(&iter);
}

typedef struct node_tree {
  PBVHNode *data;

  struct node_tree *left;
  struct node_tree *right;
} node_tree;

static void node_tree_insert(node_tree *tree, node_tree *new_node)
{
  if (new_node->data->tmin < tree->data->tmin) {
    if (tree->left) {
      node_tree_insert(tree->left, new_node);
    }
    else {
      tree->left = new_node;
    }
  }
  else {
    if (tree->right) {
      node_tree_insert(tree->right, new_node);
    }
    else {
      tree->right = new_node;
    }
  }
}

static void traverse_tree(node_tree *tree,
                          BKE_pbvh_HitOccludedCallback hcb,
                          void *hit_data,
                          float *tmin)
{
  if (tree->left) {
    traverse_tree(tree->left, hcb, hit_data, tmin);
  }

  hcb(tree->data, hit_data, tmin);

  if (tree->right) {
    traverse_tree(tree->right, hcb, hit_data, tmin);
  }
}

static void free_tree(node_tree *tree)
{
  if (tree->left) {
    free_tree(tree->left);
    tree->left = NULL;
  }

  if (tree->right) {
    free_tree(tree->right);
    tree->right = NULL;
  }

  free(tree);
}

float BKE_pbvh_node_get_tmin(PBVHNode *node)
{
  return node->tmin;
}

static void BKE_pbvh_search_callback_occluded(PBVH *pbvh,
                                              BKE_pbvh_SearchCallback scb,
                                              void *search_data,
                                              BKE_pbvh_HitOccludedCallback hcb,
                                              void *hit_data)
{
  PBVHIter iter;
  PBVHNode *node;
  node_tree *tree = NULL;

  pbvh_iter_begin(&iter, pbvh, scb, search_data);

  while ((node = pbvh_iter_next_occluded(&iter))) {
    if (node->flag & PBVH_Leaf) {
      node_tree *new_node = malloc(sizeof(node_tree));

      new_node->data = node;

      new_node->left = NULL;
      new_node->right = NULL;

      if (tree) {
        node_tree_insert(tree, new_node);
      }
      else {
        tree = new_node;
      }
    }
  }

  pbvh_iter_end(&iter);

  if (tree) {
    float tmin = FLT_MAX;
    traverse_tree(tree, hcb, hit_data, &tmin);
    free_tree(tree);
  }
}

static bool update_search_cb(PBVHNode *node, void *data_v)
{
  int flag = POINTER_AS_INT(data_v);

  if (node->flag & PBVH_Leaf) {
    return (node->flag & flag) != 0;
  }

  return true;
}

typedef struct PBVHUpdateData {
  PBVH *pbvh;
  PBVHNode **nodes;
  int totnode;

  float (*vnors)[3];
  int flag;
  bool show_sculpt_face_sets;
  bool flat_vcol_shading;
  Mesh *mesh;
} PBVHUpdateData;

static void pbvh_update_normals_clear_task_cb(void *__restrict userdata,
                                              const int n,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  float(*vnors)[3] = data->vnors;

  if (node->flag & PBVH_UpdateNormals) {
    const int *verts = node->vert_indices;
    const int totvert = node->uniq_verts;
    for (int i = 0; i < totvert; i++) {
      const int v = verts[i];
      if (pbvh->vert_bitmap[v]) {
        zero_v3(vnors[v]);
      }
    }
  }
}

static void pbvh_update_normals_accum_task_cb(void *__restrict userdata,
                                              const int n,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  PBVHUpdateData *data = userdata;

  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  float(*vnors)[3] = data->vnors;

  if (node->flag & PBVH_UpdateNormals) {
    unsigned int mpoly_prev = UINT_MAX;
    float fn[3];

    const int *faces = node->prim_indices;
    const int totface = node->totprim;

    for (int i = 0; i < totface; i++) {
      const MLoopTri *lt = &pbvh->looptri[faces[i]];
      const unsigned int vtri[3] = {
          pbvh->mloop[lt->tri[0]].v,
          pbvh->mloop[lt->tri[1]].v,
          pbvh->mloop[lt->tri[2]].v,
      };
      const int sides = 3;

      /* Face normal and mask */
      if (lt->poly != mpoly_prev) {
        const MPoly *mp = &pbvh->mpoly[lt->poly];
        BKE_mesh_calc_poly_normal(mp, &pbvh->mloop[mp->loopstart], pbvh->verts, fn);
        mpoly_prev = lt->poly;
      }

      for (int j = sides; j--;) {
        const int v = vtri[j];

        if (pbvh->vert_bitmap[v]) {
          /* NOTE: This avoids `lock, add_v3_v3, unlock`
           * and is five to ten times quicker than a spin-lock.
           * Not exact equivalent though, since atomicity is only ensured for one component
           * of the vector at a time, but here it shall not make any sensible difference. */
          for (int k = 3; k--;) {
            atomic_add_and_fetch_fl(&vnors[v][k], fn[k]);
          }
        }
      }
    }
  }
}

static void pbvh_update_normals_store_task_cb(void *__restrict userdata,
                                              const int n,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  float(*vnors)[3] = data->vnors;

  if (node->flag & PBVH_UpdateNormals) {
    const int *verts = node->vert_indices;
    const int totvert = node->uniq_verts;

    for (int i = 0; i < totvert; i++) {
      const int v = verts[i];

      /* No atomics necessary because we are iterating over uniq_verts only,
       * so we know only this thread will handle this vertex. */
      if (pbvh->vert_bitmap[v]) {
        normalize_v3(vnors[v]);
        pbvh->vert_bitmap[v] = false;
      }
    }

    node->flag &= ~PBVH_UpdateNormals;
  }
}

static void pbvh_faces_update_normals(PBVH *pbvh, PBVHNode **nodes, int totnode)
{
  /* subtle assumptions:
   * - We know that for all edited vertices, the nodes with faces
   *   adjacent to these vertices have been marked with PBVH_UpdateNormals.
   *   This is true because if the vertex is inside the brush radius, the
   *   bounding box of its adjacent faces will be as well.
   * - However this is only true for the vertices that have actually been
   *   edited, not for all vertices in the nodes marked for update, so we
   *   can only update vertices marked in the `vert_bitmap`.
   */

  PBVHUpdateData data = {
      .pbvh = pbvh,
      .nodes = nodes,
      .vnors = pbvh->vert_normals,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);

  /* Zero normals before accumulation. */
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_normals_clear_task_cb, &settings);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_normals_accum_task_cb, &settings);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_normals_store_task_cb, &settings);
}

static void pbvh_update_mask_redraw_task_cb(void *__restrict userdata,
                                            const int n,
                                            const TaskParallelTLS *__restrict UNUSED(tls))
{

  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  if (node->flag & PBVH_UpdateMask) {

    bool has_unmasked = false;
    bool has_masked = true;
    if (node->flag & PBVH_Leaf) {
      PBVHVertexIter vd;

      BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_ALL) {
        if (vd.mask && *vd.mask < 1.0f) {
          has_unmasked = true;
        }
        if (vd.mask && *vd.mask > 0.0f) {
          has_masked = false;
        }
      }
      BKE_pbvh_vertex_iter_end;
    }
    else {
      has_unmasked = true;
      has_masked = true;
    }
    BKE_pbvh_node_fully_masked_set(node, !has_unmasked);
    BKE_pbvh_node_fully_unmasked_set(node, has_masked);

    node->flag &= ~PBVH_UpdateMask;
  }
}

static void pbvh_update_mask_redraw(PBVH *pbvh, PBVHNode **nodes, int totnode, int flag)
{
  PBVHUpdateData data = {
      .pbvh = pbvh,
      .nodes = nodes,
      .flag = flag,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_mask_redraw_task_cb, &settings);
}

static void pbvh_update_visibility_redraw_task_cb(void *__restrict userdata,
                                                  const int n,
                                                  const TaskParallelTLS *__restrict UNUSED(tls))
{

  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  if (node->flag & PBVH_UpdateVisibility) {
    node->flag &= ~PBVH_UpdateVisibility;
    BKE_pbvh_node_fully_hidden_set(node, true);
    if (node->flag & PBVH_Leaf) {
      PBVHVertexIter vd;
      BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_ALL) {
        if (vd.visible) {
          BKE_pbvh_node_fully_hidden_set(node, false);
          return;
        }
      }
      BKE_pbvh_vertex_iter_end;
    }
  }
}

static void pbvh_update_visibility_redraw(PBVH *pbvh, PBVHNode **nodes, int totnode, int flag)
{
  PBVHUpdateData data = {
      .pbvh = pbvh,
      .nodes = nodes,
      .flag = flag,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_visibility_redraw_task_cb, &settings);
}

static void pbvh_update_BB_redraw_task_cb(void *__restrict userdata,
                                          const int n,
                                          const TaskParallelTLS *__restrict UNUSED(tls))
{
  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  const int flag = data->flag;

  update_node_vb(pbvh, node, flag);

  if ((flag & PBVH_UpdateRedraw) && (node->flag & PBVH_UpdateRedraw)) {
    node->flag &= ~PBVH_UpdateRedraw;
  }
}

void pbvh_update_BB_redraw(PBVH *pbvh, PBVHNode **nodes, int totnode, int flag)
{
  /* update BB, redraw flag */
  PBVHUpdateData data = {
      .pbvh = pbvh,
      .nodes = nodes,
      .flag = flag,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_BB_redraw_task_cb, &settings);
}

static int pbvh_get_buffers_update_flags(PBVH *UNUSED(pbvh))
{
  int update_flags = GPU_PBVH_BUFFERS_SHOW_VCOL | GPU_PBVH_BUFFERS_SHOW_MASK |
                     GPU_PBVH_BUFFERS_SHOW_SCULPT_FACE_SETS;
  return update_flags;
}

bool BKE_pbvh_get_color_layer(const Mesh *me, CustomDataLayer **r_layer, AttributeDomain *r_attr)
{
  CustomDataLayer *layer = BKE_id_attributes_active_color_get((ID *)me);

  if (!layer || !ELEM(layer->type, CD_PROP_COLOR, CD_PROP_BYTE_COLOR)) {
    *r_layer = NULL;
    *r_attr = ATTR_DOMAIN_NUM;
    return false;
  }

  AttributeDomain domain = BKE_id_attribute_domain((ID *)me, layer);

  if (!ELEM(domain, ATTR_DOMAIN_POINT, ATTR_DOMAIN_CORNER)) {
    *r_layer = NULL;
    *r_attr = ATTR_DOMAIN_NUM;
    return false;
  }

  *r_layer = layer;
  *r_attr = domain;

  return true;
}

static void pbvh_update_draw_buffer_cb(void *__restrict userdata,
                                       const int n,
                                       const TaskParallelTLS *__restrict UNUSED(tls))
{
  /* Create and update draw buffers. The functions called here must not
   * do any OpenGL calls. Flags are not cleared immediately, that happens
   * after GPU_pbvh_buffer_flush() which does the final OpenGL calls. */
  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  Mesh *me = data->mesh;

  CustomDataLayer *vcol_layer = NULL;
  AttributeDomain vcol_domain;

  BKE_pbvh_get_color_layer(me, &vcol_layer, &vcol_domain);

  CustomData *vdata, *ldata;

  if (!pbvh->bm) {
    vdata = pbvh->vdata ? pbvh->vdata : &me->vdata;
    ldata = pbvh->ldata ? pbvh->ldata : &me->ldata;
  }
  else {
    vdata = &pbvh->bm->vdata;
    ldata = &pbvh->bm->ldata;
  }

  Mesh me_query;
  BKE_id_attribute_copy_domains_temp(ID_ME, vdata, NULL, ldata, NULL, NULL, &me_query.id);

  CustomDataLayer *render_vcol_layer = BKE_id_attributes_render_color_get(&me_query.id);

  if (node->flag & PBVH_RebuildDrawBuffers) {
    node->updategen++;

    switch (pbvh->type) {
      case PBVH_GRIDS:
        node->draw_buffers = GPU_pbvh_grid_buffers_build(node->totprim, pbvh->grid_hidden);
        break;
      case PBVH_FACES:
        node->draw_buffers = GPU_pbvh_mesh_buffers_build(
            pbvh->mpoly,
            pbvh->mloop,
            pbvh->looptri,
            pbvh->verts,
            node->prim_indices,
            CustomData_get_layer(pbvh->pdata, CD_SCULPT_FACE_SETS),
            node->totprim,
            pbvh->mesh);
        break;
      case PBVH_BMESH: {
        BKE_pbvh_bmesh_check_tris(pbvh, node);

        node->tot_mat_draw_buffers = node->tot_tri_buffers;

        if (node->tot_tri_buffers) {
          node->mat_draw_buffers = MEM_malloc_arrayN(
              node->tot_tri_buffers, sizeof(void *), "node->mat_draw_buffers");
        }

        for (int i = 0; i < node->tot_tri_buffers; i++) {
          node->mat_draw_buffers[i] = GPU_pbvh_bmesh_buffers_build(pbvh->flags &
                                                                   PBVH_DYNTOPO_SMOOTH_SHADING);
        }

        break;
      }
    }
  }

  if (node->flag & PBVH_UpdateDrawBuffers) {
    node->updategen++;

    const int update_flags = pbvh_get_buffers_update_flags(pbvh);
    switch (pbvh->type) {
      case PBVH_GRIDS:
        GPU_pbvh_grid_buffers_update(node->draw_buffers,
                                     pbvh->subdiv_ccg,
                                     pbvh->grids,
                                     pbvh->grid_flag_mats,
                                     node->prim_indices,
                                     node->totprim,
                                     pbvh->face_sets,
                                     pbvh->face_sets_color_seed,
                                     pbvh->face_sets_color_default,
                                     &pbvh->gridkey,
                                     update_flags);
        break;
      case PBVH_FACES: {
        CustomDataLayer *layer = NULL;
        AttributeDomain domain;

        BKE_pbvh_get_color_layer(pbvh->mesh, &layer, &domain);

        GPU_pbvh_mesh_buffers_update(node->draw_buffers,
                                     pbvh->verts,
                                     pbvh->mloop,
                                     pbvh->mpoly,
                                     pbvh->looptri,
                                     vdata,
                                     ldata,
                                     CustomData_get_layer(pbvh->vdata, CD_PAINT_MASK),
                                     layer,
                                     render_vcol_layer,
                                     domain,
                                     CustomData_get_layer(pbvh->pdata, CD_SCULPT_FACE_SETS),
                                     pbvh->face_sets_color_seed,
                                     pbvh->face_sets_color_default,
                                     update_flags,
                                     pbvh->vert_normals,
                                     pbvh->mdyntopo_verts);
      } break;
      case PBVH_BMESH:
        if (BKE_pbvh_bmesh_check_tris(pbvh, node)) {
          pbvh_free_draw_buffers(pbvh, node);
          node->tot_mat_draw_buffers = node->tot_tri_buffers;

          pbvh_bmesh_check_other_verts(node);

          if (node->tot_tri_buffers) {
            node->mat_draw_buffers = MEM_malloc_arrayN(
                node->tot_tri_buffers, sizeof(void *), "node->mat_draw_buffers");

            for (int i = 0; i < node->tot_tri_buffers; i++) {
              node->mat_draw_buffers[i] = GPU_pbvh_bmesh_buffers_build(
                  pbvh->flags & PBVH_DYNTOPO_SMOOTH_SHADING);
            }
          }
        }

        for (int i = 0; i < node->tot_mat_draw_buffers; i++) {
          if (i >= node->tot_tri_buffers) {
            printf("node->tot_mat_draw_buffers >= node->tot_tri_buffers! %d %d\n",
                   node->tot_mat_draw_buffers,
                   node->tot_tri_buffers);
            continue;
          }

          PBVHGPUBuildArgs args = {.buffers = node->mat_draw_buffers[i],
                                   .bm = pbvh->bm,
                                   .bm_faces = node->bm_faces,
                                   .bm_unique_verts = node->bm_unique_verts,
                                   .bm_other_verts = node->bm_other_verts,
                                   .tribuf = node->tri_buffers + i,
                                   .update_flags = update_flags,
                                   .cd_vert_node_offset = pbvh->cd_vert_node_offset,
                                   .face_sets_color_seed = pbvh->face_sets_color_seed,
                                   .face_sets_color_default = pbvh->face_sets_color_default,
                                   .flat_vcol = data->flat_vcol_shading,
                                   .mat_nr = node->tri_buffers[i].mat_nr,
                                   .active_vcol_domain = pbvh->color_domain,
                                   .active_vcol_type = pbvh->color_type,
                                   .active_vcol_layer = pbvh->color_layer,
                                   .render_vcol_layer = render_vcol_layer};

          GPU_pbvh_bmesh_buffers_update(&args);
        }
        break;
    }
  }
}

void BKE_pbvh_set_flat_vcol_shading(PBVH *pbvh, bool value)
{
  if (value != pbvh->flat_vcol_shading) {
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if (!(node->flag & PBVH_Leaf)) {
        continue;
      }

      BKE_pbvh_node_mark_rebuild_draw(node);
    }
  }

  pbvh->flat_vcol_shading = value;
}

void pbvh_free_draw_buffers(PBVH *pbvh, PBVHNode *node)
{
  if (node->draw_buffers) {
    GPU_pbvh_buffers_free(node->draw_buffers);
    node->draw_buffers = NULL;
    pbvh->draw_cache_invalid = true;
  }

  for (int i = 0; i < node->tot_mat_draw_buffers; i++) {
    GPU_pbvh_buffers_free(node->mat_draw_buffers[i]);
    pbvh->draw_cache_invalid = true;
  }

  MEM_SAFE_FREE(node->mat_draw_buffers);

  node->mat_draw_buffers = NULL;
  node->tot_mat_draw_buffers = 0;
}

void pbvh_update_free_all_draw_buffers(PBVH *pbvh, PBVHNode *node)
{
  if (pbvh->type == PBVH_GRIDS) {
    GPU_pbvh_grid_buffers_update_free(
        node->draw_buffers, pbvh->grid_flag_mats, node->prim_indices);
  }
  else if (pbvh->type == PBVH_BMESH) {
    for (int i = 0; i < node->tot_mat_draw_buffers; i++) {
      GPU_pbvh_bmesh_buffers_update_free(node->mat_draw_buffers[i]);
    }
  }
}

static void pbvh_update_draw_buffers(
    PBVH *pbvh, Mesh *me, PBVHNode **nodes, int totnode, int update_flag)
{

  CustomData *vdata;
  CustomData *ldata;

  if (pbvh->type == PBVH_BMESH) {
    if (pbvh->bm) {
      vdata = &pbvh->bm->vdata;
      ldata = &pbvh->bm->ldata;
    }
    else {
      vdata = ldata = NULL;
    }
  }
  else {
    vdata = pbvh->vdata;
    ldata = pbvh->ldata;
  }

  if (!vdata && me) {
    vdata = &me->vdata;
  }

  if (!ldata && me) {
    ldata = &me->ldata;
  }

  Mesh me_query;
  BKE_id_attribute_copy_domains_temp(ID_ME, vdata, NULL, ldata, NULL, NULL, &me_query.id);

  CustomDataLayer *vcol_layer = NULL;
  AttributeDomain domain;
  BKE_pbvh_get_color_layer(&me_query, &vcol_layer, &domain);
  CustomDataLayer *render_vcol_layer = BKE_id_attributes_render_color_get(&me_query.id);

  /* rebuild all draw buffers if attribute layout changed */
  if (GPU_pbvh_update_attribute_names(vdata,
                                      ldata,
                                      GPU_pbvh_need_full_render_get(),
                                      pbvh->flags & PBVH_FAST_DRAW,
                                      pbvh->color_type,
                                      pbvh->color_domain,
                                      vcol_layer,
                                      render_vcol_layer,
                                      !GPU_pbvh_need_full_render_get())) {
    // attribute layout changed; force rebuild
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if (node->flag & PBVH_Leaf) {
        node->flag |= PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw;
      }
    }
  }

  if ((update_flag & PBVH_RebuildDrawBuffers) || ELEM(pbvh->type, PBVH_GRIDS, PBVH_BMESH)) {
    /* Free buffers uses OpenGL, so not in parallel. */
    for (int n = 0; n < totnode; n++) {
      PBVHNode *node = nodes[n];
      if (node->flag & PBVH_RebuildDrawBuffers) {
        pbvh_free_draw_buffers(pbvh, node);
      }
      else if ((node->flag & PBVH_UpdateDrawBuffers)) {
        pbvh_update_free_all_draw_buffers(pbvh, node);
      }
    }
  }

  /* Parallel creation and update of draw buffers. */
  PBVHUpdateData data = {
      .pbvh = pbvh, .nodes = nodes, .flat_vcol_shading = pbvh->flat_vcol_shading, .mesh = me};

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_draw_buffer_cb, &settings);

  for (int i = 0; i < totnode; i++) {
    PBVHNode *node = nodes[i];

    if (node->flag & PBVH_UpdateDrawBuffers) {
      /* Flush buffers uses OpenGL, so not in parallel. */
      if (node->draw_buffers) {
        GPU_pbvh_buffers_update_flush(node->draw_buffers);
      }

      for (int i = 0; i < node->tot_mat_draw_buffers; i++) {
        GPU_pbvh_buffers_update_flush(node->mat_draw_buffers[i]);
      }
    }

    node->flag &= ~(PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers);
  }
}

static int pbvh_flush_bb(PBVH *pbvh, PBVHNode *node, int flag)
{
  int update = 0;

  /* Difficult to multi-thread well, we just do single threaded recursive. */
  if (node->flag & PBVH_Leaf) {
    if (flag & PBVH_UpdateBB) {
      update |= (node->flag & PBVH_UpdateBB);
      node->flag &= ~PBVH_UpdateBB;
    }

    if (flag & PBVH_UpdateOriginalBB) {
      update |= (node->flag & PBVH_UpdateOriginalBB);
      node->flag &= ~PBVH_UpdateOriginalBB;
    }

    return update;
  }

  update |= pbvh_flush_bb(pbvh, pbvh->nodes + node->children_offset, flag);
  update |= pbvh_flush_bb(pbvh, pbvh->nodes + node->children_offset + 1, flag);

  update_node_vb(pbvh, node, update);

  return update;
}

void BKE_pbvh_update_bounds(PBVH *pbvh, int flag)
{
  if (!pbvh->nodes) {
    return;
  }

  PBVHNode **nodes;
  int totnode;

  BKE_pbvh_search_gather(pbvh, update_search_cb, POINTER_FROM_INT(flag), &nodes, &totnode);

  if (flag & (PBVH_UpdateBB | PBVH_UpdateOriginalBB | PBVH_UpdateRedraw)) {
    pbvh_update_BB_redraw(pbvh, nodes, totnode, flag);
  }

  if (flag & (PBVH_UpdateBB | PBVH_UpdateOriginalBB)) {
    pbvh_flush_bb(pbvh, pbvh->nodes, flag);
  }

  MEM_SAFE_FREE(nodes);
}

void BKE_pbvh_update_vertex_data(PBVH *pbvh, int flag)
{
  if (!pbvh->nodes) {
    return;
  }

  PBVHNode **nodes;
  int totnode;

  BKE_pbvh_search_gather(pbvh, update_search_cb, POINTER_FROM_INT(flag), &nodes, &totnode);

  if (flag & (PBVH_UpdateMask)) {
    pbvh_update_mask_redraw(pbvh, nodes, totnode, flag);
  }

  if (flag & (PBVH_UpdateColor)) {
    for (int i = 0; i < totnode; i++) {
      nodes[i]->flag |= PBVH_UpdateRedraw | PBVH_UpdateDrawBuffers | PBVH_UpdateColor;
    }
  }

  if (flag & (PBVH_UpdateVisibility)) {
    pbvh_update_visibility_redraw(pbvh, nodes, totnode, flag);
  }

  if (nodes) {
    MEM_freeN(nodes);
  }
}

static void pbvh_faces_node_visibility_update(PBVH *pbvh, PBVHNode *node)
{
  MVert *mvert;
  const int *vert_indices;
  int totvert, i;
  BKE_pbvh_node_num_verts(pbvh, node, NULL, &totvert);
  BKE_pbvh_node_get_verts(pbvh, node, &vert_indices, &mvert);

  for (i = 0; i < totvert; i++) {
    MVert *v = &mvert[vert_indices[i]];
    if (!(v->flag & ME_HIDE)) {
      BKE_pbvh_node_fully_hidden_set(node, false);
      return;
    }
  }

  BKE_pbvh_node_fully_hidden_set(node, true);
}

static void pbvh_grids_node_visibility_update(PBVH *pbvh, PBVHNode *node)
{
  CCGElem **grids;
  BLI_bitmap **grid_hidden;
  int *grid_indices, totgrid, i;

  BKE_pbvh_node_get_grids(pbvh, node, &grid_indices, &totgrid, NULL, NULL, &grids);
  grid_hidden = BKE_pbvh_grid_hidden(pbvh);
  CCGKey key = *BKE_pbvh_get_grid_key(pbvh);

  for (i = 0; i < totgrid; i++) {
    int g = grid_indices[i], x, y;
    BLI_bitmap *gh = grid_hidden[g];

    if (!gh) {
      BKE_pbvh_node_fully_hidden_set(node, false);
      return;
    }

    for (y = 0; y < key.grid_size; y++) {
      for (x = 0; x < key.grid_size; x++) {
        if (!BLI_BITMAP_TEST(gh, y * key.grid_size + x)) {
          BKE_pbvh_node_fully_hidden_set(node, false);
          return;
        }
      }
    }
  }
  BKE_pbvh_node_fully_hidden_set(node, true);
}

static void pbvh_bmesh_node_visibility_update(PBVHNode *node)
{
  TableGSet *unique, *other;

  unique = BKE_pbvh_bmesh_node_unique_verts(node);
  other = BKE_pbvh_bmesh_node_other_verts(node);

  BMVert *v;

  TGSET_ITER (v, unique) {
    if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      BKE_pbvh_node_fully_hidden_set(node, false);
      return;
    }
  }
  TGSET_ITER_END

  TGSET_ITER (v, other) {
    if (!BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      BKE_pbvh_node_fully_hidden_set(node, false);
      return;
    }
  }
  TGSET_ITER_END

  BKE_pbvh_node_fully_hidden_set(node, true);
}

static void pbvh_update_visibility_task_cb(void *__restrict userdata,
                                           const int n,
                                           const TaskParallelTLS *__restrict UNUSED(tls))
{

  PBVHUpdateData *data = userdata;
  PBVH *pbvh = data->pbvh;
  PBVHNode *node = data->nodes[n];
  if (node->flag & PBVH_UpdateVisibility) {
    switch (BKE_pbvh_type(pbvh)) {
      case PBVH_FACES:
        pbvh_faces_node_visibility_update(pbvh, node);
        break;
      case PBVH_GRIDS:
        pbvh_grids_node_visibility_update(pbvh, node);
        break;
      case PBVH_BMESH:
        pbvh_bmesh_node_visibility_update(node);
        break;
    }
    node->flag &= ~PBVH_UpdateVisibility;
  }
}

static void pbvh_update_visibility(PBVH *pbvh, PBVHNode **nodes, int totnode)
{
  PBVHUpdateData data = {
      .pbvh = pbvh,
      .nodes = nodes,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_visibility_task_cb, &settings);
}

void BKE_pbvh_update_visibility(PBVH *pbvh)
{
  if (!pbvh->nodes) {
    return;
  }

  PBVHNode **nodes;
  int totnode;

  BKE_pbvh_search_gather(
      pbvh, update_search_cb, POINTER_FROM_INT(PBVH_UpdateVisibility), &nodes, &totnode);
  pbvh_update_visibility(pbvh, nodes, totnode);

  if (nodes) {
    MEM_freeN(nodes);
  }
}

void BKE_pbvh_redraw_BB(PBVH *pbvh, float bb_min[3], float bb_max[3])
{
  PBVHIter iter;
  PBVHNode *node;
  BB bb;

  BB_reset(&bb);

  pbvh_iter_begin(&iter, pbvh, NULL, NULL);

  while ((node = pbvh_iter_next(&iter))) {
    if (node->flag & PBVH_UpdateRedraw) {
      BB_expand_with_bb(&bb, &node->vb);
    }
  }

  pbvh_iter_end(&iter);

  copy_v3_v3(bb_min, bb.bmin);
  copy_v3_v3(bb_max, bb.bmax);
}

void BKE_pbvh_get_grid_updates(PBVH *pbvh, bool clear, void ***r_gridfaces, int *r_totface)
{
  GSet *face_set = BLI_gset_ptr_new(__func__);
  PBVHNode *node;
  PBVHIter iter;

  pbvh_iter_begin(&iter, pbvh, NULL, NULL);

  while ((node = pbvh_iter_next(&iter))) {
    if (node->flag & PBVH_UpdateNormals) {
      for (uint i = 0; i < node->totprim; i++) {
        void *face = pbvh->gridfaces[node->prim_indices[i]];
        BLI_gset_add(face_set, face);
      }

      if (clear) {
        node->flag &= ~PBVH_UpdateNormals;
      }
    }
  }

  pbvh_iter_end(&iter);

  const int tot = BLI_gset_len(face_set);
  if (tot == 0) {
    *r_totface = 0;
    *r_gridfaces = NULL;
    BLI_gset_free(face_set, NULL);
    return;
  }

  void **faces = MEM_mallocN(sizeof(*faces) * tot, "PBVH Grid Faces");

  GSetIterator gs_iter;
  int i;
  GSET_ITER_INDEX (gs_iter, face_set, i) {
    faces[i] = BLI_gsetIterator_getKey(&gs_iter);
  }

  BLI_gset_free(face_set, NULL);

  *r_totface = tot;
  *r_gridfaces = faces;
}

/***************************** PBVH Access ***********************************/

PBVHType BKE_pbvh_type(const PBVH *pbvh)
{
  return pbvh->type;
}

bool BKE_pbvh_has_faces(const PBVH *pbvh)
{
  if (pbvh->type == PBVH_BMESH) {
    return (pbvh->bm->totface != 0);
  }

  return (pbvh->totprim != 0);
}

void BKE_pbvh_bounding_box(const PBVH *pbvh, float min[3], float max[3])
{
  if (pbvh->totnode) {
    const BB *bb = &pbvh->nodes[0].vb;
    copy_v3_v3(min, bb->bmin);
    copy_v3_v3(max, bb->bmax);
  }
  else {
    zero_v3(min);
    zero_v3(max);
  }
}

BLI_bitmap **BKE_pbvh_grid_hidden(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return pbvh->grid_hidden;
}

const CCGKey *BKE_pbvh_get_grid_key(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return &pbvh->gridkey;
}

struct CCGElem **BKE_pbvh_get_grids(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return pbvh->grids;
}

BLI_bitmap **BKE_pbvh_get_grid_visibility(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return pbvh->grid_hidden;
}

int BKE_pbvh_get_grid_num_vertices(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return pbvh->totgrid * pbvh->gridkey.grid_area;
}

int BKE_pbvh_get_grid_num_faces(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_GRIDS);
  return pbvh->totgrid * (pbvh->gridkey.grid_size - 1) * (pbvh->gridkey.grid_size - 1);
}

BMesh *BKE_pbvh_get_bmesh(PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_BMESH);
  return pbvh->bm;
}

/***************************** Node Access ***********************************/

void BKE_pbvh_node_mark_original_update(PBVHNode *node)
{
  node->flag |= PBVH_UpdateOriginalBB;
}

void BKE_pbvh_node_mark_update(PBVHNode *node)
{
  node->flag |= PBVH_UpdateNormals | PBVH_UpdateBB | PBVH_UpdateOriginalBB |
                PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw | PBVH_UpdateCurvatureDir |
                PBVH_RebuildPixels | PBVH_UpdateTriAreas;
}

void BKE_pbvh_node_mark_update_mask(PBVHNode *node)
{
  node->flag |= PBVH_UpdateMask | PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw;
}

void BKE_pbvh_node_mark_update_color(PBVHNode *node)
{
  node->flag |= PBVH_UpdateColor | PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw;
}

void BKE_pbvh_mark_rebuild_pixels(PBVH *pbvh)
{
  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = &pbvh->nodes[n];
    if (node->flag & PBVH_Leaf) {
      node->flag |= PBVH_RebuildPixels;
    }
  }
}

void BKE_pbvh_node_mark_update_visibility(PBVHNode *node)
{
  node->flag |= PBVH_UpdateVisibility | PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers |
                PBVH_UpdateRedraw | PBVH_UpdateCurvatureDir;
}

void BKE_pbvh_node_mark_rebuild_draw(PBVHNode *node)
{
  node->flag |= PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw |
                PBVH_UpdateCurvatureDir;
}

void BKE_pbvh_node_mark_redraw(PBVHNode *node)
{
  node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateRedraw;
}

void BKE_pbvh_node_mark_normals_update(PBVHNode *node)
{
  node->flag |= PBVH_UpdateNormals | PBVH_UpdateCurvatureDir;
}

void BKE_pbvh_node_mark_curvature_update(PBVHNode *node)
{
  node->flag |= PBVH_UpdateCurvatureDir;
}

void BKE_pbvh_curvature_update_set(PBVHNode *node, bool state)
{
  if (state) {
    node->flag |= PBVH_UpdateCurvatureDir;
  }
  else {
    node->flag &= ~PBVH_UpdateCurvatureDir;
  }
}

bool BKE_pbvh_curvature_update_get(PBVHNode *node)
{
  return node->flag & PBVH_UpdateCurvatureDir;
}

void BKE_pbvh_node_fully_hidden_set(PBVHNode *node, int fully_hidden)
{
  BLI_assert(node->flag & PBVH_Leaf);

  if (fully_hidden) {
    node->flag |= PBVH_FullyHidden;
  }
  else {
    node->flag &= ~PBVH_FullyHidden;
  }
}

bool BKE_pbvh_node_fully_hidden_get(PBVHNode *node)
{
  return (node->flag & PBVH_Leaf) && (node->flag & PBVH_FullyHidden);
}

void BKE_pbvh_node_fully_masked_set(PBVHNode *node, int fully_masked)
{
  BLI_assert(node->flag & PBVH_Leaf);

  if (fully_masked) {
    node->flag |= PBVH_FullyMasked;
  }
  else {
    node->flag &= ~PBVH_FullyMasked;
  }
}

bool BKE_pbvh_node_fully_masked_get(PBVHNode *node)
{
  return (node->flag & PBVH_Leaf) && (node->flag & PBVH_FullyMasked);
}

void BKE_pbvh_node_fully_unmasked_set(PBVHNode *node, int fully_masked)
{
  BLI_assert(node->flag & PBVH_Leaf);

  if (fully_masked) {
    node->flag |= PBVH_FullyUnmasked;
  }
  else {
    node->flag &= ~PBVH_FullyUnmasked;
  }
}

bool BKE_pbvh_node_fully_unmasked_get(PBVHNode *node)
{
  return (node->flag & PBVH_Leaf) && (node->flag & PBVH_FullyUnmasked);
}

void BKE_pbvh_vert_mark_update(PBVH *pbvh, SculptVertRef vertex)
{
  BLI_assert(pbvh->type == PBVH_FACES);
  pbvh->vert_bitmap[vertex.i] = true;
}

void BKE_pbvh_node_get_loops(PBVH *pbvh,
                             PBVHNode *node,
                             const int **r_loop_indices,
                             const MLoop **r_loops)
{
  BLI_assert(BKE_pbvh_type(pbvh) == PBVH_FACES);

  if (r_loop_indices) {
    *r_loop_indices = node->loop_indices;
  }

  if (r_loops) {
    *r_loops = pbvh->mloop;
  }
}

void BKE_pbvh_node_get_verts(PBVH *pbvh,
                             PBVHNode *node,
                             const int **r_vert_indices,
                             MVert **r_verts)
{
  if (r_vert_indices) {
    *r_vert_indices = node->vert_indices;
  }

  if (r_verts) {
    *r_verts = pbvh->verts;
  }
}

void BKE_pbvh_node_num_verts(PBVH *pbvh, PBVHNode *node, int *r_uniquevert, int *r_totvert)
{
  int tot;

  switch (pbvh->type) {
    case PBVH_GRIDS:
      tot = node->totprim * pbvh->gridkey.grid_area;
      if (r_totvert) {
        *r_totvert = tot;
      }
      if (r_uniquevert) {
        *r_uniquevert = tot;
      }
      break;
    case PBVH_FACES:
      if (r_totvert) {
        *r_totvert = node->uniq_verts + node->face_verts;
      }
      if (r_uniquevert) {
        *r_uniquevert = node->uniq_verts;
      }
      break;
    case PBVH_BMESH:
      // not a leaf? return zero
      if (!(node->flag & PBVH_Leaf)) {
        if (r_totvert) {
          *r_totvert = 0;
        }

        if (r_uniquevert) {
          *r_uniquevert = 0;
        }

        return;
      }

      pbvh_bmesh_check_other_verts(node);

      tot = BLI_table_gset_len(node->bm_unique_verts);
      if (r_totvert) {
        *r_totvert = tot + BLI_table_gset_len(node->bm_other_verts);
      }
      if (r_uniquevert) {
        *r_uniquevert = tot;
      }
      break;
  }
}

void BKE_pbvh_node_get_grids(PBVH *pbvh,
                             PBVHNode *node,
                             int **r_grid_indices,
                             int *r_totgrid,
                             int *r_maxgrid,
                             int *r_gridsize,
                             CCGElem ***r_griddata)
{
  switch (pbvh->type) {
    case PBVH_GRIDS:
      if (r_grid_indices) {
        *r_grid_indices = node->prim_indices;
      }
      if (r_totgrid) {
        *r_totgrid = node->totprim;
      }
      if (r_maxgrid) {
        *r_maxgrid = pbvh->totgrid;
      }
      if (r_gridsize) {
        *r_gridsize = pbvh->gridkey.grid_size;
      }
      if (r_griddata) {
        *r_griddata = pbvh->grids;
      }
      break;
    case PBVH_FACES:
    case PBVH_BMESH:
      if (r_grid_indices) {
        *r_grid_indices = NULL;
      }
      if (r_totgrid) {
        *r_totgrid = 0;
      }
      if (r_maxgrid) {
        *r_maxgrid = 0;
      }
      if (r_gridsize) {
        *r_gridsize = 0;
      }
      if (r_griddata) {
        *r_griddata = NULL;
      }
      break;
  }
}

void BKE_pbvh_node_get_BB(PBVHNode *node, float bb_min[3], float bb_max[3])
{
  copy_v3_v3(bb_min, node->vb.bmin);
  copy_v3_v3(bb_max, node->vb.bmax);
}

void BKE_pbvh_node_get_original_BB(PBVHNode *node, float bb_min[3], float bb_max[3])
{
  copy_v3_v3(bb_min, node->orig_vb.bmin);
  copy_v3_v3(bb_max, node->orig_vb.bmax);
}

void BKE_pbvh_node_get_proxies(PBVHNode *node, PBVHProxyNode **proxies, int *proxy_count)
{
  if (node->proxy_count > 0) {
    if (proxies) {
      *proxies = node->proxies;
    }
    if (proxy_count) {
      *proxy_count = node->proxy_count;
    }
  }
  else {
    if (proxies) {
      *proxies = NULL;
    }
    if (proxy_count) {
      *proxy_count = 0;
    }
  }
}

/**
 * \note doing a full search on all vertices here seems expensive,
 * however this is important to avoid having to recalculate bound-box & sync the buffers to the
 * GPU (which is far more expensive!) See: T47232.
 */
bool BKE_pbvh_node_vert_update_check_any(PBVH *pbvh, PBVHNode *node)
{
  BLI_assert(pbvh->type == PBVH_FACES);
  const int *verts = node->vert_indices;
  const int totvert = node->uniq_verts + node->face_verts;

  for (int i = 0; i < totvert; i++) {
    const int v = verts[i];

    if (pbvh->vert_bitmap[v]) {
      return true;
    }
  }

  return false;
}

/********************************* Ray-cast ***********************************/

typedef struct {
  struct IsectRayAABB_Precalc ray;
  bool original;
  int stroke_id;
} RaycastData;

static bool ray_aabb_intersect(PBVHNode *node, void *data_v)
{
  RaycastData *rcd = data_v;
  const float *bb_min, *bb_max;

  if (rcd->original) {
    /* BKE_pbvh_node_get_original_BB */
    bb_min = node->orig_vb.bmin;
    bb_max = node->orig_vb.bmax;
  }
  else {
    /* BKE_pbvh_node_get_BB */
    bb_min = node->vb.bmin;
    bb_max = node->vb.bmax;
  }

  return isect_ray_aabb_v3(&rcd->ray, bb_min, bb_max, &node->tmin);
}

void BKE_pbvh_raycast(PBVH *pbvh,
                      BKE_pbvh_HitOccludedCallback cb,
                      void *data,
                      const float ray_start[3],
                      const float ray_normal[3],
                      bool original,
                      int stroke_id)
{
  RaycastData rcd;

  isect_ray_aabb_v3_precalc(&rcd.ray, ray_start, ray_normal);

  rcd.original = original;
  rcd.stroke_id = stroke_id;
  pbvh->stroke_id = stroke_id;

  BKE_pbvh_search_callback_occluded(pbvh, ray_aabb_intersect, &rcd, cb, data);
}

bool ray_face_intersection_quad(const float ray_start[3],
                                struct IsectRayPrecalc *isect_precalc,
                                const float t0[3],
                                const float t1[3],
                                const float t2[3],
                                const float t3[3],
                                float *depth)
{
  float depth_test;

  if ((isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t1, t2, &depth_test, NULL) &&
       (depth_test < *depth)) ||
      (isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t2, t3, &depth_test, NULL) &&
       (depth_test < *depth))) {
    *depth = depth_test;
    return true;
  }

  return false;
}

bool ray_face_intersection_tri(const float ray_start[3],
                               struct IsectRayPrecalc *isect_precalc,
                               const float t0[3],
                               const float t1[3],
                               const float t2[3],
                               float *depth)
{
  float depth_test;
  if ((isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t1, t2, &depth_test, NULL) &&
       (depth_test < *depth))) {
    *depth = depth_test;
    return true;
  }

  return false;
}

bool ray_update_depth_and_hit_count(const float depth_test,
                                    float *r_depth,
                                    float *r_back_depth,
                                    int *hit_count)
{
  (*hit_count)++;
  if (depth_test < *r_depth) {
    *r_back_depth = *r_depth;
    *r_depth = depth_test;
    return true;
  }
  else if (depth_test > *r_depth && depth_test <= *r_back_depth) {
    *r_back_depth = depth_test;
    return false;
  }

  return false;
}

bool ray_face_intersection_depth_quad(const float ray_start[3],
                                      struct IsectRayPrecalc *isect_precalc,
                                      const float t0[3],
                                      const float t1[3],
                                      const float t2[3],
                                      const float t3[3],
                                      float *r_depth,
                                      float *r_back_depth,
                                      int *hit_count)
{
  float depth_test;
  if (!(isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t1, t2, &depth_test, NULL) ||
        isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t2, t3, &depth_test, NULL))) {
    return false;
  }
  return ray_update_depth_and_hit_count(depth_test, r_depth, r_back_depth, hit_count);
}

bool ray_face_intersection_depth_tri(const float ray_start[3],
                                     struct IsectRayPrecalc *isect_precalc,
                                     const float t0[3],
                                     const float t1[3],
                                     const float t2[3],
                                     float *r_depth,
                                     float *r_back_depth,
                                     int *hit_count)
{
  float depth_test;

  if (!isect_ray_tri_watertight_v3(ray_start, isect_precalc, t0, t1, t2, &depth_test, NULL)) {
    return false;
  }
  return ray_update_depth_and_hit_count(depth_test, r_depth, r_back_depth, hit_count);
}

/* Take advantage of the fact we know this won't be an intersection.
 * Just handle ray-tri edges. */
static float dist_squared_ray_to_tri_v3_fast(const float ray_origin[3],
                                             const float ray_direction[3],
                                             const float v0[3],
                                             const float v1[3],
                                             const float v2[3],
                                             float r_point[3],
                                             float *r_depth)
{
  const float *tri[3] = {v0, v1, v2};
  float dist_sq_best = FLT_MAX;
  for (int i = 0, j = 2; i < 3; j = i++) {
    float point_test[3], depth_test = FLT_MAX;
    const float dist_sq_test = dist_squared_ray_to_seg_v3(
        ray_origin, ray_direction, tri[i], tri[j], point_test, &depth_test);
    if (dist_sq_test < dist_sq_best || i == 0) {
      copy_v3_v3(r_point, point_test);
      *r_depth = depth_test;
      dist_sq_best = dist_sq_test;
    }
  }
  return dist_sq_best;
}

bool ray_face_nearest_quad(const float ray_start[3],
                           const float ray_normal[3],
                           const float t0[3],
                           const float t1[3],
                           const float t2[3],
                           const float t3[3],
                           float *depth,
                           float *dist_sq)
{
  float dist_sq_test;
  float co[3], depth_test;

  if (((dist_sq_test = dist_squared_ray_to_tri_v3_fast(
            ray_start, ray_normal, t0, t1, t2, co, &depth_test)) < *dist_sq)) {
    *dist_sq = dist_sq_test;
    *depth = depth_test;
    if (((dist_sq_test = dist_squared_ray_to_tri_v3_fast(
              ray_start, ray_normal, t0, t2, t3, co, &depth_test)) < *dist_sq)) {
      *dist_sq = dist_sq_test;
      *depth = depth_test;
    }
    return true;
  }

  return false;
}

bool ray_face_nearest_tri(const float ray_start[3],
                          const float ray_normal[3],
                          const float t0[3],
                          const float t1[3],
                          const float t2[3],
                          float *depth,
                          float *dist_sq)
{
  float dist_sq_test;
  float co[3], depth_test;

  if (((dist_sq_test = dist_squared_ray_to_tri_v3_fast(
            ray_start, ray_normal, t0, t1, t2, co, &depth_test)) < *dist_sq)) {
    *dist_sq = dist_sq_test;
    *depth = depth_test;
    return true;
  }

  return false;
}

static bool pbvh_faces_node_raycast(PBVH *pbvh,
                                    const PBVHNode *node,
                                    float (*origco)[3],
                                    const float ray_start[3],
                                    const float ray_normal[3],
                                    struct IsectRayPrecalc *isect_precalc,
                                    int *hit_count,
                                    float *depth,

                                    float *depth_back,
                                    SculptVertRef *r_active_vertex_index,
                                    SculptFaceRef *r_active_face_index,
                                    float *r_face_normal,
                                    int stroke_id)
{
  const MVert *vert = pbvh->verts;
  const MLoop *mloop = pbvh->mloop;
  const int *faces = node->prim_indices;
  int totface = node->totprim;
  bool hit = false;
  float nearest_vertex_co[3] = {0.0f};

  for (int i = 0; i < totface; i++) {
    const MLoopTri *lt = &pbvh->looptri[faces[i]];
    const int *face_verts = node->face_vert_indices[i];

    if (pbvh->respect_hide && paint_is_face_hidden(lt, vert, mloop)) {
      continue;
    }

    const float *co[3];
    if (origco) {
      /* Intersect with backed up original coordinates. */
      co[0] = origco[face_verts[0]];
      co[1] = origco[face_verts[1]];
      co[2] = origco[face_verts[2]];
    }
    else {
      /* intersect with current coordinates */
      co[0] = vert[mloop[lt->tri[0]].v].co;
      co[1] = vert[mloop[lt->tri[1]].v].co;
      co[2] = vert[mloop[lt->tri[2]].v].co;
    }

    if (!ray_face_intersection_depth_tri(
            ray_start, isect_precalc, co[0], co[1], co[2], depth, depth_back, hit_count)) {
      continue;
    }

    hit = true;

    if (r_face_normal) {
      normal_tri_v3(r_face_normal, co[0], co[1], co[2]);
    }

    if (r_active_vertex_index) {
      float location[3] = {0.0f};
      madd_v3_v3v3fl(location, ray_start, ray_normal, *depth);
      for (int j = 0; j < 3; j++) {
        /* Always assign nearest_vertex_co in the first iteration to avoid comparison against
         * uninitialized values. This stores the closest vertex in the current intersecting
         * triangle. */
        if (j == 0 ||
            len_squared_v3v3(location, co[j]) < len_squared_v3v3(location, nearest_vertex_co)) {
          copy_v3_v3(nearest_vertex_co, co[j]);
          *r_active_vertex_index = (SculptVertRef){.i = mloop[lt->tri[j]].v};
          *r_active_face_index = (SculptFaceRef){.i = lt->poly};
        }
      }
    }
  }

  return hit;
}

static bool pbvh_grids_node_raycast(PBVH *pbvh,
                                    PBVHNode *node,
                                    float (*origco)[3],
                                    const float ray_start[3],
                                    const float ray_normal[3],
                                    struct IsectRayPrecalc *isect_precalc,
                                    int *hit_count,
                                    float *depth,
                                    float *back_depth,

                                    SculptVertRef *r_active_vertex_index,
                                    SculptFaceRef *r_active_grid_index,
                                    float *r_face_normal)
{
  const int totgrid = node->totprim;
  const int gridsize = pbvh->gridkey.grid_size;
  bool hit = false;
  float nearest_vertex_co[3] = {0.0};
  const CCGKey *gridkey = &pbvh->gridkey;

  for (int i = 0; i < totgrid; i++) {
    const int grid_index = node->prim_indices[i];
    CCGElem *grid = pbvh->grids[grid_index];
    BLI_bitmap *gh;

    if (!grid) {
      continue;
    }

    gh = pbvh->grid_hidden[grid_index];

    for (int y = 0; y < gridsize - 1; y++) {
      for (int x = 0; x < gridsize - 1; x++) {
        /* check if grid face is hidden */
        if (gh) {
          if (paint_is_grid_face_hidden(gh, gridsize, x, y)) {
            continue;
          }
        }

        const float *co[4];
        if (origco) {
          co[0] = origco[y * gridsize + x];
          co[1] = origco[y * gridsize + x + 1];
          co[2] = origco[(y + 1) * gridsize + x + 1];
          co[3] = origco[(y + 1) * gridsize + x];
        }
        else {
          co[0] = CCG_grid_elem_co(gridkey, grid, x, y);
          co[1] = CCG_grid_elem_co(gridkey, grid, x + 1, y);
          co[2] = CCG_grid_elem_co(gridkey, grid, x + 1, y + 1);
          co[3] = CCG_grid_elem_co(gridkey, grid, x, y + 1);
        }

        if (!ray_face_intersection_depth_quad(ray_start,
                                              isect_precalc,
                                              co[0],
                                              co[1],
                                              co[2],
                                              co[3],
                                              depth,
                                              back_depth,
                                              hit_count)) {
          continue;
        }
        hit = true;

        if (r_face_normal) {
          normal_quad_v3(r_face_normal, co[0], co[1], co[2], co[3]);
        }

        if (r_active_vertex_index) {
          float location[3] = {0.0};
          madd_v3_v3v3fl(location, ray_start, ray_normal, *depth);

          const int x_it[4] = {0, 1, 1, 0};
          const int y_it[4] = {0, 0, 1, 1};

          for (int j = 0; j < 4; j++) {
            /* Always assign nearest_vertex_co in the first iteration to avoid comparison against
             * uninitialized values. This stores the closest vertex in the current intersecting
             * quad. */
            if (j == 0 || len_squared_v3v3(location, co[j]) <
                              len_squared_v3v3(location, nearest_vertex_co)) {
              copy_v3_v3(nearest_vertex_co, co[j]);

              r_active_vertex_index->i = gridkey->grid_area * grid_index +
                                         (y + y_it[j]) * gridkey->grid_size + (x + x_it[j]);
            }
          }
        }
        if (r_active_grid_index) {
          r_active_grid_index->i = grid_index;
        }
      }
    }

    if (origco) {
      origco += gridsize * gridsize;
    }
  }

  return hit;
}

bool BKE_pbvh_node_raycast(PBVH *pbvh,
                           PBVHNode *node,
                           float (*origco)[3],
                           bool use_origco,
                           const float ray_start[3],
                           const float ray_normal[3],
                           struct IsectRayPrecalc *isect_precalc,
                           int *hit_count,
                           float *depth,
                           float *back_depth,
                           SculptVertRef *active_vertex_index,
                           SculptFaceRef *active_face_grid_index,
                           float *face_normal,
                           int stroke_id)
{
  bool hit = false;

  if (node->flag & PBVH_FullyHidden) {
    return false;
  }

  switch (pbvh->type) {
    case PBVH_FACES:
      hit |= pbvh_faces_node_raycast(pbvh,
                                     node,
                                     origco,
                                     ray_start,
                                     ray_normal,
                                     isect_precalc,
                                     hit_count,
                                     depth,
                                     back_depth,
                                     active_vertex_index,
                                     active_face_grid_index,
                                     face_normal,
                                     stroke_id);

      break;
    case PBVH_GRIDS:
      hit |= pbvh_grids_node_raycast(pbvh,
                                     node,
                                     origco,
                                     ray_start,
                                     ray_normal,
                                     isect_precalc,
                                     hit_count,
                                     depth,
                                     back_depth,
                                     active_vertex_index,
                                     active_face_grid_index,
                                     face_normal);
      break;
    case PBVH_BMESH:
      // BM_mesh_elem_index_ensure(pbvh->bm, BM_VERT);

      hit = pbvh_bmesh_node_raycast(pbvh,
                                    node,
                                    ray_start,
                                    ray_normal,
                                    isect_precalc,
                                    hit_count,
                                    depth,
                                    back_depth,
                                    use_origco,
                                    active_vertex_index,
                                    active_face_grid_index,
                                    face_normal,
                                    stroke_id);
      break;
  }

  return hit;
}

void BKE_pbvh_raycast_project_ray_root(
    PBVH *pbvh, bool original, float ray_start[3], float ray_end[3], float ray_normal[3])
{
  if (pbvh->nodes) {
    float rootmin_start, rootmin_end;
    float bb_min_root[3], bb_max_root[3], bb_center[3], bb_diff[3];
    struct IsectRayAABB_Precalc ray;
    float ray_normal_inv[3];
    float offset = 1.0f + 1e-3f;
    const float offset_vec[3] = {1e-3f, 1e-3f, 1e-3f};

    if (original) {
      BKE_pbvh_node_get_original_BB(pbvh->nodes, bb_min_root, bb_max_root);
    }
    else {
      BKE_pbvh_node_get_BB(pbvh->nodes, bb_min_root, bb_max_root);
    }

    /* Slightly offset min and max in case we have a zero width node
     * (due to a plane mesh for instance), or faces very close to the bounding box boundary. */
    mid_v3_v3v3(bb_center, bb_max_root, bb_min_root);
    /* diff should be same for both min/max since it's calculated from center */
    sub_v3_v3v3(bb_diff, bb_max_root, bb_center);
    /* handles case of zero width bb */
    add_v3_v3(bb_diff, offset_vec);
    madd_v3_v3v3fl(bb_max_root, bb_center, bb_diff, offset);
    madd_v3_v3v3fl(bb_min_root, bb_center, bb_diff, -offset);

    /* first project start ray */
    isect_ray_aabb_v3_precalc(&ray, ray_start, ray_normal);
    if (!isect_ray_aabb_v3(&ray, bb_min_root, bb_max_root, &rootmin_start)) {
      return;
    }

    /* then the end ray */
    mul_v3_v3fl(ray_normal_inv, ray_normal, -1.0);
    isect_ray_aabb_v3_precalc(&ray, ray_end, ray_normal_inv);
    /* unlikely to fail exiting if entering succeeded, still keep this here */
    if (!isect_ray_aabb_v3(&ray, bb_min_root, bb_max_root, &rootmin_end)) {
      return;
    }

    madd_v3_v3v3fl(ray_start, ray_start, ray_normal, rootmin_start);
    madd_v3_v3v3fl(ray_end, ray_end, ray_normal_inv, rootmin_end);
  }
}

/* -------------------------------------------------------------------- */

typedef struct {
  struct DistRayAABB_Precalc dist_ray_to_aabb_precalc;
  bool original;
} FindNearestRayData;

static bool nearest_to_ray_aabb_dist_sq(PBVHNode *node, void *data_v)
{
  FindNearestRayData *rcd = data_v;
  const float *bb_min, *bb_max;

  if (rcd->original) {
    /* BKE_pbvh_node_get_original_BB */
    bb_min = node->orig_vb.bmin;
    bb_max = node->orig_vb.bmax;
  }
  else {
    /* BKE_pbvh_node_get_BB */
    bb_min = node->vb.bmin;
    bb_max = node->vb.bmax;
  }

  float co_dummy[3], depth;
  node->tmin = dist_squared_ray_to_aabb_v3(
      &rcd->dist_ray_to_aabb_precalc, bb_min, bb_max, co_dummy, &depth);
  /* Ideally we would skip distances outside the range. */
  return depth > 0.0f;
}

void BKE_pbvh_find_nearest_to_ray(PBVH *pbvh,
                                  BKE_pbvh_SearchNearestCallback cb,
                                  void *data,
                                  const float ray_start[3],
                                  const float ray_normal[3],
                                  bool original)
{
  FindNearestRayData ncd;

  dist_squared_ray_to_aabb_v3_precalc(&ncd.dist_ray_to_aabb_precalc, ray_start, ray_normal);
  ncd.original = original;

  BKE_pbvh_search_callback_occluded(pbvh, nearest_to_ray_aabb_dist_sq, &ncd, cb, data);
}

static bool pbvh_faces_node_nearest_to_ray(PBVH *pbvh,
                                           const PBVHNode *node,
                                           float (*origco)[3],
                                           const float ray_start[3],
                                           const float ray_normal[3],
                                           float *depth,
                                           float *dist_sq)
{
  const MVert *vert = pbvh->verts;
  const MLoop *mloop = pbvh->mloop;
  const int *faces = node->prim_indices;
  int i, totface = node->totprim;
  bool hit = false;

  for (i = 0; i < totface; i++) {
    const MLoopTri *lt = &pbvh->looptri[faces[i]];
    const int *face_verts = node->face_vert_indices[i];

    if (pbvh->respect_hide && paint_is_face_hidden(lt, vert, mloop)) {
      continue;
    }

    if (origco) {
      /* intersect with backuped original coordinates */
      hit |= ray_face_nearest_tri(ray_start,
                                  ray_normal,
                                  origco[face_verts[0]],
                                  origco[face_verts[1]],
                                  origco[face_verts[2]],
                                  depth,
                                  dist_sq);
    }
    else {
      /* intersect with current coordinates */
      hit |= ray_face_nearest_tri(ray_start,
                                  ray_normal,
                                  vert[mloop[lt->tri[0]].v].co,
                                  vert[mloop[lt->tri[1]].v].co,
                                  vert[mloop[lt->tri[2]].v].co,
                                  depth,
                                  dist_sq);
    }
  }

  return hit;
}

static bool pbvh_grids_node_nearest_to_ray(PBVH *pbvh,
                                           PBVHNode *node,
                                           float (*origco)[3],
                                           const float ray_start[3],
                                           const float ray_normal[3],
                                           float *depth,
                                           float *dist_sq)
{
  const int totgrid = node->totprim;
  const int gridsize = pbvh->gridkey.grid_size;
  bool hit = false;

  for (int i = 0; i < totgrid; i++) {
    CCGElem *grid = pbvh->grids[node->prim_indices[i]];
    BLI_bitmap *gh;

    if (!grid) {
      continue;
    }

    gh = pbvh->grid_hidden[node->prim_indices[i]];

    for (int y = 0; y < gridsize - 1; y++) {
      for (int x = 0; x < gridsize - 1; x++) {
        /* check if grid face is hidden */
        if (gh) {
          if (paint_is_grid_face_hidden(gh, gridsize, x, y)) {
            continue;
          }
        }

        if (origco) {
          hit |= ray_face_nearest_quad(ray_start,
                                       ray_normal,
                                       origco[y * gridsize + x],
                                       origco[y * gridsize + x + 1],
                                       origco[(y + 1) * gridsize + x + 1],
                                       origco[(y + 1) * gridsize + x],
                                       depth,
                                       dist_sq);
        }
        else {
          hit |= ray_face_nearest_quad(ray_start,
                                       ray_normal,
                                       CCG_grid_elem_co(&pbvh->gridkey, grid, x, y),
                                       CCG_grid_elem_co(&pbvh->gridkey, grid, x + 1, y),
                                       CCG_grid_elem_co(&pbvh->gridkey, grid, x + 1, y + 1),
                                       CCG_grid_elem_co(&pbvh->gridkey, grid, x, y + 1),
                                       depth,
                                       dist_sq);
        }
      }
    }

    if (origco) {
      origco += gridsize * gridsize;
    }
  }

  return hit;
}

bool BKE_pbvh_node_find_nearest_to_ray(PBVH *pbvh,
                                       PBVHNode *node,
                                       float (*origco)[3],
                                       bool use_origco,
                                       const float ray_start[3],
                                       const float ray_normal[3],
                                       float *depth,
                                       float *dist_sq,
                                       int stroke_id)
{
  bool hit = false;

  if (node->flag & PBVH_FullyHidden) {
    return false;
  }

  switch (pbvh->type) {
    case PBVH_FACES:
      hit |= pbvh_faces_node_nearest_to_ray(
          pbvh, node, origco, ray_start, ray_normal, depth, dist_sq);
      break;
    case PBVH_GRIDS:
      hit |= pbvh_grids_node_nearest_to_ray(
          pbvh, node, origco, ray_start, ray_normal, depth, dist_sq);
      break;
    case PBVH_BMESH:
      hit = pbvh_bmesh_node_nearest_to_ray(
          pbvh, node, ray_start, ray_normal, depth, dist_sq, use_origco, stroke_id);
      break;
  }

  return hit;
}

typedef enum {
  ISECT_INSIDE,
  ISECT_OUTSIDE,
  ISECT_INTERSECT,
} PlaneAABBIsect;

/* Adapted from:
 * http://www.gamedev.net/community/forums/topic.asp?topic_id=512123
 * Returns true if the AABB is at least partially within the frustum
 * (ok, not a real frustum), false otherwise.
 */
static PlaneAABBIsect test_frustum_aabb(const float bb_min[3],
                                        const float bb_max[3],
                                        PBVHFrustumPlanes *frustum)
{
  PlaneAABBIsect ret = ISECT_INSIDE;
  float(*planes)[4] = frustum->planes;

  for (int i = 0; i < frustum->num_planes; i++) {
    float vmin[3], vmax[3];

    for (int axis = 0; axis < 3; axis++) {
      if (planes[i][axis] < 0) {
        vmin[axis] = bb_min[axis];
        vmax[axis] = bb_max[axis];
      }
      else {
        vmin[axis] = bb_max[axis];
        vmax[axis] = bb_min[axis];
      }
    }

    if (dot_v3v3(planes[i], vmin) + planes[i][3] < 0) {
      return ISECT_OUTSIDE;
    }
    if (dot_v3v3(planes[i], vmax) + planes[i][3] <= 0) {
      ret = ISECT_INTERSECT;
    }
  }

  return ret;
}

bool BKE_pbvh_node_frustum_contain_AABB(PBVHNode *node, void *data)
{
  const float *bb_min, *bb_max;
  /* BKE_pbvh_node_get_BB */
  bb_min = node->vb.bmin;
  bb_max = node->vb.bmax;

  return test_frustum_aabb(bb_min, bb_max, data) != ISECT_OUTSIDE;
}

bool BKE_pbvh_node_frustum_exclude_AABB(PBVHNode *node, void *data)
{
  const float *bb_min, *bb_max;
  /* BKE_pbvh_node_get_BB */
  bb_min = node->vb.bmin;
  bb_max = node->vb.bmax;

  return test_frustum_aabb(bb_min, bb_max, data) != ISECT_INSIDE;
}

void BKE_pbvh_update_normals(PBVH *pbvh, struct SubdivCCG *subdiv_ccg)
{
  /* Update normals */
  PBVHNode **nodes;
  int totnode;

  if (pbvh->type == PBVH_BMESH) {
    for (int i = 0; i < pbvh->totnode; i++) {
      if (pbvh->nodes[i].flag & PBVH_Leaf) {
        BKE_pbvh_bmesh_check_tris(pbvh, pbvh->nodes + i);
      }
    }
  }

  BKE_pbvh_search_gather(
      pbvh, update_search_cb, POINTER_FROM_INT(PBVH_UpdateNormals), &nodes, &totnode);

  if (totnode > 0) {
    if (pbvh->type == PBVH_BMESH) {
      pbvh_bmesh_normals_update(pbvh, nodes, totnode);
    }
    else if (pbvh->type == PBVH_FACES) {
      pbvh_faces_update_normals(pbvh, nodes, totnode);
    }
    else if (pbvh->type == PBVH_GRIDS) {
      struct CCGFace **faces;
      int num_faces;
      BKE_pbvh_get_grid_updates(pbvh, true, (void ***)&faces, &num_faces);
      if (num_faces > 0) {
        BKE_subdiv_ccg_update_normals(subdiv_ccg, faces, num_faces);
        MEM_freeN(faces);
      }
    }
  }

  MEM_SAFE_FREE(nodes);
}

void BKE_pbvh_face_sets_color_set(PBVH *pbvh, int seed, int color_default)
{
  pbvh->face_sets_color_seed = seed;
  pbvh->face_sets_color_default = color_default;
}

/**
 * PBVH drawing, updating draw buffers as needed and culling any nodes outside
 * the specified frustum.
 */
typedef struct PBVHDrawSearchData {
  PBVHFrustumPlanes *frustum;
  int accum_update_flag;
} PBVHDrawSearchData;

static bool pbvh_draw_search_cb(PBVHNode *node, void *data_v)
{
  PBVHDrawSearchData *data = data_v;
  if (data->frustum && !BKE_pbvh_node_frustum_contain_AABB(node, data->frustum)) {
    return false;
  }

  data->accum_update_flag |= node->flag;
  return true;
}

void BKE_pbvh_draw_cb(PBVH *pbvh,
                      Mesh *me,
                      bool update_only_visible,
                      PBVHFrustumPlanes *update_frustum,
                      PBVHFrustumPlanes *draw_frustum,
                      void (*draw_fn)(void *user_data, GPU_PBVH_Buffers *buffers),
                      void *user_data)
{
  PBVHNode **nodes;
  int totnode;
  int update_flag = 0;

  pbvh->draw_cache_invalid = false;

  /* Search for nodes that need updates. */
  if (update_only_visible) {
    /* Get visible nodes with draw updates. */
    PBVHDrawSearchData data = {.frustum = update_frustum, .accum_update_flag = 0};
    BKE_pbvh_search_gather(pbvh, pbvh_draw_search_cb, &data, &nodes, &totnode);
    update_flag = data.accum_update_flag;
  }
  else {
    /* Get all nodes with draw updates, also those outside the view. */
    const int search_flag = PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers;
    BKE_pbvh_search_gather(
        pbvh, update_search_cb, POINTER_FROM_INT(search_flag), &nodes, &totnode);
    update_flag = PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers;
  }

  /* Update draw buffers. */
  if (totnode != 0 && (update_flag & (PBVH_RebuildDrawBuffers | PBVH_UpdateDrawBuffers))) {
    // check that need_full_render is set to GPU_pbvh_need_full_render_get(),
    // but only if nodes need updating

    if (pbvh->type != PBVH_GRIDS && pbvh->need_full_render != GPU_pbvh_need_full_render_get()) {
      // update all nodes
      MEM_SAFE_FREE(nodes);

      printf("Rebuilding PBVH draw buffers...\n");

      for (int i = 0; i < pbvh->totnode; i++) {
        PBVHNode *node = pbvh->nodes + i;

        node->flag |= PBVH_UpdateDrawBuffers | PBVH_RebuildDrawBuffers;
      }

      pbvh->need_full_render = GPU_pbvh_need_full_render_get();
      BKE_pbvh_draw_cb(
          pbvh, me, update_only_visible, update_frustum, draw_frustum, draw_fn, user_data);
      return;
    }

    pbvh_update_draw_buffers(pbvh, me, nodes, totnode, update_flag);
  }
  MEM_SAFE_FREE(nodes);

  /* Draw visible nodes. */
  PBVHDrawSearchData draw_data = {.frustum = draw_frustum, .accum_update_flag = 0};
  BKE_pbvh_search_gather(pbvh, pbvh_draw_search_cb, &draw_data, &nodes, &totnode);

  for (int i = 0; i < totnode; i++) {
    PBVHNode *node = nodes[i];
    if (!(node->flag & PBVH_FullyHidden)) {
      if (node->draw_buffers) {
        draw_fn(user_data, node->draw_buffers);
      }

      for (int i = 0; i < node->tot_mat_draw_buffers; i++) {
        draw_fn(user_data, node->mat_draw_buffers[i]);
      }
    }
  }

  MEM_SAFE_FREE(nodes);
}

// bad global from gpu_buffers.c
extern bool pbvh_show_orig_co;

void BKE_pbvh_draw_debug_cb(PBVH *pbvh,
                            void (*draw_fn)(void *user_data,
                                            const float bmin[3],
                                            const float bmax[3],
                                            PBVHNodeFlags flag,
                                            int depth),
                            void *user_data)
{
  for (int a = 0; a < pbvh->totnode; a++) {
    PBVHNode *node = &pbvh->nodes[a];

    int num = a + node->updategen;

    if (pbvh_show_orig_co) {
      draw_fn(&num, node->orig_vb.bmin, node->orig_vb.bmax, node->flag, node->depth);
    }
    else {
      draw_fn(&num, node->vb.bmin, node->vb.bmax, node->flag, node->depth);
    }
  }
}

void BKE_pbvh_grids_update(
    PBVH *pbvh, CCGElem **grids, void **gridfaces, DMFlagMat *flagmats, BLI_bitmap **grid_hidden)
{
  pbvh->grids = grids;
  pbvh->gridfaces = gridfaces;

  if (flagmats != pbvh->grid_flag_mats || pbvh->grid_hidden != grid_hidden) {
    pbvh->grid_flag_mats = flagmats;
    pbvh->grid_hidden = grid_hidden;

    for (int a = 0; a < pbvh->totnode; a++) {
      BKE_pbvh_node_mark_rebuild_draw(&pbvh->nodes[a]);
    }
  }
}

float (*BKE_pbvh_vert_coords_alloc(PBVH *pbvh))[3]
{
  float(*vertCos)[3] = NULL;

  if (pbvh->verts) {
    MVert *mvert = pbvh->verts;

    vertCos = MEM_callocN(3 * pbvh->totvert * sizeof(float), "BKE_pbvh_get_vertCoords");
    float *co = (float *)vertCos;

    for (int a = 0; a < pbvh->totvert; a++, mvert++, co += 3) {
      copy_v3_v3(co, mvert->co);
    }
  }

  return vertCos;
}

void BKE_pbvh_vert_coords_apply(PBVH *pbvh, const float (*vertCos)[3], const int totvert)
{
  if (totvert != pbvh->totvert) {
    BLI_assert_msg(0, "PBVH: Given deforming vcos number does not match PBVH vertex number!");
    return;
  }

  if (!pbvh->deformed) {
    if (pbvh->verts) {
      /* if pbvh is not already deformed, verts/faces points to the */
      /* original data and applying new coords to this arrays would lead to */
      /* unneeded deformation -- duplicate verts/faces to avoid this */

      pbvh->verts = MEM_dupallocN(pbvh->verts);
      /* No need to dupalloc pbvh->looptri, this one is 'totally owned' by pbvh,
       * it's never some mesh data. */

      pbvh->deformed = true;
    }
  }

  if (pbvh->verts) {
    MVert *mvert = pbvh->verts;
    /* copy new verts coords */
    for (int a = 0; a < pbvh->totvert; a++, mvert++) {
      /* no need for float comparison here (memory is exactly equal or not) */
      if (memcmp(mvert->co, vertCos[a], sizeof(float[3])) != 0) {
        copy_v3_v3(mvert->co, vertCos[a]);
        BKE_pbvh_vert_mark_update(pbvh, BKE_pbvh_make_vref(a));
      }
    }

    /* coordinates are new -- normals should also be updated */
    BKE_mesh_calc_normals_looptri(
        pbvh->verts, pbvh->totvert, pbvh->mloop, pbvh->looptri, pbvh->totprim, NULL);

    for (int a = 0; a < pbvh->totnode; a++) {
      BKE_pbvh_node_mark_update(&pbvh->nodes[a]);
    }

    BKE_pbvh_update_bounds(pbvh, PBVH_UpdateBB | PBVH_UpdateOriginalBB);
  }
}

bool BKE_pbvh_is_deformed(PBVH *pbvh)
{
  return pbvh->deformed;
}
/* Proxies */

PBVHProxyNode *BKE_pbvh_node_add_proxy(PBVH *pbvh, PBVHNode *node)
{
  int index, totverts;

  index = node->proxy_count;

  node->proxy_count++;

  if (node->proxies) {
    node->proxies = MEM_reallocN(node->proxies, node->proxy_count * sizeof(PBVHProxyNode));
  }
  else {
    node->proxies = MEM_mallocN(sizeof(PBVHProxyNode), "PBVHNodeProxy");
  }

  BKE_pbvh_node_num_verts(pbvh, node, &totverts, NULL);
  node->proxies[index].co = MEM_callocN(sizeof(float[3]) * totverts, "PBVHNodeProxy.co");

  return node->proxies + index;
}

void BKE_pbvh_node_free_proxies(PBVHNode *node)
{
  for (int p = 0; p < node->proxy_count; p++) {
    MEM_freeN(node->proxies[p].co);
    node->proxies[p].co = NULL;
  }

  MEM_SAFE_FREE(node->proxies);
  node->proxies = NULL;

  node->proxy_count = 0;
}

void BKE_pbvh_gather_proxies(PBVH *pbvh, PBVHNode ***r_array, int *r_tot)
{
  PBVHNode **array = NULL;
  int tot = 0, space = 0;

  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = pbvh->nodes + n;

    if (node->proxy_count > 0) {
      if (tot == space) {
        /* resize array if needed */
        space = (tot == 0) ? 32 : space * 2;
        array = MEM_recallocN_id(array, sizeof(PBVHNode *) * space, __func__);
      }

      array[tot] = node;
      tot++;
    }
  }

  if (tot == 0 && array) {
    MEM_freeN(array);
    array = NULL;
  }

  *r_array = array;
  *r_tot = tot;
}

void pbvh_vertex_iter_init(PBVH *pbvh, PBVHNode *node, PBVHVertexIter *vi, int mode)
{
  struct CCGElem **grids;
  struct MVert *verts;
  const int *vert_indices;
  int *grid_indices;
  int totgrid, gridsize, uniq_verts, totvert;

  vi->grid = NULL;
  vi->no = NULL;
  vi->fno = NULL;
  vi->mvert = NULL;
  vi->vertex.i = 0;
  vi->index = 0;

  vi->respect_hide = pbvh->respect_hide;
  if (pbvh->respect_hide == false) {
    /* The same value for all vertices. */
    vi->visible = true;
  }

  BKE_pbvh_node_get_grids(pbvh, node, &grid_indices, &totgrid, NULL, &gridsize, &grids);
  BKE_pbvh_node_num_verts(pbvh, node, &uniq_verts, &totvert);
  BKE_pbvh_node_get_verts(pbvh, node, &vert_indices, &verts);
  vi->key = pbvh->gridkey;

  vi->grids = grids;
  vi->grid_indices = grid_indices;
  vi->totgrid = (grids) ? totgrid : 1;
  vi->gridsize = gridsize;

  if (mode == PBVH_ITER_ALL) {
    vi->totvert = totvert;
  }
  else {
    vi->totvert = uniq_verts;
  }
  vi->vert_indices = vert_indices;
  vi->mverts = verts;

  if (pbvh->type == PBVH_BMESH) {
    if (mode == PBVH_ITER_ALL) {
      pbvh_bmesh_check_other_verts(node);
    }

    vi->mverts = NULL;

    vi->bi = 0;
    vi->bm_cur_set = node->bm_unique_verts;
    vi->bm_unique_verts = node->bm_unique_verts;
    vi->bm_other_verts = node->bm_other_verts;
    vi->bm_vdata = &pbvh->bm->vdata;
    vi->bm_vert = NULL;

    vi->cd_sculpt_vert = CustomData_get_offset(vi->bm_vdata, CD_DYNTOPO_VERT);
    vi->cd_vert_mask_offset = CustomData_get_offset(vi->bm_vdata, CD_PAINT_MASK);
  }

  vi->gh = NULL;
  if (vi->grids && mode == PBVH_ITER_UNIQUE) {
    vi->grid_hidden = pbvh->grid_hidden;
  }

  vi->mask = NULL;
  if (pbvh->type == PBVH_FACES) {
    vi->vert_normals = pbvh->vert_normals;

    vi->vmask = CustomData_get_layer(pbvh->vdata, CD_PAINT_MASK);
  }
}

bool BKE_pbvh_draw_mask(const PBVH *pbvh)
{
  if (pbvh->flags & PBVH_FAST_DRAW) {
    return false;
  }

  switch (pbvh->type) {
    case PBVH_GRIDS:
      return (pbvh->gridkey.has_mask != 0);
    case PBVH_FACES:
      return (pbvh->vdata && CustomData_get_layer(pbvh->vdata, CD_PAINT_MASK));
    case PBVH_BMESH:
      return (pbvh->bm && (CustomData_get_offset(&pbvh->bm->vdata, CD_PAINT_MASK) != -1));
  }

  return false;
}

SculptVertRef BKE_pbvh_table_index_to_vertex(PBVH *pbvh, int idx)
{
  if (pbvh->type == PBVH_BMESH) {
    SculptVertRef ref = {(intptr_t)pbvh->bm->vtable[idx]};
    return ref;
  }

  return BKE_pbvh_make_vref(idx);
}

SculptEdgeRef BKE_pbvh_table_index_to_edge(PBVH *pbvh, int idx)
{
  if (pbvh->type == PBVH_BMESH) {
    SculptEdgeRef ref = {(intptr_t)pbvh->bm->etable[idx]};
    return ref;
  }

  return BKE_pbvh_make_eref(idx);
}

SculptFaceRef BKE_pbvh_table_index_to_face(PBVH *pbvh, int idx)
{
  if (pbvh->type == PBVH_BMESH) {
    SculptFaceRef ref = {(intptr_t)pbvh->bm->ftable[idx]};
    return ref;
  }

  return BKE_pbvh_make_fref(idx);
}

bool BKE_pbvh_draw_face_sets(PBVH *pbvh)
{
  if (pbvh->flags & PBVH_FAST_DRAW) {
    return false;
  }

  switch (pbvh->type) {
    case PBVH_GRIDS:
      return (pbvh->pdata && CustomData_get_layer(pbvh->pdata, CD_SCULPT_FACE_SETS));
    case PBVH_FACES:
      return (pbvh->pdata && CustomData_get_layer(pbvh->pdata, CD_SCULPT_FACE_SETS));
    case PBVH_BMESH:
      return (pbvh->bm && CustomData_get_layer(&pbvh->bm->pdata, CD_SCULPT_FACE_SETS));
  }

  return false;
}

void pbvh_show_mask_set(PBVH *pbvh, bool show_mask)
{
  pbvh->show_mask = show_mask;
}

void pbvh_show_face_sets_set(PBVH *pbvh, bool show_face_sets)
{
  pbvh->show_face_sets = show_face_sets;
}

void BKE_pbvh_set_frustum_planes(PBVH *pbvh, PBVHFrustumPlanes *planes)
{
  pbvh->num_planes = planes->num_planes;
  for (int i = 0; i < pbvh->num_planes; i++) {
    copy_v4_v4(pbvh->planes[i], planes->planes[i]);
  }
}

void BKE_pbvh_get_frustum_planes(PBVH *pbvh, PBVHFrustumPlanes *planes)
{
  planes->num_planes = pbvh->num_planes;
  for (int i = 0; i < planes->num_planes; i++) {
    copy_v4_v4(planes->planes[i], pbvh->planes[i]);
  }
}

#include "BKE_global.h"
void BKE_pbvh_parallel_range_settings(TaskParallelSettings *settings,
                                      bool use_threading,
                                      int totnode)
{
  memset(settings, 0, sizeof(*settings));
  settings->use_threading = use_threading && totnode > 1 && G.debug_value != 890;
}

MVert *BKE_pbvh_get_verts(const PBVH *pbvh)
{
  BLI_assert(pbvh->type == PBVH_FACES);
  return pbvh->verts;
}

const float (*BKE_pbvh_get_vert_normals(const PBVH *pbvh))[3]
{
  BLI_assert(pbvh->type == PBVH_FACES);
  return pbvh->vert_normals;
}

void BKE_pbvh_subdiv_ccg_set(PBVH *pbvh, SubdivCCG *subdiv_ccg)
{
  pbvh->subdiv_ccg = subdiv_ccg;
  pbvh->gridfaces = (void **)subdiv_ccg->grid_faces;
  pbvh->grid_hidden = subdiv_ccg->grid_hidden;
  pbvh->grid_flag_mats = subdiv_ccg->grid_flag_mats;
  pbvh->grids = subdiv_ccg->grids;
}

void BKE_pbvh_face_sets_set(PBVH *pbvh, int *face_sets)
{
  pbvh->face_sets = face_sets;
}

void BKE_pbvh_respect_hide_set(PBVH *pbvh, bool respect_hide)
{
  pbvh->respect_hide = respect_hide;
}

int BKE_pbvh_get_node_index(PBVH *pbvh, PBVHNode *node)
{
  return (int)(node - pbvh->nodes);
}

int BKE_pbvh_get_totnodes(PBVH *pbvh)
{
  return pbvh->totnode;
}

int BKE_pbvh_get_node_id(PBVH *pbvh, PBVHNode *node)
{
  return node->id;
}

void BKE_pbvh_get_nodes(PBVH *pbvh, int flag, PBVHNode ***r_array, int *r_totnode)
{
  BKE_pbvh_search_gather(pbvh, update_search_cb, POINTER_FROM_INT(flag), r_array, r_totnode);
}

PBVHNode *BKE_pbvh_node_from_index(PBVH *pbvh, int node_i)
{
  return pbvh->nodes + node_i;
}

#ifdef PROXY_ADVANCED
// TODO: if this really works, make sure to pull the neighbor iterator out of sculpt.c and put it
// here
/* clang-format off */
#  include "BKE_context.h"
#  include "DNA_object_types.h"
#  include "DNA_scene_types.h"
#  include "../../editors/sculpt_paint/sculpt_intern.h"
/* clang-format on */

int checkalloc(void **data, int esize, int oldsize, int newsize, int emask, int umask)
{
  // update channel if it already was allocated once, or is requested by umask
  if (newsize != oldsize && (*data || (emask & umask))) {
    if (*data) {
      *data = MEM_reallocN(*data, newsize * esize);
    }
    else {
      *data = MEM_mallocN(newsize * esize, "pbvh proxy vert arrays");
    }
    return emask;
  }

  return 0;
}

void BKE_pbvh_ensure_proxyarray_indexmap(PBVH *pbvh, PBVHNode *node, GHash *vert_node_map)
{
  ProxyVertArray *p = &node->proxyverts;

  int totvert = 0;
  BKE_pbvh_node_num_verts(pbvh, node, &totvert, NULL);

  bool update = !p->indexmap || p->size != totvert;
  update = update || (p->indexmap && BLI_ghash_len(p->indexmap) != totvert);

  if (!update) {
    return;
  }

  if (p->indexmap) {
    BLI_ghash_free(p->indexmap, NULL, NULL);
  }

  GHash *gs = p->indexmap = BLI_ghash_ptr_new("BKE_pbvh_ensure_proxyarray_indexmap");

  PBVHVertexIter vd;

  int i = 0;
  BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_UNIQUE) {
    BLI_ghash_insert(gs, (void *)vd.vertex.i, (void *)i);
    i++;
  }
  BKE_pbvh_vertex_iter_end;
}

bool pbvh_proxyarray_needs_update(PBVH *pbvh, PBVHNode *node, int mask)
{
  ProxyVertArray *p = &node->proxyverts;
  int totvert = 0;

  if (!(node->flag & PBVH_Leaf) || !node->bm_unique_verts) {
    return false;
  }

  BKE_pbvh_node_num_verts(pbvh, node, &totvert, NULL);

  bool bad = p->size != totvert;
  bad = bad || ((mask & PV_NEIGHBORS) && p->neighbors_dirty);
  bad = bad || (p->datamask & mask) != mask;

  bad = bad && totvert > 0;

  return bad;
}

GHash *pbvh_build_vert_node_map(PBVH *pbvh, PBVHNode **nodes, int totnode, int mask)
{
  GHash *vert_node_map = BLI_ghash_ptr_new("BKE_pbvh_ensure_proxyarrays");

  for (int i = 0; i < totnode; i++) {
    PBVHVertexIter vd;
    PBVHNode *node = nodes[i];

    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_UNIQUE) {
      BLI_ghash_insert(vert_node_map, (void *)vd.vertex.i, (void *)(node - pbvh->nodes));
    }
    BKE_pbvh_vertex_iter_end;
  }

  return vert_node_map;
}

void BKE_pbvh_ensure_proxyarrays(
    SculptSession *ss, PBVH *pbvh, PBVHNode **nodes, int totnode, int mask)
{

  bool update = false;

  for (int i = 0; i < totnode; i++) {
    if (pbvh_proxyarray_needs_update(pbvh, nodes[i], mask)) {
      update = true;
      break;
    }
  }

  if (!update) {
    return;
  }

  GHash *vert_node_map = pbvh_build_vert_node_map(pbvh, nodes, totnode, mask);

  for (int i = 0; i < totnode; i++) {
    if (nodes[i]->flag & PBVH_Leaf) {
      BKE_pbvh_ensure_proxyarray_indexmap(pbvh, nodes[i], vert_node_map);
    }
  }

  for (int i = 0; i < totnode; i++) {
    if (nodes[i]->flag & PBVH_Leaf) {
      BKE_pbvh_ensure_proxyarray(ss, pbvh, nodes[i], mask, vert_node_map, false, false);
    }
  }

  if (vert_node_map) {
    BLI_ghash_free(vert_node_map, NULL, NULL);
  }
}

void BKE_pbvh_ensure_proxyarray(SculptSession *ss,
                                PBVH *pbvh,
                                PBVHNode *node,
                                int mask,
                                GHash *vert_node_map,
                                bool check_indexmap,
                                bool force_update)
{
  ProxyVertArray *p = &node->proxyverts;

  if (check_indexmap) {
    BKE_pbvh_ensure_proxyarray_indexmap(pbvh, node, vert_node_map);
  }

  GHash *gs = p->indexmap;

  int totvert = 0;
  BKE_pbvh_node_num_verts(pbvh, node, &totvert, NULL);

  if (!totvert) {
    return;
  }

  int updatemask = 0;

#  define UPDATETEST(name, emask, esize) \
    if (mask & emask) { \
      updatemask |= checkalloc((void **)&p->name, esize, p->size, totvert, emask, mask); \
    }

  UPDATETEST(ownerco, PV_OWNERCO, sizeof(void *))
  UPDATETEST(ownerno, PV_OWNERNO, sizeof(void *))
  UPDATETEST(ownermask, PV_OWNERMASK, sizeof(void *))
  UPDATETEST(ownercolor, PV_OWNERCOLOR, sizeof(void *))
  UPDATETEST(co, PV_CO, sizeof(float) * 3)
  UPDATETEST(no, PV_NO, sizeof(short) * 3)
  UPDATETEST(fno, PV_NO, sizeof(float) * 3)
  UPDATETEST(mask, PV_MASK, sizeof(float))
  UPDATETEST(color, PV_COLOR, sizeof(float) * 4)
  UPDATETEST(index, PV_INDEX, sizeof(SculptVertRef))
  UPDATETEST(neighbors, PV_NEIGHBORS, sizeof(ProxyKey) * MAX_PROXY_NEIGHBORS)

  p->size = totvert;

  if (force_update) {
    updatemask |= mask;
  }

  if ((mask & PV_NEIGHBORS) && p->neighbors_dirty) {
    updatemask |= PV_NEIGHBORS;
  }

  if (!updatemask) {
    return;
  }

  p->datamask |= mask;

  PBVHVertexIter vd;

  int i = 0;

#  if 1
  BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_UNIQUE) {
    void **val;

    if (!BLI_ghash_ensure_p(gs, (void *)vd.vertex.i, &val)) {
      *val = (void *)i;
    };
    i++;
  }
  BKE_pbvh_vertex_iter_end;
#  endif

  if (updatemask & PV_NEIGHBORS) {
    p->neighbors_dirty = false;
  }

  i = 0;
  BKE_pbvh_vertex_iter_begin (pbvh, node, vd, PBVH_ITER_UNIQUE) {
    if (i >= p->size) {
      printf("error!! %s\n", __func__);
      break;
    }

    if (updatemask & PV_OWNERCO) {
      p->ownerco[i] = vd.co;
    }
    if (updatemask & PV_INDEX) {
      p->index[i] = vd.vertex;
    }
    if (updatemask & PV_OWNERNO) {
      p->ownerno[i] = vd.no;
    }
    if (updatemask & PV_NO) {
      if (vd.fno) {
        if (p->fno) {
          copy_v3_v3(p->fno[i], vd.fno);
        }
        normal_float_to_short_v3(p->no[i], vd.fno);
      }
      else if (vd.no) {
        copy_v3_v3_short(p->no[i], vd.no);
        if (p->fno) {
          normal_short_to_float_v3(p->fno[i], vd.no);
        }
      }
      else {
        p->no[i][0] = p->no[i][1] = p->no[i][2] = 0;
        if (p->fno) {
          zero_v3(p->fno[i]);
        }
      }
    }
    if (updatemask & PV_CO) {
      copy_v3_v3(p->co[i], vd.co);
    }
    if (updatemask & PV_OWNERMASK) {
      p->ownermask[i] = vd.mask;
    }
    if (updatemask & PV_MASK) {
      p->mask[i] = vd.mask ? *vd.mask : 0.0f;
    }
    if (updatemask & PV_COLOR) {
      if (vd.vcol) {
        copy_v4_v4(p->color[i], vd.vcol->color);
      }
    }

    if (updatemask & PV_NEIGHBORS) {
      int j = 0;
      SculptVertexNeighborIter ni;

      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.vertex, ni) {
        if (j >= MAX_PROXY_NEIGHBORS - 1) {
          break;
        }

        ProxyKey key;

        int *pindex = (int *)BLI_ghash_lookup_p(gs, (void *)ni.vertex.i);

        if (!pindex) {
          if (vert_node_map) {
            int *nindex = (int *)BLI_ghash_lookup_p(vert_node_map, (void *)ni.vertex.i);

            if (!nindex) {
              p->neighbors_dirty = true;
              continue;
            }

            PBVHNode *node2 = pbvh->nodes + *nindex;
            if (node2->proxyverts.indexmap) {
              pindex = (int *)BLI_ghash_lookup_p(node2->proxyverts.indexmap, (void *)ni.vertex.i);
            }
            else {
              pindex = NULL;
            }

            if (!pindex) {
              p->neighbors_dirty = true;
              continue;
            }

            key.node = (int)(node2 - pbvh->nodes);
            key.pindex = *pindex;
            //*
            if (node2->proxyverts.size != 0 &&
                (key.pindex < 0 || key.pindex >= node2->proxyverts.size)) {
              printf("error! %s\n", __func__);
              fflush(stdout);
              p->neighbors_dirty = true;
              continue;
            }
            //*/
          }
          else {
            p->neighbors_dirty = true;
            continue;
          }
        }
        else {
          key.node = (int)(node - pbvh->nodes);
          key.pindex = *pindex;
        }

        p->neighbors[i][j++] = key;
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

      p->neighbors[i][j].node = -1;
    }

    i++;
  }
  BKE_pbvh_vertex_iter_end;
}

typedef struct GatherProxyThread {
  PBVHNode **nodes;
  PBVH *pbvh;
  int mask;
} GatherProxyThread;

static void pbvh_load_proxyarray_exec(void *__restrict userdata,
                                      const int n,
                                      const TaskParallelTLS *__restrict tls)
{
  GatherProxyThread *data = (GatherProxyThread *)userdata;
  PBVHNode *node = data->nodes[n];
  PBVHVertexIter vd;
  ProxyVertArray *p = &node->proxyverts;
  int i = 0;

  int mask = p->datamask;

  BKE_pbvh_ensure_proxyarray(NULL, data->pbvh, node, data->mask, NULL, false, true);
}

void BKE_pbvh_load_proxyarrays(PBVH *pbvh, PBVHNode **nodes, int totnode, int mask)
{
  GatherProxyThread data = {.nodes = nodes, .pbvh = pbvh, .mask = mask};

  mask = mask & ~PV_NEIGHBORS;  // don't update neighbors in threaded code?

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_load_proxyarray_exec, &settings);
}

static void pbvh_gather_proxyarray_exec(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict tls)
{
  GatherProxyThread *data = (GatherProxyThread *)userdata;
  PBVHNode *node = data->nodes[n];
  PBVHVertexIter vd;
  ProxyVertArray *p = &node->proxyverts;
  int i = 0;

  int mask = p->datamask;

  BKE_pbvh_vertex_iter_begin (data->pbvh, node, vd, PBVH_ITER_UNIQUE) {
    if (mask & PV_CO) {
      copy_v3_v3(vd.co, p->co[i]);
    }

    if (mask & PV_COLOR && vd.col) {
      copy_v4_v4(vd.col, p->color[i]);
    }

    if (vd.mask && (mask & PV_MASK)) {
      *vd.mask = p->mask[i];
    }

    i++;
  }
  BKE_pbvh_vertex_iter_end;
}

void BKE_pbvh_gather_proxyarray(PBVH *pbvh, PBVHNode **nodes, int totnode)
{
  GatherProxyThread data = {.nodes = nodes, .pbvh = pbvh};

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_gather_proxyarray_exec, &settings);
}

void BKE_pbvh_free_proxyarray(PBVH *pbvh, PBVHNode *node)
{
  ProxyVertArray *p = &node->proxyverts;

  if (p->indexmap) {
    BLI_ghash_free(p->indexmap, NULL, NULL);
  }
  if (p->co)
    MEM_freeN(p->co);
  if (p->no)
    MEM_freeN(p->no);
  if (p->index)
    MEM_freeN(p->index);
  if (p->mask)
    MEM_freeN(p->mask);
  if (p->ownerco)
    MEM_freeN(p->ownerco);
  if (p->ownerno)
    MEM_freeN(p->ownerno);
  if (p->ownermask)
    MEM_freeN(p->ownermask);
  if (p->ownercolor)
    MEM_freeN(p->ownercolor);
  if (p->color)
    MEM_freeN(p->color);
  if (p->neighbors)
    MEM_freeN(p->neighbors);

  memset(p, 0, sizeof(*p));
}

void BKE_pbvh_update_proxyvert(PBVH *pbvh, PBVHNode *node, ProxyVertUpdateRec *rec)
{
}

ProxyVertArray *BKE_pbvh_get_proxyarrays(PBVH *pbvh, PBVHNode *node)
{
  return &node->proxyverts;
}

#endif

/* checks if pbvh needs to sync its flat vcol shading flag with scene tool settings
   scene and ob are allowd to be NULL (in which case nothing is done).
*/
void SCULPT_update_flat_vcol_shading(Object *ob, Scene *scene)
{
  if (!scene || !ob || !ob->sculpt || !ob->sculpt->pbvh) {
    return;
  }

  if (ob->sculpt->pbvh) {
    bool flat_vcol_shading = ((scene->toolsettings->sculpt->flags &
                               SCULPT_DYNTOPO_FLAT_VCOL_SHADING) != 0);

    BKE_pbvh_set_flat_vcol_shading(ob->sculpt->pbvh, flat_vcol_shading);
  }
}

PBVHNode *BKE_pbvh_get_node(PBVH *pbvh, int node)
{
  return pbvh->nodes + node;
}

bool BKE_pbvh_node_mark_update_index_buffer(PBVH *pbvh, PBVHNode *node)
{
  bool split_indexed = pbvh->bm && (pbvh->flags & (PBVH_DYNTOPO_SMOOTH_SHADING | PBVH_FAST_DRAW));

  if (split_indexed) {
    BKE_pbvh_node_mark_update_triangulation(node);
  }

  return split_indexed;
}

void BKE_pbvh_node_mark_update_triangulation(PBVHNode *node)
{
  node->flag |= PBVH_UpdateTris;
}

void BKE_pbvh_node_mark_update_tri_area(PBVHNode *node)
{
  node->flag |= PBVH_UpdateTriAreas;
}

/* must be called outside of threads */
void BKE_pbvh_face_areas_begin(PBVH *pbvh)
{
  pbvh->face_area_i ^= 1;
}

void BKE_pbvh_update_all_tri_areas(PBVH *pbvh)
{
  /* swap read/write face area buffers */
  pbvh->face_area_i ^= 1;

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;
    if (node->flag & PBVH_Leaf) {
      node->flag |= PBVH_UpdateTriAreas;
#if 0
      // ensure node triangulations are valid
      // so we don't end up doing it inside brush threads
      BKE_pbvh_bmesh_check_tris(pbvh, node);
#endif
    }
  }
}

void BKE_pbvh_check_tri_areas(PBVH *pbvh, PBVHNode *node)
{
  if (!(node->flag & PBVH_UpdateTriAreas)) {
    return;
  }

  if (pbvh->type == PBVH_BMESH && (!node->tribuf || !node->tribuf->tottri)) {
    return;
  }

  node->flag &= ~PBVH_UpdateTriAreas;

  if (pbvh->type == PBVH_BMESH && (node->flag & PBVH_UpdateTris)) {
    BKE_pbvh_bmesh_check_tris(pbvh, node);
  }

  const int cur_i = pbvh->face_area_i ^ 1;

  switch (BKE_pbvh_type(pbvh)) {
    case PBVH_FACES: {
      for (int i = 0; i < (int)node->totprim; i++) {
        const MLoopTri *lt = &pbvh->looptri[node->prim_indices[i]];

        if (pbvh->face_sets[lt->poly] < 0) {
          /* Skip hidden faces. */
          continue;
        }

        pbvh->face_areas[lt->poly * 2 + cur_i] = 0.0f;
      }

      for (int i = 0; i < (int)node->totprim; i++) {
        const MLoopTri *lt = &pbvh->looptri[node->prim_indices[i]];

        if (pbvh->face_sets[lt->poly] < 0) {
          /* Skip hidden faces. */
          continue;
        }

        const MVert *mv1 = pbvh->verts + pbvh->mloop[lt->tri[0]].v;
        const MVert *mv2 = pbvh->verts + pbvh->mloop[lt->tri[1]].v;
        const MVert *mv3 = pbvh->verts + pbvh->mloop[lt->tri[2]].v;

        float area = area_tri_v3(mv1->co, mv2->co, mv3->co);

        pbvh->face_areas[lt->poly * 2 + cur_i] += area;

        /* sanity check on read side of read write buffer */
        if (pbvh->face_areas[lt->poly * 2 + (cur_i ^ 1)] == 0.0f) {
          pbvh->face_areas[lt->poly * 2 + (cur_i ^ 1)] = pbvh->face_areas[lt->poly * 2 + cur_i];
        }
      }
      break;
    }
    case PBVH_GRIDS:
      break;
    case PBVH_BMESH: {
      BMFace *f;
      const int cd_face_area = pbvh->cd_face_area;

      TGSET_ITER (f, node->bm_faces) {
        float *areabuf = BM_ELEM_CD_GET_VOID_P(f, cd_face_area);
        areabuf[cur_i] = 0.0f;
      }
      TGSET_ITER_END;

      for (int i = 0; i < node->tribuf->tottri; i++) {
        PBVHTri *tri = node->tribuf->tris + i;

        BMVert *v1 = (BMVert *)(node->tribuf->verts[tri->v[0]].i);
        BMVert *v2 = (BMVert *)(node->tribuf->verts[tri->v[1]].i);
        BMVert *v3 = (BMVert *)(node->tribuf->verts[tri->v[2]].i);
        BMFace *f = (BMFace *)tri->f.i;

        float *areabuf = BM_ELEM_CD_GET_VOID_P(f, cd_face_area);

        float area = area_tri_v3(v1->co, v2->co, v3->co);
        float farea = BM_ELEM_CD_GET_FLOAT(f, cd_face_area);

        areabuf[cur_i] = farea + area;
      }
      break;
    }
    default:
      break;
  }
}

static void pbvh_pmap_to_edges_add(PBVH *pbvh,
                                   SculptVertRef vertex,
                                   int **r_edges,
                                   int *r_edges_size,
                                   bool *heap_alloc,
                                   int e,
                                   int p,
                                   int *len,
                                   int **r_polys)
{
  for (int i = 0; i < *len; i++) {
    if ((*r_edges)[i] == e) {
      if ((*r_polys)[i * 2 + 1] == -1) {
        (*r_polys)[i * 2 + 1] = p;
      }
      return;
    }
  }

  if (*len >= *r_edges_size) {
    int newsize = *len + ((*len) >> 1) + 1;

    int *r_edges_new = MEM_malloc_arrayN(newsize, sizeof(*r_edges_new), "r_edges_new");
    int *r_polys_new = MEM_malloc_arrayN(newsize * 2, sizeof(*r_polys_new), "r_polys_new");

    memcpy((void *)r_edges_new, (void *)*r_edges, sizeof(int) * (*r_edges_size));
    memcpy((void *)r_polys_new, (void *)(*r_polys), sizeof(int) * 2 * (*r_edges_size));

    *r_edges_size = newsize;

    if (*heap_alloc) {
      MEM_freeN(*r_polys);
      MEM_freeN(*r_edges);
    }

    *r_edges = r_edges_new;
    *r_polys = r_polys_new;

    *heap_alloc = true;
  }

  (*r_polys)[*len * 2] = p;
  (*r_polys)[*len * 2 + 1] = -1;

  (*r_edges)[*len] = e;
  (*len)++;
}

void BKE_pbvh_pmap_to_edges(PBVH *pbvh,
                            SculptVertRef vertex,
                            int **r_edges,
                            int *r_edges_size,
                            bool *r_heap_alloc,
                            int **r_polys)
{
  MeshElemMap *map = pbvh->pmap->pmap + vertex.i;
  int len = 0;

  for (int i = 0; i < map->count; i++) {
    const MPoly *mp = pbvh->mpoly + map->indices[i];
    const MLoop *ml = pbvh->mloop + mp->loopstart;

    if (pbvh->face_sets[map->indices[i]] < 0) {
      /* Skip connectivity from hidden faces. */
      continue;
    }

    for (int j = 0; j < mp->totloop; j++, ml++) {
      if (ml->v == vertex.i) {
        pbvh_pmap_to_edges_add(pbvh,
                               vertex,
                               r_edges,
                               r_edges_size,
                               r_heap_alloc,
                               ME_POLY_LOOP_PREV(pbvh->mloop, mp, j)->e,
                               map->indices[i],
                               &len,
                               r_polys);
        pbvh_pmap_to_edges_add(pbvh,
                               vertex,
                               r_edges,
                               r_edges_size,
                               r_heap_alloc,
                               ml->e,
                               map->indices[i],
                               &len,
                               r_polys);
      }
    }
  }

  *r_edges_size = len;
}

void BKE_pbvh_set_vemap(PBVH *pbvh, MeshElemMap *vemap)
{
  pbvh->vemap = vemap;
}

void BKE_pbvh_get_vert_face_areas(PBVH *pbvh, SculptVertRef vertex, float *r_areas, int valence)
{
  const int cur_i = pbvh->face_area_i;

  switch (BKE_pbvh_type(pbvh)) {
    case PBVH_FACES: {
      int *edges = BLI_array_alloca(edges, 16);
      int *polys = BLI_array_alloca(polys, 32);
      bool heap_alloc = false;
      int len = 16;

      BKE_pbvh_pmap_to_edges(pbvh, vertex, &edges, &len, &heap_alloc, &polys);
      len = MIN2(len, valence);

      if (pbvh->vemap) {
        /* sort poly references by vemap edge ordering */
        MeshElemMap *emap = pbvh->vemap + vertex.i;

        int *polys_old = BLI_array_alloca(polys, len * 2);
        memcpy((void *)polys_old, (void *)polys, sizeof(int) * len * 2);

        /* note that wire edges will break this, but
           should only result in incorrect weights
           and isn't worth fixing */

        for (int i = 0; i < len; i++) {
          for (int j = 0; j < len; j++) {
            if (emap->indices[i] == edges[j]) {
              polys[i * 2] = polys_old[j * 2];
              polys[i * 2 + 1] = polys_old[j * 2 + 1];
            }
          }
        }
      }
      for (int i = 0; i < len; i++) {
        r_areas[i] = pbvh->face_areas[polys[i * 2] * 2 + cur_i];

        if (polys[i * 2 + 1] != -1) {
          r_areas[i] += pbvh->face_areas[polys[i * 2 + 1] * 2 + cur_i];
          r_areas[i] *= 0.5f;
        }
      }

      if (heap_alloc) {
        MEM_freeN(edges);
        MEM_freeN(polys);
      }

      break;
    }
    case PBVH_BMESH: {
      BMVert *v = (BMVert *)vertex.i;
      BMEdge *e = v->e;

      if (!e) {
        for (int i = 0; i < valence; i++) {
          r_areas[i] = 1.0f;
        }

        return;
      }

      const int cd_face_area = pbvh->cd_face_area;
      int j = 0;

      do {
        float w = 0.0f;

        if (!e->l) {
          w = 0.0f;
        }
        else {
          float *a1 = BM_ELEM_CD_GET_VOID_P(e->l->f, cd_face_area);
          float *a2 = BM_ELEM_CD_GET_VOID_P(e->l->radial_next->f, cd_face_area);

          w += a1[cur_i] * 0.5f;
          w += a2[cur_i] * 0.5f;
        }

        if (j >= valence) {
          printf("%s: error, corrupt edge cycle\n", __func__);
          break;
        }

        r_areas[j++] = w;

        e = v == e->v1 ? e->v1_disk_link.next : e->v2_disk_link.next;
      } while (e != v->e);

      for (; j < valence; j++) {
        r_areas[j] = 1.0f;
      }

      break;
    }

    case PBVH_GRIDS: { /* estimate from edge lengths */
      int index = (int)vertex.i;

      const CCGKey *key = BKE_pbvh_get_grid_key(pbvh);
      const int grid_index = index / key->grid_area;
      const int vertex_index = index - grid_index * key->grid_area;

      SubdivCCGCoord coord = {.grid_index = grid_index,
                              .x = vertex_index % key->grid_size,
                              .y = vertex_index / key->grid_size};

      SubdivCCGNeighbors neighbors;
      BKE_subdiv_ccg_neighbor_coords_get(pbvh->subdiv_ccg, &coord, false, &neighbors);

      float *co1 = CCG_elem_co(key, CCG_elem_offset(key, pbvh->grids[grid_index], vertex_index));
      float totw = 0.0f;
      int i = 0;

      for (i = 0; i < neighbors.size; i++) {
        SubdivCCGCoord *coord2 = neighbors.coords + i;

        int vertex_index2 = coord2->y * key->grid_size + coord2->x;

        float *co2 = CCG_elem_co(
            key, CCG_elem_offset(key, pbvh->grids[coord2->grid_index], vertex_index2));
        float w = len_v3v3(co1, co2);

        r_areas[i] = w;
        totw += w;
      }

      if (neighbors.size != valence) {
        printf("%s: error!\n", __func__);
      }
      if (totw < 0.000001f) {
        for (int i = 0; i < neighbors.size; i++) {
          r_areas[i] = 1.0f;
        }
      }

      for (; i < valence; i++) {
        r_areas[i] = 1.0f;
      }

      break;
    }
  }
}

void BKE_pbvh_set_stroke_id(PBVH *pbvh, int stroke_id)
{
  pbvh->stroke_id = stroke_id;
}

static void pbvh_boundaries_flag_update(PBVH *pbvh)
{

  if (pbvh->bm) {
    BMVert *v;
    BMIter iter;

    BM_ITER_MESH (v, &iter, pbvh->bm, BM_VERTS_OF_MESH) {
      MSculptVert *mv = BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_sculpt_vert);

      mv->flag |= SCULPTVERT_NEED_BOUNDARY;
    }
  }
  else {
    int totvert = pbvh->totvert;

    if (BKE_pbvh_type(pbvh) == PBVH_GRIDS) {
      totvert = BKE_pbvh_get_grid_num_vertices(pbvh);
    }

    for (int i = 0; i < totvert; i++) {
      pbvh->mdyntopo_verts[i].flag |= SCULPTVERT_NEED_BOUNDARY;
    }
  }
}

void BKE_pbvh_set_symmetry(PBVH *pbvh, int symmetry, int boundary_symmetry)
{
  if (symmetry == pbvh->symmetry && boundary_symmetry == pbvh->boundary_symmetry) {
    return;
  }

  pbvh->symmetry = symmetry;
  pbvh->boundary_symmetry = boundary_symmetry;

  pbvh_boundaries_flag_update(pbvh);
}

void BKE_pbvh_set_mdyntopo_verts(PBVH *pbvh, struct MSculptVert *mdyntopoverts)
{
  pbvh->mdyntopo_verts = mdyntopoverts;
}

void BKE_pbvh_update_vert_boundary_grids(PBVH *pbvh,
                                         struct SubdivCCG *subdiv_ccg,
                                         SculptVertRef vertex)
{
  MSculptVert *mv = pbvh->mdyntopo_verts + vertex.i;

  mv->flag &= ~(SCULPTVERT_BOUNDARY | SCULPTVERT_FSET_BOUNDARY | SCULPTVERT_NEED_BOUNDARY |
                SCULPTVERT_FSET_CORNER | SCULPTVERT_CORNER | SCULPTVERT_SEAM_BOUNDARY |
                SCULPTVERT_SHARP_BOUNDARY | SCULPTVERT_SEAM_CORNER | SCULPTVERT_SHARP_CORNER);

  int index = (int)vertex.i;

  /* TODO: optimize this. We could fill #SculptVertexNeighborIter directly,
   * maybe provide coordinate and mask pointers directly rather than converting
   * back and forth between #CCGElem and global index. */
  const CCGKey *key = BKE_pbvh_get_grid_key(pbvh);
  const int grid_index = index / key->grid_area;
  const int vertex_index = index - grid_index * key->grid_area;

  SubdivCCGCoord coord = {.grid_index = grid_index,
                          .x = vertex_index % key->grid_size,
                          .y = vertex_index / key->grid_size};

  SubdivCCGNeighbors neighbors;
  BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, &coord, false, &neighbors);

  mv->valence = neighbors.size;
  mv->flag &= ~SCULPTVERT_NEED_VALENCE;
}

void BKE_pbvh_update_vert_boundary_faces(int *face_sets,
                                         MVert *mvert,
                                         MEdge *medge,
                                         MLoop *mloop,
                                         MPoly *mpoly,
                                         MSculptVert *mdyntopo_verts,
                                         MeshElemMap *pmap,
                                         SculptVertRef vertex)
{
  MSculptVert *mv = mdyntopo_verts + vertex.i;
  MeshElemMap *vert_map = &pmap[vertex.i];

  int last_fset = -1;
  int last_fset2 = -1;

  mv->flag &= ~(SCULPTVERT_BOUNDARY | SCULPTVERT_FSET_BOUNDARY | SCULPTVERT_NEED_BOUNDARY |
                SCULPTVERT_FSET_CORNER | SCULPTVERT_CORNER | SCULPTVERT_SEAM_BOUNDARY |
                SCULPTVERT_SHARP_BOUNDARY | SCULPTVERT_SEAM_CORNER | SCULPTVERT_SHARP_CORNER);

  int totsharp = 0, totseam = 0;
  int visible = false;

  for (int i = 0; i < vert_map->count; i++) {
    int f_i = vert_map->indices[i];

    MPoly *mp = mpoly + f_i;
    MLoop *ml = mloop + mp->loopstart;
    int j = 0;

    for (j = 0; j < mp->totloop; j++, ml++) {
      if (ml->v == (int)vertex.i) {
        break;
      }
    }

    if (j < mp->totloop) {
      MEdge *me = medge + ml->e;
      if (me->flag & ME_SHARP) {
        mv->flag |= SCULPTVERT_SHARP_BOUNDARY;
        totsharp++;
      }

      if (me->flag & ME_SEAM) {
        mv->flag |= SCULPTVERT_SEAM_BOUNDARY;
        totseam++;
      }
    }

    int fset = face_sets ? abs(face_sets[f_i]) : -1;

    if (fset > 0) {
      visible = true;
    }
    else {
      fset = -fset;
    }

    if (i > 0 && fset != last_fset) {
      mv->flag |= SCULPTVERT_FSET_BOUNDARY;

      if (i > 1 && last_fset2 != last_fset && last_fset != -1 && last_fset2 != -1 && fset != -1 &&
          last_fset2 != fset) {
        mv->flag |= SCULPTVERT_FSET_CORNER;
      }
    }

    if (i > 0 && last_fset != fset) {
      last_fset2 = last_fset;
    }

    last_fset = fset;
  }

  if (!visible) {
    mv->flag |= SCULPTVERT_VERT_FSET_HIDDEN;
  }

  if (totsharp > 2) {
    mv->flag |= SCULPTVERT_SHARP_CORNER;
  }

  if (totseam > 2) {
    mv->flag |= SCULPTVERT_SEAM_CORNER;
  }
}

void BKE_pbvh_ignore_uvs_set(PBVH *pbvh, bool value)
{
  if (!!(pbvh->flags & PBVH_IGNORE_UVS) == value) {
    return;  // no change
  }

  if (value) {
    pbvh->flags |= PBVH_IGNORE_UVS;
  }
  else {
    pbvh->flags &= ~PBVH_IGNORE_UVS;
  }

  pbvh_boundaries_flag_update(pbvh);
}

bool BKE_pbvh_cache(const struct Mesh *me, PBVH *pbvh)
{
  memset(&pbvh->cached_data, 0, sizeof(pbvh->cached_data));

  if (pbvh->invalid) {
    printf("invalid pbvh!\n");
    return false;
  }

  switch (pbvh->type) {
    case PBVH_BMESH:
      if (!pbvh->bm) {
        return false;
      }

      pbvh->cached_data.bm = pbvh->bm;

      pbvh->cached_data.vdata = pbvh->bm->vdata;
      pbvh->cached_data.edata = pbvh->bm->edata;
      pbvh->cached_data.ldata = pbvh->bm->ldata;
      pbvh->cached_data.pdata = pbvh->bm->pdata;

      pbvh->cached_data.totvert = pbvh->bm->totvert;
      pbvh->cached_data.totedge = pbvh->bm->totedge;
      pbvh->cached_data.totloop = pbvh->bm->totloop;
      pbvh->cached_data.totpoly = pbvh->bm->totface;
      break;
    case PBVH_GRIDS:
      pbvh->cached_data.vdata = me->vdata;
      pbvh->cached_data.edata = me->edata;
      pbvh->cached_data.ldata = me->ldata;
      pbvh->cached_data.pdata = me->pdata;

      int grid_side = pbvh->gridkey.grid_size;

      pbvh->cached_data.totvert = pbvh->totgrid * grid_side * grid_side;
      pbvh->cached_data.totedge = me->totedge;
      pbvh->cached_data.totloop = me->totloop;
      pbvh->cached_data.totpoly = pbvh->totgrid * (grid_side - 1) * (grid_side - 1);
      break;
    case PBVH_FACES:
      pbvh->cached_data.vdata = me->vdata;
      pbvh->cached_data.edata = me->edata;
      pbvh->cached_data.ldata = me->ldata;
      pbvh->cached_data.pdata = me->pdata;

      pbvh->cached_data.totvert = me->totvert;
      pbvh->cached_data.totedge = me->totedge;
      pbvh->cached_data.totloop = me->totloop;
      pbvh->cached_data.totpoly = me->totpoly;
      break;
  }

  return true;
}

static bool customdata_is_same(const CustomData *a, const CustomData *b)
{
  return memcmp(a, b, sizeof(CustomData)) == 0;
}

bool BKE_pbvh_cache_is_valid(const struct Object *ob,
                             const struct Mesh *me,
                             const PBVH *pbvh,
                             int pbvh_type)
{
  if (pbvh->invalid) {
    printf("pbvh invalid!\n");
  }

  if (pbvh->type != pbvh_type) {
    return false;
  }

  bool ok = true;
  int totvert = 0, totedge = 0, totloop = 0, totpoly = 0;
  const CustomData *vdata, *edata, *ldata, *pdata;

  switch (pbvh_type) {
    case PBVH_BMESH:
      if (!pbvh->bm || pbvh->bm != pbvh->cached_data.bm) {
        return false;
      }

      totvert = pbvh->bm->totvert;
      totedge = pbvh->bm->totedge;
      totloop = pbvh->bm->totloop;
      totpoly = pbvh->bm->totface;

      vdata = &pbvh->bm->vdata;
      edata = &pbvh->bm->edata;
      ldata = &pbvh->bm->ldata;
      pdata = &pbvh->bm->pdata;
      break;
    case PBVH_FACES:
      totvert = me->totvert;
      totedge = me->totedge;
      totloop = me->totloop;
      totpoly = me->totpoly;

      vdata = &me->vdata;
      edata = &me->edata;
      ldata = &me->ldata;
      pdata = &me->pdata;
      break;
    case PBVH_GRIDS: {
      MultiresModifierData *mmd = NULL;

      LISTBASE_FOREACH (ModifierData *, md, &ob->modifiers) {
        if (md->type == eModifierType_Multires) {
          mmd = (MultiresModifierData *)md;
          break;
        }
      }

      if (!mmd) {
        return false;
      }

      int grid_side = 1 + (1 << (mmd->sculptlvl - 1));

      totvert = me->totloop * grid_side * grid_side;
      totedge = me->totedge;
      totloop = me->totloop;
      totpoly = me->totloop * (grid_side - 1) * (grid_side - 1);

      vdata = &me->vdata;
      edata = &me->edata;
      ldata = &me->ldata;
      pdata = &me->pdata;
      break;
    }
  }

  ok = ok && totvert == pbvh->cached_data.totvert;
  ok = ok && totedge == pbvh->cached_data.totedge;
  ok = ok && totloop == pbvh->cached_data.totloop;
  ok = ok && totpoly == pbvh->cached_data.totpoly;

  ok = ok && customdata_is_same(vdata, &pbvh->cached_data.vdata);
  ok = ok && customdata_is_same(edata, &pbvh->cached_data.edata);
  ok = ok && customdata_is_same(ldata, &pbvh->cached_data.ldata);
  ok = ok && customdata_is_same(pdata, &pbvh->cached_data.pdata);

  return ok;
}

GHash *cached_pbvhs = NULL;
static void pbvh_clear_cached_pbvhs(PBVH *exclude)
{
  PBVH **pbvhs = NULL;
  BLI_array_staticdeclare(pbvhs, 8);

  GHashIterator iter;
  GHASH_ITER (iter, cached_pbvhs) {
    PBVH *pbvh = BLI_ghashIterator_getValue(&iter);

    if (pbvh != exclude) {
      BLI_array_append(pbvhs, pbvh);
    }
  }

  for (int i = 0; i < BLI_array_len(pbvhs); i++) {
    PBVH *pbvh = pbvhs[i];

    if (pbvh->bm) {
      BM_mesh_free(pbvh->bm);
    }

    BKE_pbvh_free(pbvh);
  }

  BLI_array_free(pbvhs);
  BLI_ghash_clear(cached_pbvhs, MEM_freeN, NULL);
}

void BKE_pbvh_clear_cache(PBVH *preserve)
{
  pbvh_clear_cached_pbvhs(NULL);
}

#define PBVH_CACHE_KEY_SIZE 1024

static void pbvh_make_cached_key(Object *ob, char out[PBVH_CACHE_KEY_SIZE])
{
  sprintf(out, "%s:%p", ob->id.name, G.main);
}

void BKE_pbvh_invalidate_cache(Object *ob)
{
  Object *ob_orig = DEG_get_original_object(ob);

  char key[PBVH_CACHE_KEY_SIZE];
  pbvh_make_cached_key(ob_orig, key);

  PBVH *pbvh = BLI_ghash_lookup(cached_pbvhs, key);

  if (pbvh) {
    BKE_pbvh_cache_remove(pbvh);
  }
}

PBVH *BKE_pbvh_get_or_free_cached(Object *ob, Mesh *me, PBVHType pbvh_type)
{
  Object *ob_orig = DEG_get_original_object(ob);

  char key[PBVH_CACHE_KEY_SIZE];
  pbvh_make_cached_key(ob_orig, key);

  PBVH *pbvh = BLI_ghash_lookup(cached_pbvhs, key);

  if (!pbvh) {
    return NULL;
  }

  if (BKE_pbvh_cache_is_valid(ob, me, pbvh, pbvh_type)) {
    switch (pbvh_type) {
      case PBVH_BMESH:
        break;
      case PBVH_FACES:
        pbvh->vert_normals = BKE_mesh_vertex_normals_for_write(me);
      case PBVH_GRIDS:
        if (!pbvh->deformed) {
          pbvh->verts = me->mvert;
        }

        pbvh->mloop = me->mloop;
        pbvh->mpoly = me->mpoly;
        pbvh->vdata = &me->vdata;
        pbvh->ldata = &me->ldata;
        pbvh->pdata = &me->pdata;

        pbvh->face_sets = CustomData_get_layer(&me->pdata, CD_SCULPT_FACE_SETS);

        break;
    }

    BKE_pbvh_update_active_vcol(pbvh, me);

    return pbvh;
  }

  pbvh_clear_cached_pbvhs(NULL);
  return NULL;
}

void BKE_pbvh_set_cached(Object *ob, PBVH *pbvh)
{
  if (!pbvh) {
    return;
  }

  Object *ob_orig = DEG_get_original_object(ob);

  char key[PBVH_CACHE_KEY_SIZE];
  pbvh_make_cached_key(ob_orig, key);

  PBVH *exist = BLI_ghash_lookup(cached_pbvhs, key);

  if (pbvh->invalid) {
    printf("pbvh invalid!");
  }

  if (exist && exist->invalid) {
    printf("pbvh invalid!");
  }

  if (!exist || exist != pbvh) {
    pbvh_clear_cached_pbvhs(pbvh);

    char key[PBVH_CACHE_KEY_SIZE];
    pbvh_make_cached_key(ob_orig, key);

    BLI_ghash_insert(cached_pbvhs, BLI_strdup(key), pbvh);
  }

  BKE_pbvh_cache(BKE_object_get_original_mesh(ob_orig), pbvh);
}

struct SculptPMap *BKE_pbvh_get_pmap(PBVH *pbvh)
{
  return pbvh->pmap;
}

void BKE_pbvh_set_pmap(PBVH *pbvh, SculptPMap *pmap)
{
  if (pbvh->pmap != pmap) {
    BKE_pbvh_pmap_aquire(pmap);
  }

  pbvh->pmap = pmap;
}

/** Does not free pbvh itself. */
void BKE_pbvh_cache_remove(PBVH *pbvh)
{
  char **keys = NULL;
  BLI_array_staticdeclare(keys, 32);

  GHashIterator iter;
  GHASH_ITER (iter, cached_pbvhs) {
    PBVH *pbvh2 = BLI_ghashIterator_getValue(&iter);

    if (pbvh2 == pbvh) {
      BLI_array_append(keys, (char *)BLI_ghashIterator_getKey(&iter));
      break;
    }
  }

  for (int i = 0; i < BLI_array_len(keys); i++) {
    BLI_ghash_remove(cached_pbvhs, keys[i], MEM_freeN, NULL);
  }

  BLI_array_free(keys);
}

void BKE_pbvh_set_bmesh(PBVH *pbvh, BMesh *bm)
{
  pbvh->bm = bm;
}

void BKE_pbvh_free_bmesh(PBVH *pbvh, BMesh *bm)
{
  if (pbvh) {
    pbvh->bm = NULL;
  }

  BM_mesh_free(bm);

  GHashIterator iter;
  char **keys = NULL;
  BLI_array_staticdeclare(keys, 32);

  PBVH **pbvhs = NULL;
  BLI_array_staticdeclare(pbvhs, 8);

  GHASH_ITER (iter, cached_pbvhs) {
    PBVH *pbvh2 = BLI_ghashIterator_getValue(&iter);

    if (pbvh2->bm == bm) {
      pbvh2->bm = NULL;

      if (pbvh2 != pbvh) {
        bool ok = true;

        for (int i = 0; i < BLI_array_len(pbvhs); i++) {
          if (pbvhs[i] == pbvh2) {
            ok = false;
          }
        }

        if (ok) {
          BLI_array_append(pbvhs, pbvh2);
        }
      }

      BLI_array_append(keys, BLI_ghashIterator_getKey(&iter));
    }
  }

  for (int i = 0; i < BLI_array_len(keys); i++) {
    BLI_ghash_remove(cached_pbvhs, keys[i], MEM_freeN, NULL);
  }

  for (int i = 0; i < BLI_array_len(pbvhs); i++) {
    BKE_pbvh_free(pbvhs[i]);
  }

  BLI_array_free(pbvhs);
  BLI_array_free(keys);
}

struct BMLog *BKE_pbvh_get_bm_log(PBVH *pbvh)
{
  return pbvh->bm_log;
}

void BKE_pbvh_system_init()
{
  cached_pbvhs = BLI_ghash_str_new("pbvh cache ghash");
}

void BKE_pbvh_system_exit()
{
  pbvh_clear_cached_pbvhs(NULL);
  BLI_ghash_free(cached_pbvhs, NULL, NULL);
}

SculptPMap *BKE_pbvh_make_pmap(const struct Mesh *me)
{
  SculptPMap *pmap = MEM_callocN(sizeof(*pmap), "SculptPMap");

  BKE_mesh_vert_poly_map_create(&pmap->pmap,
                                &pmap->pmap_mem,
                                me->mvert,
                                me->medge,
                                me->mpoly,
                                me->mloop,
                                me->totvert,
                                me->totpoly,
                                me->totloop,
                                false);

  pmap->refcount = 1;

  return pmap;
}

void BKE_pbvh_pmap_aquire(SculptPMap *pmap)
{
  pmap->refcount++;
}

bool BKE_pbvh_pmap_release(SculptPMap *pmap)
{
  if (!pmap) {
    return false;
  }

  pmap->refcount--;

  if (pmap->refcount == 0) {
    MEM_SAFE_FREE(pmap->pmap);
    MEM_SAFE_FREE(pmap->pmap_mem);
    MEM_SAFE_FREE(pmap);

    return true;
  }

  return false;
}

bool BKE_pbvh_is_drawing(const PBVH *pbvh)
{
  return pbvh->is_drawing;
}

void BKE_pbvh_is_drawing_set(PBVH *pbvh, bool val)
{
  pbvh->is_drawing = val;
}

void BKE_pbvh_node_num_loops(PBVH *pbvh, PBVHNode *node, int *r_totloop)
{
  UNUSED_VARS(pbvh);
  BLI_assert(BKE_pbvh_type(pbvh) == PBVH_FACES);

  if (r_totloop) {
    *r_totloop = node->loop_indices_num;
  }
}

void BKE_pbvh_update_active_vcol(PBVH *pbvh, const Mesh *mesh)
{
  CustomDataLayer *last_layer = pbvh->color_layer;

  Mesh me_query;
  const CustomData *vdata, *ldata;

  if (pbvh->type == PBVH_BMESH && pbvh->bm) {
    vdata = &pbvh->bm->vdata;
    ldata = &pbvh->bm->ldata;
  }
  else {
    vdata = &mesh->vdata;
    ldata = &mesh->ldata;
  }

  BKE_id_attribute_copy_domains_temp(ID_ME, vdata, NULL, ldata, NULL, NULL, &me_query.id);
  BKE_pbvh_get_color_layer(&me_query, &pbvh->color_layer, &pbvh->color_domain);

  if (pbvh->color_layer) {
    pbvh->color_type = pbvh->color_layer->type;

    if (pbvh->bm) {
      pbvh->cd_vcol_offset = pbvh->color_layer->offset;
    }
  }
  else {
    pbvh->cd_vcol_offset = -1;
  }

  if (pbvh->color_layer != last_layer) {
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if (node->flag & PBVH_Leaf) {
        BKE_pbvh_node_mark_update_color(node);
      }
    }
  }
}

void BKE_pbvh_pmap_set(PBVH *pbvh, SculptPMap *pmap)
{
  pbvh->pmap = pmap;
}

void BKE_pbvh_ensure_node_loops(PBVH *pbvh)
{
  BLI_assert(BKE_pbvh_type(pbvh) == PBVH_FACES);

  int totloop = 0;

  /* Check if nodes already have loop indices. */
  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    if (node->loop_indices) {
      return;
    }

    totloop += node->totprim * 3;
  }

  BLI_bitmap *visit = BLI_BITMAP_NEW(totloop, __func__);

  /* Create loop indices from node loop triangles. */
  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    node->loop_indices = MEM_malloc_arrayN(node->totprim * 3, sizeof(int), __func__);
    node->loop_indices_num = 0;

    for (int j = 0; j < (int)node->totprim; j++) {
      const MLoopTri *mlt = pbvh->looptri + node->prim_indices[j];

      for (int k = 0; k < 3; k++) {
        if (!BLI_BITMAP_TEST(visit, mlt->tri[k])) {
          node->loop_indices[node->loop_indices_num++] = mlt->tri[k];
          BLI_BITMAP_ENABLE(visit, mlt->tri[k]);
        }
      }
    }
  }

  MEM_SAFE_FREE(visit);
}

bool BKE_pbvh_get_origvert(
    PBVH *pbvh, SculptVertRef vertex, const float **r_co, float **r_no, float **r_color)
{
  MSculptVert *mv;

  switch (pbvh->type) {
    case PBVH_FACES:
    case PBVH_GRIDS:
      mv = pbvh->mdyntopo_verts + vertex.i;

      if (mv->stroke_id != pbvh->stroke_id) {
        mv->stroke_id = pbvh->stroke_id;
        float *mask = NULL;

        if (pbvh->type == PBVH_FACES) {
          copy_v3_v3(mv->origco, pbvh->verts[vertex.i].co);
          copy_v3_v3(mv->origno, pbvh->vert_normals[vertex.i]);
          mask = (float *)CustomData_get_layer(pbvh->vdata, CD_PAINT_MASK);

          if (mask) {
            mask += vertex.i;
          }
        }
        else {
          const CCGKey *key = BKE_pbvh_get_grid_key(pbvh);
          const int grid_index = vertex.i / key->grid_area;
          const int vertex_index = vertex.i - grid_index * key->grid_area;
          CCGElem *elem = BKE_pbvh_get_grids(pbvh)[grid_index];

          copy_v3_v3(mv->origco, CCG_elem_co(key, CCG_elem_offset(key, elem, vertex_index)));
          copy_v3_v3(mv->origno, CCG_elem_no(key, CCG_elem_offset(key, elem, vertex_index)));
          mask = CCG_elem_mask(key, CCG_elem_offset(key, elem, vertex_index));
        }

        if (mask) {
          mv->origmask = (ushort)(*mask * 65535.0f);
        }

        if (pbvh->color_layer) {
          BKE_pbvh_vertex_color_get(pbvh, vertex, mv->origcolor);
        }
      }
      break;
    case PBVH_BMESH: {
      BMVert *v = (BMVert *)vertex.i;
      mv = BKE_PBVH_SCULPTVERT(pbvh->cd_sculpt_vert, v);

      if (mv->stroke_id != pbvh->stroke_id) {
        mv->stroke_id = pbvh->stroke_id;

        copy_v3_v3(mv->origco, v->co);
        copy_v3_v3(mv->origno, v->no);

        if (pbvh->cd_vert_mask_offset != -1) {
          mv->origmask = (short)(BM_ELEM_CD_GET_FLOAT(v, pbvh->cd_vert_mask_offset) * 65535.0f);
        }

        if (pbvh->cd_vcol_offset != -1) {
          BKE_pbvh_vertex_color_get(pbvh, vertex, mv->origcolor);
        }
      }
      break;
    }
  }

  if (r_co) {
    *r_co = mv->origco;
  }

  if (r_no) {
    *r_no = mv->origno;
  }

  if (r_color) {
    *r_color = mv->origcolor;
  }

  return true;
}

bool BKE_pbvh_draw_cache_invalid(const PBVH *pbvh)
{
  return pbvh->draw_cache_invalid;
}
