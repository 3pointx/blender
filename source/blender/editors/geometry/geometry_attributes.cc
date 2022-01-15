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
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edgeometry
 */

#include "BLI_math.h"

#include "BKE_attribute.h"
#include "BKE_context.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "DEG_depsgraph.h"

#include "DNA_mesh_types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_object.h"

#include "geometry_intern.hh"

/*********************** Attribute Operators ************************/

static bool geometry_attributes_poll(bContext *C)
{
  Object *ob = ED_object_context(C);
  ID *data = (ob) ? static_cast<ID *>(ob->data) : nullptr;
  return (ob && !ID_IS_LINKED(ob) && data && !ID_IS_LINKED(data)) &&
         BKE_id_attributes_supported(data);
}

static bool geometry_attributes_remove_poll(bContext *C)
{
  if (!geometry_attributes_poll(C)) {
    return false;
  }

  Object *ob = ED_object_context(C);
  ID *data = (ob) ? static_cast<ID *>(ob->data) : nullptr;
  if (BKE_id_attributes_active_get(data) != nullptr) {
    return true;
  }

  return false;
}

static const EnumPropertyItem *geometry_attribute_domain_itemf(bContext *C,
                                                               PointerRNA *UNUSED(ptr),
                                                               PropertyRNA *UNUSED(prop),
                                                               bool *r_free)
{
  if (C == nullptr) {
    return DummyRNA_NULL_items;
  }

  Object *ob = ED_object_context(C);
  if (ob == nullptr) {
    return DummyRNA_NULL_items;
  }

  return rna_enum_attribute_domain_itemf(static_cast<ID *>(ob->data), r_free);
}

static int geometry_attribute_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = ED_object_context(C);
  ID *id = static_cast<ID *>(ob->data);

  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  CustomDataType type = (CustomDataType)RNA_enum_get(op->ptr, "data_type");
  AttributeDomain domain = (AttributeDomain)RNA_enum_get(op->ptr, "domain");
  CustomDataLayer *layer = BKE_id_attribute_new(
      id, name, type, CD_MASK_PROP_ALL, domain, op->reports);

  if (layer == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BKE_id_attributes_active_set(id, layer);

  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);

  return OPERATOR_FINISHED;
}

static void next_color_attr(struct ID *id, CustomDataLayer *layer, bool is_render)
{
  AttributeRef *ref = is_render ? BKE_id_attributes_render_color_ref_p(id) :
                                  BKE_id_attributes_active_color_ref_p(id);

  if (!ref || layer != (is_render ? BKE_id_attributes_render_color_get(id) :
                                    BKE_id_attributes_active_color_get(id))) {
    return;
  }

  AttributeDomainMask domain_mask = ATTR_DOMAIN_MASK_POINT | ATTR_DOMAIN_MASK_CORNER;
  CustomDataMask type_mask = CD_MASK_PROP_COLOR | CD_MASK_MLOOPCOL;

  int length = BKE_id_attributes_length(id, domain_mask, type_mask);
  int idx = BKE_id_attribute_index_from_ref(id, ref, domain_mask, type_mask);

  idx = mod_i(idx + 1, length);

  BKE_id_attribute_ref_from_index(id, idx, domain_mask, type_mask, ref);
}

static void next_color_attrs(struct ID *id, CustomDataLayer *layer)
{
  next_color_attr(id, layer, false); /* active */
  next_color_attr(id, layer, true);  /* render */
}

void GEOMETRY_OT_attribute_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Geometry Attribute";
  ot->description = "Add attribute to geometry";
  ot->idname = "GEOMETRY_OT_attribute_add";

  /* api callbacks */
  ot->poll = geometry_attributes_poll;
  ot->exec = geometry_attribute_add_exec;
  ot->invoke = WM_operator_props_popup_confirm;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  PropertyRNA *prop;

  prop = RNA_def_string(ot->srna, "name", "Attribute", MAX_NAME, "Name", "Name of new attribute");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna,
                      "domain",
                      rna_enum_attribute_domain_items,
                      ATTR_DOMAIN_POINT,
                      "Domain",
                      "Type of element that attribute is stored on");
  RNA_def_enum_funcs(prop, geometry_attribute_domain_itemf);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna,
                      "data_type",
                      rna_enum_attribute_type_items,
                      CD_PROP_FLOAT,
                      "Data Type",
                      "Type of data stored in attribute");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static int geometry_attribute_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = ED_object_context(C);
  ID *id = static_cast<ID *>(ob->data);
  CustomDataLayer *layer = BKE_id_attributes_active_get(id);

  if (layer == nullptr) {
    return OPERATOR_CANCELLED;
  }

  next_color_attrs(id, layer);

  if (!BKE_id_attribute_remove(id, layer, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  int *active_index = BKE_id_attributes_active_index_p(id);
  if (*active_index > 0) {
    *active_index -= 1;
  }

  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);

  return OPERATOR_FINISHED;
}

void GEOMETRY_OT_attribute_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Geometry Attribute";
  ot->description = "Remove attribute from geometry";
  ot->idname = "GEOMETRY_OT_attribute_remove";

  /* api callbacks */
  ot->exec = geometry_attribute_remove_exec;
  ot->poll = geometry_attributes_remove_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int geometry_color_attribute_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = ED_object_context(C);
  ID *id = ob->data;

  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  CustomDataType type = (CustomDataType)RNA_enum_get(op->ptr, "data_type");
  AttributeDomain domain = (AttributeDomain)RNA_enum_get(op->ptr, "domain");
  CustomDataLayer *layer = BKE_id_attribute_new(
      id, name, type, domain, CD_MASK_PROP_ALL, op->reports);

  if (layer == NULL) {
    return OPERATOR_CANCELLED;
  }

  BKE_id_attributes_active_color_set(id, layer);

  if (!BKE_id_attributes_render_color_get(id)) {
    BKE_id_attributes_render_color_set(id, layer);
  }

  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);

  return OPERATOR_FINISHED;
}

void GEOMETRY_OT_color_attribute_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Geometry Attribute";
  ot->description = "Add attribute to geometry";
  ot->idname = "GEOMETRY_OT_color_attribute_add";

  /* api callbacks */
  ot->poll = geometry_attributes_poll;
  ot->exec = geometry_color_attribute_add_exec;
  ot->invoke = WM_operator_props_popup_confirm;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  PropertyRNA *prop;

  prop = RNA_def_string(ot->srna, "name", "Color", MAX_NAME, "Name", "Name of color attribute");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  static EnumPropertyItem domains[3] = {{ATTR_DOMAIN_POINT, "POINT", -1, "Point", ""},
                                        {ATTR_DOMAIN_CORNER, "CORNER", -1, "Face Corner", ""},
                                        {0, NULL, 0, NULL, NULL}};

  static EnumPropertyItem types[3] = {{CD_PROP_COLOR, "COLOR", -1, "Color", ""},
                                      {CD_MLOOPCOL, "BYTE_COLOR", -1, "Byte Color", ""},
                                      {0, NULL, 0, NULL, NULL}};

  prop = RNA_def_enum(ot->srna,
                      "domain",
                      domains,
                      ATTR_DOMAIN_POINT,
                      "Domain",
                      "Type of element that attribute is stored on");

  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna,
                      "data_type",
                      types,
                      CD_PROP_COLOR,
                      "Data Type",
                      "Type of data stored in attribute");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static int geometry_color_attribute_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = ED_object_context(C);
  ID *id = ob->data;
  CustomDataLayer *layer = BKE_id_attributes_active_color_get(id);

  if (layer == NULL) {
    return OPERATOR_CANCELLED;
  }

  next_color_attrs(id, layer);

  if (!BKE_id_attribute_remove(id, layer, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);

  return OPERATOR_FINISHED;
}

static bool geometry_color_attributes_remove_poll(bContext *C)
{
  if (!geometry_attributes_poll(C)) {
    return false;
  }

  Object *ob = ED_object_context(C);
  ID *data = (ob) ? ob->data : NULL;
  if (BKE_id_attributes_active_color_get(data) != NULL) {
    return true;
  }

  return false;
}
void GEOMETRY_OT_color_attribute_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Color Attribute";
  ot->description = "Remove color attribute from geometry";
  ot->idname = "GEOMETRY_OT_color_attribute_remove";

  /* api callbacks */
  ot->exec = geometry_color_attribute_remove_exec;
  ot->poll = geometry_color_attributes_remove_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
