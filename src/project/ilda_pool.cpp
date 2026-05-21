// IldaPool — media pool for loaded ILDA files.

#include "ilda_pool.h"
#include <algorithm>

namespace idhmfis {

int IldaPool::load(const std::string& path)
{
    std::string err;
    IldaFile file = ilda_import(path, err);
    if (file.frames.empty()) return -1;

    int id = next_id_++;
    pool_.emplace_back(id, std::move(file));
    entries_.emplace_back(id, path);
    return id;
}

void IldaPool::unload(int id)
{
    pool_.erase(
        std::remove_if(pool_.begin(), pool_.end(),
                       [id](const std::pair<int, IldaFile>& p) { return p.first == id; }),
        pool_.end());
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [id](const std::pair<int, std::string>& p) { return p.first == id; }),
        entries_.end());
}

const IldaFile* IldaPool::get(int id) const
{
    for (const auto& p : pool_)
        if (p.first == id) return &p.second;
    return nullptr;
}

} // namespace idhmfis
