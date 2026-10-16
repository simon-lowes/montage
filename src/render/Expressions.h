// Montage — editing parameter expressions (core/Expression.h): setting one on a clip's setting, and baking it to keys
// for interchange (other applications read keys, not Lua).
#pragma once

#include <string>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage::edit {

// Puts `expr` on `param` of a clip's effect (`effect` 0 = its Transform), or takes it off with ""; fails, changing
// nothing, when it does not compile.
Result setExpression(Project& p, Sequence& s, Id clip, Id effect, const std::string& param, const std::string& expr);

// Replaces the expression on `param` with what it gives: a key a frame through the clip, thinned to the ones needed to
// stay within a thousandth of the setting's range (held keys for switches and choices).
Result bakeExpression(Project& p, Sequence& s, Id clip, Id effect, const std::string& param);

}  // namespace montage::edit
