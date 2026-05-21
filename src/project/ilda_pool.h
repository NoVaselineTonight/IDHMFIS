#pragma once
// IldaPool — media pool for loaded ILDA files.
// Files are loaded once and cached by integer ID.

#include "ilda.h"
#include <string>
#include <utility>
#include <vector>

namespace idhmfis {

class IldaPool {
public:
    // Load an ILDA file; returns a pool id (>= 0) on success, -1 on failure.
    int load(const std::string& path);

    // Unload a file by id.
    void unload(int id);

    // Get a loaded file by id; returns nullptr if not found.
    const IldaFile* get(int id) const;

    // Number of loaded files.
    int count() const { return static_cast<int>(pool_.size()); }

    // All current (id, source_path) pairs.
    const std::vector<std::pair<int, std::string>>& entries() const { return entries_; }

private:
    std::vector<std::pair<int, IldaFile>> pool_;
    std::vector<std::pair<int, std::string>> entries_;
    int next_id_ = 0;
};

} // namespace idhmfis
