// Copyright 2016 3 Point X. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// Author: Sergey Sharybin

#include "internal/topology/topology_refiner_impl.h"

namespace blender {
namespace opensubdiv {

TopologyRefinerImpl::TopologyRefinerImpl() : topology_refiner(nullptr)
{
}

TopologyRefinerImpl::~TopologyRefinerImpl()
{
  delete topology_refiner;
}

}  // namespace opensubdiv
}  // namespace blender
