/*
 * Copyright 2014 3 Point X
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#ifndef __SDL_EW_H__
#define __SDL_EW_H__

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SDLEW_SUCCESS = 0,
  SDLEW_ERROR_OPEN_FAILED = -1,
  SDLEW_ERROR_ATEXIT_FAILED = -2,
  SDLEW_ERROR_VERSION = -3,
};

int sdlewInit(void);

#ifdef __cplusplus
}
#endif

#endif  /* __SDL_EW_H__ */
