/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2015 3 Point X. All rights reserved. */

#include "openvdb_capi.h"
#include <openvdb/openvdb.h>

int OpenVDB_getVersionHex()
{
  return openvdb::OPENVDB_LIBRARY_VERSION;
}
