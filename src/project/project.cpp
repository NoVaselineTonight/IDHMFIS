// project.cpp — Project utility functions: UUID generation, save/load, migration.

#include "serialization.h"
#include "../core/logger.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <random>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <string_view>

#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  UUID v4 generator
//  Uses std::mt19937_64 seeded from std::random_device so that generated IDs
//  are unique per process. Thread-safe under C++11 via function-local statics.
// ─────────────────────────────────────────────────────────────────────────────
std::string Project::new_id() {
    // Thread-local RNG so there is no locking overhead
    thread_local std::mt19937_64 rng{ std::random_device{}() };
    thread_local std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    // Set version bits (version 4)
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    // Set variant bits (variant 1, RFC 4122)
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    // Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint16_t>(hi >> 16),
        static_cast<uint16_t>(hi),
        static_cast<uint16_t>(lo >> 48),
        static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFull));
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ISO 8601 UTC timestamp helper
// ─────────────────────────────────────────────────────────────────────────────
static std::string utc_now_iso8601() {
    auto now   = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_MSC_VER) || defined(IDHMFIS_WINDOWS)
    gmtime_s(&tm_val, &tt);
#else
    gmtime_r(&tt, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Schema migration
// ─────────────────────────────────────────────────────────────────────────────
bool Project::needs_migration(const std::string& schema_ver) {
    return schema_ver != kCurrentSchemaVersion;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Migration dispatcher — called from from_json after initial parse.
//  Add new cases here as the schema evolves.
// ─────────────────────────────────────────────────────────────────────────────
static void migrate_project(Project& p, const std::string& from_ver) {
    // 2.8.0 → 2.9.0: added audio_track in TimelineDef and ui_layout in Project.
    // Fields are optional in from_json so old files load cleanly with defaults.
    (void)p;
    (void)from_ver;

    // After all migrations, stamp the new version.
    p.schema_version = kCurrentSchemaVersion;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHA-256 integrity helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr std::string_view kIntegrityPrefix = "\n// IDHMFIS_INTEGRITY sha256:";
static constexpr int              kHashHexLen       = 64;

#if defined(_WIN32) || defined(IDHMFIS_WINDOWS)
static std::string sha256_hex(const char* data, std::size_t len) {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("BCrypt: cannot open SHA-256 provider");

    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCrypt: cannot create hash");
    }

    NTSTATUS status = BCryptHashData(hash,
                                     reinterpret_cast<PUCHAR>(const_cast<char*>(data)),
                                     static_cast<ULONG>(len), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCrypt: BCryptHashData failed");
    }

    BYTE digest[32]{};
    ULONG digest_len = sizeof(digest);
    status = BCryptFinishHash(hash, digest, digest_len, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCrypt: BCryptFinishHash failed");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    char hex[kHashHexLen + 1]{};
    for (int i = 0; i < 32; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    return std::string(hex, kHashHexLen);
}
#else
// Minimal portable SHA-256 for non-Windows builds (public domain, RFC 6234 derived)
static std::string sha256_hex(const char* data, std::size_t len) {
    // 64-byte block SHA-256 — RFC 4634 §8.2.2
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    // Process in 64-byte chunks; include padding inline
    auto process_block = [&](const uint8_t* blk) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(blk[i*4])<<24)|(uint32_t(blk[i*4+1])<<16)|
                   (uint32_t(blk[i*4+2])<<8)|uint32_t(blk[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^(~e&g);
            uint32_t t1=hh+S1+ch+K[i]+w[i];
            uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    };
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    std::size_t i = 0;
    for (; i + 64 <= len; i += 64) process_block(src + i);
    uint8_t tail[128]{};
    std::size_t rem = len - i;
    std::memcpy(tail, src + i, rem);
    tail[rem] = 0x80;
    std::size_t pad_end = (rem < 56) ? 64 : 128;
    for (int k = 7; k >= 0; --k) { tail[pad_end-1-k] = static_cast<uint8_t>(bit_len>>(k*8)); }
    process_block(tail);
    if (pad_end == 128) process_block(tail + 64);
    char hex[kHashHexLen + 1]{};
    for (int j = 0; j < 8; ++j)
        std::snprintf(hex + j*8, 9, "%08x", h[j]);
    return std::string(hex, kHashHexLen);
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Serialisation
// ─────────────────────────────────────────────────────────────────────────────
nlohmann::json Project::to_json() const {
    nlohmann::json j;
    idhmfis::to_json(j, *this);
    return j;
}

Project Project::from_json(const nlohmann::json& j) {
    Project p;
    idhmfis::from_json(j, p);

    if (needs_migration(p.schema_version)) {
        std::string from_ver = p.schema_version;
        // BUG #54: detect files saved with a newer schema version.
        // Do not refuse to load — just warn and preserve the original version
        // so it is not silently downgraded when re-saved.
        if (from_ver > kCurrentSchemaVersion) {
            log::warn("Project: file was saved with newer schema version %s (current: %s). "
                      "Some features may be missing.",
                      from_ver.c_str(), kCurrentSchemaVersion);
            // Do NOT call migrate_project — leave p.schema_version untouched
            // so a re-save does not downgrade the version stamp.
        } else {
            migrate_project(p, from_ver);
        }
    }

    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Atomic save
//  Writes to <path>.tmp, then renames to <path>. On most OS / file systems
//  rename() is atomic with respect to power-loss, so a partial save never
//  leaves a corrupt .idhmfis file.
// ─────────────────────────────────────────────────────────────────────────────
bool Project::save(const std::string& path) const {
    // Stamp modified_at before serialising
    Project copy = *this;
    copy.modified_at = utc_now_iso8601();
    if (copy.created_at.empty())
        copy.created_at = copy.modified_at;

    nlohmann::json j;
    idhmfis::to_json(j, copy);

    std::string json_str = j.dump(2);
    std::string hash     = sha256_hex(json_str.data(), json_str.size());
    json_str += std::string(kIntegrityPrefix) + hash + "\n";

    std::string tmp_path = path + ".tmp";

    {
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!ofs.is_open())
            throw std::runtime_error("Cannot open for writing: " + tmp_path);
        ofs.write(json_str.data(), static_cast<std::streamsize>(json_str.size()));
        if (!ofs.good())
            throw std::runtime_error("Write error: " + tmp_path);
    }

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback: copy then remove (e.g. cross-device)
        std::filesystem::copy_file(tmp_path, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            throw std::runtime_error("Cannot rename/copy save file: " + ec.message());
        std::filesystem::remove(tmp_path, ec);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Load
// ─────────────────────────────────────────────────────────────────────────────
// Strip integrity trailer and return the JSON portion.
// Sets integrity_present and integrity_ok on the result.
static std::string strip_integrity(const std::string& content,
                                   bool& integrity_present,
                                   bool& integrity_ok)
{
    integrity_present = false;
    integrity_ok      = true;

    auto pos = content.rfind(kIntegrityPrefix);
    if (pos == std::string::npos)
        return content;

    integrity_present = true;
    std::string json_part    = content.substr(0, pos);
    std::string after_prefix = content.substr(pos + kIntegrityPrefix.size());

    if (after_prefix.size() >= static_cast<std::size_t>(kHashHexLen)) {
        std::string stored_hash   = after_prefix.substr(0, kHashHexLen);
        std::string computed_hash = sha256_hex(json_part.data(), json_part.size());
        integrity_ok = (stored_hash == computed_hash);
    } else {
        integrity_ok = false;
    }

    return json_part;
}

Project Project::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
        throw ProjectLoadError("File not found: " + path,
                               ProjectLoadError::Reason::FileNotFound);

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    bool integrity_present = false, integrity_ok = true;
    std::string json_part = strip_integrity(content, integrity_present, integrity_ok);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_part);
    } catch (const nlohmann::json::parse_error& e) {
        throw ProjectLoadError(
            std::string("JSON parse error in '") + path + "': " + e.what(),
            ProjectLoadError::Reason::ParseError);
    }

    if (!j.is_object())
        throw ProjectLoadError("Root JSON element is not an object: " + path,
                               ProjectLoadError::Reason::InvalidData);

    std::string file_schema = j.value("schema_version", std::string{});
    if (file_schema.empty())
        throw ProjectLoadError(
            "Missing schema_version in '" + path + "'. File may be corrupt.",
            ProjectLoadError::Reason::SchemaMismatch);

    try {
        return Project::from_json(j);
    } catch (const nlohmann::json::exception& e) {
        throw ProjectLoadError(
            std::string("Failed to deserialise '") + path + "': " + e.what(),
            ProjectLoadError::Reason::MissingField);
    }
}

ProjectLoadResult project_load_full(const std::string& path) {
    ProjectLoadResult result;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        result.fatal_error = "File not found: " + path;
        return result;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    std::string json_part = strip_integrity(content,
                                            result.integrity_present,
                                            result.integrity_ok);

    if (!result.integrity_ok && result.integrity_present)
        result.warnings.push_back("Integrity check failed — file may be corrupt or tampered.");

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_part);
    } catch (const nlohmann::json::parse_error& e) {
        result.fatal_error = std::string("JSON parse error: ") + e.what();
        return result;
    }

    if (!j.is_object()) {
        result.fatal_error = "Root JSON element is not an object.";
        return result;
    }

    std::string file_schema = j.value("schema_version", std::string{});
    if (file_schema.empty()) {
        result.fatal_error = "Missing schema_version. File may be corrupt.";
        return result;
    }

    try {
        result.project = Project::from_json(j);
    } catch (const std::exception& e) {
        result.fatal_error = std::string("Deserialisation failed: ") + e.what();
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace idhmfis
