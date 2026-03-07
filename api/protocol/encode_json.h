#pragma once

#include <string>

#include "protocol.h"

namespace protocol {

// DO NOT change field names/types without bumping protocol version and updating
// frontend parsing code.
std::string encode_snapshot_json(const Snapshot& s);

}  // namespace protocol
