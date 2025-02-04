/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tao_bridge/tf/defs.h"

namespace tensorflow {
namespace tao {

const char* const kXlaMustCompileAttr = "_TaoXlaMustCompile";

const char* const kXlaCompileAttr = "_TaoXlaCompile";

// User-provided through jit_scope APIs. Effective only when auto_jit is OFF.
const char* const kXlaScopeAttr = "_TaoXlaScope";

// Automatically inserted by auto_jit to guide clustering results.  Effective
// only when auto_jit is ON.
const char* const kXlaInternalScopeAttr = "_TaoXlaInternalScope";

}  // namespace tao
}  // namespace tensorflow
