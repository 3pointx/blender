#pragma once

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 * \brief New brush engine for sculpt
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "RNA_types.h"

/*
The new brush engine is based on command lists.  These lists
will eventually be created by a node editor.

Key is the concept of BrushChannels.  A brush channel is
a logical parameter with a type, input settings (e.g. pen),
a falloff curve, etc.

Brush channels have a concept of inheritance.  There is a
BrushChannelSet (collection of channels) in Sculpt,
in Brush, and in BrushCommand.  Inheritence behavior
is controller via BrushChannel->flag.

This should completely replace UnifiedPaintSettings.
*/

#include "DNA_sculpt_brush_types.h"
#include "DNA_texture_types.h"

struct BrushChannel;
struct BlendWriter;
struct StructRNA;
struct BlendDataReader;
struct BlendLibReader;
struct ID;
struct BlendExpander;
struct Brush;
struct Sculpt;
struct LibraryForeachIDData;
struct UnifiedPaintSettings;

#define MAKE_BUILTIN_CH_NAME(idname) BRUSH_BUILTIN_##idname

/* these macros validate channel names at compile time */
#define BRUSHSET_LOOKUP_FINAL(childset, parentset, idname) \
  BKE_brush_channelset_lookup_final(childset, parentset, MAKE_BUILTIN_CH_NAME(idname))
#define BRUSHSET_LOOKUP(chset, channel) \
  BKE_brush_channelset_lookup(chset, MAKE_BUILTIN_CH_NAME(channel))
#define BRUSHSET_HAS(chset, channel, mapdata) \
  BKE_brush_channelset_lookup(chset, MAKE_BUILTIN_CH_NAME(channel))
#define BRUSHSET_GET_FLOAT(chset, channel, mapdata) \
  BKE_brush_channelset_get_float(chset, MAKE_BUILTIN_CH_NAME(channel), mapdata)
#define BRUSHSET_GET_FINAL_FLOAT(childset, parentset, channel, mapdata) \
  BKE_brush_channelset_get_final_float(childset, parentset, MAKE_BUILTIN_CH_NAME(channel), mapdata)
#define BRUSHSET_GET_INT(chset, channel, mapdata) \
  BKE_brush_channelset_get_int(chset, MAKE_BUILTIN_CH_NAME(channel), mapdata)
#define BRUSHSET_GET_BOOL(chset, channel, mapdata) (!!BRUSHSET_GET_INT(chset, channel, mapdata))
#define BRUSHSET_GET_FINAL_INT(child, parent, channel, mapdata) \
  BKE_brush_channelset_get_final_int(child, parent, MAKE_BUILTIN_CH_NAME(channel), mapdata)
#define BRUSHSET_GET_FINAL_BOOL(child, parent, channel, mapdata) \
  BRUSHSET_GET_FINAL_INT(child, parent, channel, mapdata)
#define BRUSHSET_ENSURE_BUILTIN(chset, channel) \
  BKE_brush_channelset_ensure_builtin(chset, MAKE_BUILTIN_CH_NAME(channel))
#define BRUSHSET_SET_FLOAT(chset, channel, val) \
  BKE_brush_channelset_set_float(chset, MAKE_BUILTIN_CH_NAME(channel), val)
#define BRUSHSET_SET_INT(chset, channel, val) \
  BKE_brush_channelset_set_int(chset, MAKE_BUILTIN_CH_NAME(channel), val)
#define BRUSHSET_SET_BOOL(chset, channel, val) BRUSHSET_SET_INT(chset, channel, (val) ? 1 : 0)

#define BRUSHSET_GET_VECTOR(chset, channel, out, mapdata) \
  BKE_brush_channelset_get_vector(chset, MAKE_BUILTIN_CH_NAME(channel), out, mapdata)
#define BRUSHSET_SET_VECTOR(chset, channel, out, mapdata) \
  BKE_brush_channelset_set_vector(chset, MAKE_BUILTIN_CH_NAME(channel), out)
#define BRUSHSET_GET_FINAL_VECTOR(child, parent, channel, out, mapdata) \
  BKE_brush_channelset_get_final_vector(child, parent, MAKE_BUILTIN_CH_NAME(channel), out, mapdata)

//#define DEBUG_CURVE_MAPPING_ALLOC
#ifdef DEBUG_CURVE_MAPPING_ALLOC
void namestack_push(const char *name);
void *namestack_pop(void *passthru);
#endif

typedef void (*BrushChannelIDCallback)(void *userdata,
                                       struct ID *id,
                                       BrushChannelSet *chset,
                                       BrushChannel *ch);
/* TODO: clean up this struct */
typedef struct BrushMappingDef {
  int curve;
  bool enabled;
  bool inv;
  bool inherit;
  float min, max;
  int blendmode;
  float func_cutoff;
  float factor;  // if 0, will default to 1.0
  bool no_default;
} BrushMappingDef;

typedef struct BrushMappingPreset {
  // must match order of BRUSH_MAPPING_XXX enums
  struct BrushMappingDef pressure, xtilt, ytilt, angle, speed, random, stroke_t;
} BrushMappingPreset;

/* input mapping data */
typedef struct BrushMappingData {
  float pressure, xtilt, ytilt, angle, speed, random, stroke_t;
} BrushMappingData;

#define MAX_BRUSH_ENUM_DEF 32

/* copy of PropertyEnumItem only with static char arrays instead of pointers
   for strings */
typedef struct BrushEnumDef {
  int value;
  const char identifier[64];
  char icon[32];  // don't forget when writing literals that icon here is a string, not an int!
  const char name[64];
  const char description[512];
} BrushEnumDef;

/*
  Defines a brush channel.  Includes limits, UI data,
  default values, etc.
*/
typedef struct BrushChannelType {
  char name[128], idname[64], tooltip[512], category[128];
  float min, max, soft_min, soft_max;
  BrushMappingPreset mappings;

  int type, flag;
  int subtype;
  int ivalue; /* for int, bool, enum and bitmask types */
  int icon;
  float fvalue;
  float vector[4]; /* for vector3 & vector4 types */

  int curve_preset;
  bool curve_preset_slope_neg;

  BrushEnumDef enumdef[MAX_BRUSH_ENUM_DEF]; /* for enum/bitmask types */
  EnumPropertyItem *rna_enumdef;

  bool user_defined;         /* this is a user-defined channel; currently unused */
  struct StructRNA *rna_ext; /* private rna pointer */
} BrushChannelType;

/* since MTex is going away lets'
   keep the texture abstraction
   and simple.  From the brush engine's
   point of view it's just a bunch of
   BrushChannels.

   This will eventually end up in DNA.

   Note, this hasn't been fully implemented yet.
   */

typedef struct BrushTex {
  struct BrushTex *next, *prev;
  char idname[64], name[64];

  BrushChannelSet *channels;
  MTex __mtex;  // do not access directly. except for the actual evaluation code.
} BrushTex;

BrushTex *BKE_brush_tex_create(void);
void BKE_brush_tex_free(BrushTex *btex);
void BKE_brush_tex_patch_channels(BrushTex *btex);
void BKE_brush_tex_from_mtex(BrushTex *btex, MTex *mtex);

// initializes the internal mtex struct
void BKE_brush_tex_start(BrushTex *btex, BrushChannelSet *chset, BrushMappingData *mapdata);

#define MAKE_BRUSHTEX_SLOTS 5

/*

*/
typedef struct BrushCommand {
  int tool;                  /* SCULPT_TOOL_XXX value */
  float last_spacing_t[512]; /* has values for all possible symmetry passes */
  struct BrushChannelSet *params;
  struct BrushChannelSet *params_final;  /* with inheritence applied */
  struct BrushChannelSet *params_mapped; /* with pressure etc applied */

  BrushTex *texture_slots[MAKE_BRUSHTEX_SLOTS]; /* currently unused */
} BrushCommand;

typedef struct BrushCommandList {
  BrushCommand *commands;
  int totcommand;
} BrushCommandList;

void BKE_brush_channel_free_data(BrushChannel *ch);
void BKE_brush_channel_free(BrushChannel *ch);

/* creates all the channels needed for a given sculpt tool */
void BKE_brush_builtin_create(struct Brush *brush, int tool);

/*
Copies channel data from src to dst.  Dst's data
will be freed first.
*/
void BKE_brush_channel_copy_data(BrushChannel *dst,
                                 BrushChannel *src,
                                 bool keep_mappings,
                                 bool keep_idname_and_def);

/* Initialize a channel from a channel definition */
void BKE_brush_channel_init(BrushChannel *ch, BrushChannelType *def);

/* Lookup a built-in channel definition by name */
BrushChannelType *BKE_brush_builtin_channel_def_find(const char *name);

/* Create new channel set; info is a guardedalloc tag and should point
   to a static/permanent string */
BrushChannelSet *BKE_brush_channelset_create(const char *info);

/* adds missing channels to exising .channels in brush.
 * if channels do not exist use BKE_brush_builtin_create.
 */
void BKE_brush_builtin_patch(struct Brush *brush, int tool);

/* loads old brush settings to/from channels */
void BKE_brush_channelset_compat_load(BrushChannelSet *chset,
                                      struct Brush *brush,
                                      bool to_channels);

#ifdef DEBUG_CURVE_MAPPING_ALLOC
BrushChannelSet *_BKE_brush_channelset_copy(BrushChannelSet *src);
#  define BKE_brush_channelset_copy(src) \
    (namestack_push(__func__), (BrushChannelSet *)namestack_pop(_BKE_brush_channelset_copy(src)))
#else
BrushChannelSet *BKE_brush_channelset_copy(BrushChannelSet *src);
#endif
void BKE_brush_channelset_free(BrushChannelSet *chset);

void BKE_brush_channelset_add(BrushChannelSet *chset, BrushChannel *ch);

/* makes a copy of ch and adds it to the channel set */
void BKE_brush_channelset_add_duplicate(BrushChannelSet *chset, BrushChannel *ch);

/* finds a unique name for ch, does not add it to chset->namemap */
void BKE_brush_channel_ensure_unque_name(BrushChannelSet *chset, BrushChannel *ch);

/* does not free ch or its data */
void BKE_brush_channelset_remove(BrushChannelSet *chset, BrushChannel *ch);

/* does not free ch or its data */
bool BKE_brush_channelset_remove_named(BrushChannelSet *chset, const char *idname);

/* checks if a channel with existing->idname exists; if not a copy of existing
   is made and inserted
 */
void BKE_brush_channelset_ensure_existing(BrushChannelSet *chset, BrushChannel *existing);

/* Lookup a channel in chset; returns NULL if it does not exist */
BrushChannel *BKE_brush_channelset_lookup(BrushChannelSet *chset, const char *idname);

/* Looks up a channel in child; if it is set to inherit or does not exist, then
   the channel will be looked up in parent; otherwise the one in child is returned.

   Note that this does not apply brush mapping inheritance.
 */
BrushChannel *BKE_brush_channelset_lookup_final(BrushChannelSet *child,
                                                BrushChannelSet *parent,
                                                const char *idname);

/*
takes child and parent channels and builds inherited channel
in dst, with channel and brush mapping inheritance flags
properly resolved.

note that child or parent may be NULL, but not both.
*/
void BKE_brush_channel_copy_final_data(BrushChannel *dst,
                                       BrushChannel *src_child,
                                       BrushChannel *src_parent,
                                       bool keep_mapping,
                                       bool keep_idname_and_def);

bool BKE_brush_channelset_has(BrushChannelSet *chset, const char *idname);

BrushChannel *BKE_brush_channelset_add_builtin(BrushChannelSet *chset, const char *idname);
BrushChannel *BKE_brush_channelset_ensure_builtin(BrushChannelSet *chset, const char *idname);

void BKE_brush_mapping_reset(BrushChannel *ch, int tool, int mapping);
void BKE_brush_mapping_inherit_all(BrushChannel *ch);

void BKE_brush_channelset_inherit_mappings(BrushChannelSet *chset);

void BKE_brush_channelset_merge(BrushChannelSet *dst,
                                BrushChannelSet *child,
                                BrushChannelSet *parent);

void BKE_brush_channel_apply_mapping_flags(BrushChannel *dst,
                                           BrushChannel *child,
                                           BrushChannel *parent);

bool BKE_brush_mapping_is_enabled(BrushChannel *child, BrushChannel *parent, int mapping);

/* iterators over each channel in brush->channels, and any set to inherit will be
   overwritten by equivalent channel in sd->channels */
void BKE_brush_resolve_channels(struct Brush *brush, struct Sculpt *sd);

/* sets channel value in either child or parent depending on inheritance flag */
void BKE_brush_channelset_set_final_int(BrushChannelSet *child,
                                        BrushChannelSet *parent,
                                        const char *idname,
                                        int value);

/* gets channel value in either child or parent depending on inheritance flag */
int BKE_brush_channelset_get_final_int(BrushChannelSet *child,
                                       BrushChannelSet *parent,
                                       const char *idname,
                                       BrushMappingData *mapdata);

/* note that mapdata can be NULL */
int BKE_brush_channelset_get_int(BrushChannelSet *chset,
                                 const char *idname,
                                 BrushMappingData *mapdata);

bool BKE_brush_channelset_set_int(BrushChannelSet *chset, const char *idname, int val);

void BKE_brush_channel_set_int(BrushChannel *ch, int val);

/* mapdata may be NULL */
float BKE_brush_channel_get_int(BrushChannel *ch, BrushMappingData *mapdata);

/* mapdata may be NULL */
float BKE_brush_channel_get_float(BrushChannel *ch, BrushMappingData *mapdata);
void BKE_brush_channel_set_float(BrushChannel *ch, float val);

/* mapdata may be NULL */
float BKE_brush_channelset_get_float(BrushChannelSet *chset,
                                     const char *idname,
                                     BrushMappingData *mapdata);
bool BKE_brush_channelset_set_float(BrushChannelSet *chset, const char *idname, float val);

/* mapdata may be NULL */
float BKE_brush_channelset_get_final_float(BrushChannelSet *child,
                                           BrushChannelSet *parent,
                                           const char *idname,
                                           BrushMappingData *mapdata);

void BKE_brush_channelset_set_final_float(BrushChannelSet *child,
                                          BrushChannelSet *parent,
                                          const char *idname,
                                          float value);

void BKE_brush_channel_set_vector(BrushChannel *ch, float vec[4]);
int BKE_brush_channel_get_vector_size(BrushChannel *ch);

float BKE_brush_channel_curve_evaluate(BrushChannel *ch, float val, const float maxval);

/* feeds f through a brush channel's mapping stack and returns the result */
double BKE_brush_channel_eval_mappings(BrushChannel *ch,
                                       BrushMappingData *mapdata,
                                       double f,
                                       int idx);

/* note the brush system caches curvemappings to avoid excessive copying
   (profiling showed this to be a major problem), so make sure to
   call BKE_brush_channel_curve_ensure_write(curve) before calling this
   function if you want to edit the curve */
CurveMapping *BKE_brush_channel_curvemapping_get(BrushCurve *curve, bool force_create);

/* ensure we can write to this brush curve */
bool BKE_brush_channel_curve_ensure_write(BrushCurve *curve);
void BKE_brush_channel_curve_assign(BrushChannel *ch, BrushCurve *curve);

/* returns size of vector */
int BKE_brush_channel_get_vector(BrushChannel *ch, float out[4], BrushMappingData *mapdata);

/* returns size of vector */
int BKE_brush_channelset_get_final_vector(BrushChannelSet *brushset,
                                          BrushChannelSet *toolset,
                                          const char *idname,
                                          float r_vec[4],
                                          BrushMappingData *mapdata);
void BKE_brush_channelset_set_final_vector(BrushChannelSet *brushset,
                                           BrushChannelSet *toolset,
                                           const char *idname,
                                           float vec[4]);

/* returns size of vector */
int BKE_brush_channelset_get_vector(BrushChannelSet *chset,
                                    const char *idname,
                                    float r_vec[4],
                                    BrushMappingData *mapdata);
bool BKE_brush_channelset_set_vector(BrushChannelSet *chset, const char *idname, float vec[4]);

void BKE_brush_channelset_flag_clear(BrushChannelSet *chset, const char *channel, int flag);
void BKE_brush_channelset_flag_set(BrushChannelSet *chset, const char *channel, int flag);

void BKE_brush_channelset_to_unified_settings(BrushChannelSet *chset,
                                              struct UnifiedPaintSettings *ups);

void BKE_brush_init_toolsettings(struct Sculpt *sd);

void BKE_brush_channelset_clear_inherit(BrushChannelSet *chset);
void BKE_brush_channelset_apply_mapping(BrushChannelSet *chset, BrushMappingData *mapdata);

/*** brush command list stuff ****/

BrushCommandList *BKE_brush_commandlist_create(void);
BrushCommand *BKE_brush_command_init(BrushCommand *command, int tool);
BrushCommand *BKE_brush_commandlist_add(BrushCommandList *cl,
                                        BrushChannelSet *chset_template,
                                        bool auto_inherit);
void BKE_builtin_commandlist_create(struct Brush *brush,
                                    BrushChannelSet *chset,
                                    BrushCommandList *cl,
                                    int tool,
                                    BrushMappingData *map_data);  // map_data may be NULL
void BKE_brush_commandlist_start(BrushCommandList *list,
                                 struct Brush *brush,
                                 BrushChannelSet *chset_final);

void BKE_brush_commandlist_free(BrushCommandList *cl);

/***** filing reading/linking stuff *******/

void BKE_brush_channelset_read(struct BlendDataReader *reader, BrushChannelSet *cset);
void BKE_brush_channelset_write(struct BlendWriter *writer, BrushChannelSet *cset);
void BKE_brush_channelset_read_lib(struct BlendLibReader *reader,
                                   struct ID *id,
                                   BrushChannelSet *chset);
void BKE_brush_channelset_expand(struct BlendExpander *expander,
                                 struct ID *id,
                                 BrushChannelSet *chset);
void BKE_brush_channelset_foreach_id(void *userdata,
                                     BrushChannelSet *chset,
                                     BrushChannelIDCallback callback);

/******** brush mapping stuff *******/
void BKE_brush_mapping_copy_data(BrushMapping *dst, BrushMapping *src);
const char *BKE_brush_mapping_type_to_str(BrushMappingType mapping);
const char *BKE_brush_mapping_type_to_typename(BrushMappingType mapping);

/********* misc utility functions *********/

// merge in channels the ui requested
void BKE_brush_apply_queued_channels(BrushChannelSet *chset, bool do_override);
void BKE_brush_channeltype_rna_check(BrushChannelType *def,
                                     int (*getIconFromName)(const char *name));
bool BKE_brush_mapping_ensure_write(BrushMapping *mp);

void BKE_brush_check_toolsettings(struct Sculpt *sd);
void BKE_brush_channelset_ui_init(struct Brush *brush, int tool);
void BKE_brush_channelset_check_radius(BrushChannelSet *chset);

/* applies "hard edge mode", currently just sets fset_slide to 0.0f */
void BKE_builtin_apply_hard_edge_mode(BrushChannelSet *chset, bool do_apply);

const char *BKE_brush_channel_category_get(BrushChannel *ch);
void BKE_brush_channel_category_set(BrushChannel *ch, const char *str);

/* set up brush curvemapping cache */
void BKE_brush_channel_system_init(void);

/* frees brush curvemapping cache */
void BKE_brush_channel_system_exit(void);

/*
set up static type checker for BRUSHSET_XXX macros
*/
#define BRUSH_CHANNEL_DEFINE_EXTERNAL
#include "intern/brush_channel_define.h"
#undef BRUSH_CHANNEL_DEFINE_EXTERNAL

#ifdef __cplusplus
}
#endif
