# SPDX-License-Identifier: Apache-2.0
# Copyright 2011-2022 3 Point X

set(LICENSES
  Apache2-license.txt
  BSD-3-Clause-license.txt
  MIT-license.txt
  readme.txt
  SPDX-license-identifiers.txt
  Zlib-license.txt
)

delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${LICENSES}" ${CYCLES_INSTALL_PATH}/license)
