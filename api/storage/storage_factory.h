#pragma once

#include <memory>

#include "storage.h"

namespace storage {

std::unique_ptr<IStorage> CreateStorageFromEnv();

}  // namespace storage
