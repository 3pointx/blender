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
 * The Original Code is Copyright (C) 2016 Kévin Dietrich.
 * All rights reserved.
 */
#pragma once

/** \file
 * \ingroup balembic
 */

#include "abc_reader_mesh.h"
#include "abc_reader_object.h"

struct Curves;

#define ABC_CURVE_RESOLUTION_U_PROPNAME "blender:resolution"

namespace blender::io::alembic {

class AbcCurveReader final : public AbcObjectReader {
  Alembic::AbcGeom::ICurvesSchema m_curves_schema;

 public:
  AbcCurveReader(const Alembic::Abc::IObject &object, ImportSettings &settings);

  bool valid() const override;
  bool accepts_object_type(const Alembic::AbcCoreAbstract::ObjectHeader &alembic_header,
                           const Object *const ob,
                           const char **err_str) const override;

  void readObjectData(Main *bmain,
                      const AbcReaderManager &manager,
                      const Alembic::Abc::ISampleSelector &sample_sel) override;

  /**
   * \note Alembic only stores data about control points, but the Mesh
   * passed from the cache modifier contains the #DispList, which has more data
   * than the control points, so to avoid corrupting the #DispList we modify the
   * object directly and create a new Mesh from that. Also we might need to
   * create new or delete existing NURBS in the curve.
   */
  void read_geometry(GeometrySet &geometry_set,
                     const Alembic::Abc::ISampleSelector &sample_sel,
                     const AttributeSelector *attribute_selector,
                     int read_flag,
                     const float velocity_scale,
                     const char **err_str) override;

  void read_curves_sample(Curves *curves,
                          const Alembic::AbcGeom::ICurvesSchema &schema,
                          const Alembic::Abc::ISampleSelector &sample_selector);

  Curves *read_curves(Curves *curves_input,
                      const Alembic::Abc::ISampleSelector &sample_sel,
                      const AttributeSelector *attribute_selector,
                      const float velocity_scale,
                      const char **err_str);
};

}  // namespace blender::io::alembic
