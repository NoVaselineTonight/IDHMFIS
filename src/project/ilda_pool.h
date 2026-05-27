#pragma once
// IldaPool — media pool for loaded ILDA files.
// Files are loaded once and cached by integer ID.

#include "ilda.h"
#include <mutex>
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
    int count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return static_cast<int>(pool_.size());
    }

    // Returns a snapshot copy of (id, source_path) pairs.
    // H-12: returns by value so the caller holds a safe snapshot rather than a
    // reference into the pool that the engine thread may mutate concurrently.
    std::vector<std::pair<int, std::string>> entries() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return entries_;
    }

private:
    mutable std::mutex mtx_;  // H-12: load/unload/get from different threads
    std::vector<std::pair<int, IldaFile>> pool_;
    std::vector<std::pair<int, std::string>> entries_;
    int next_id_ = 0;
};

} // namespace idhmfis
