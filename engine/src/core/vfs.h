// Virtual file system over the game's .gla archives.
//
// Two archive formats share the .gla extension (see tools/gla.py for details):
//  * scrambled ZIP (CScrambledZipReader): ZIP headers XORed with the archive's
//    file name, signatures replaced by "SB"/"SDIR"/"SEND"; data stored or deflated;
//  * custom pak (CCustomPakReader): big-endian index, names and optionally data
//    encrypted with DECODE_XOR32.
// Files are looked up by lowercase base name, which is how the game refers to them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/bytes.h"

namespace nova {

class Vfs {
public:
    struct Entry {
        uint32_t archive = 0;
        std::string name;           // as stored in the archive
        uint64_t offset = 0;        // ZIP: local header offset; pak: data offset
        uint32_t stored_size = 0;
        uint32_t size = 0;
        uint32_t crc = 0;           // ZIP only
        enum Method : uint8_t { Stored, Deflate, Pak, PakEncrypted } method = Stored;
    };

    // Mount every .gla in `dir`. Returns the number of archives mounted.
    size_t mount_directory(const std::filesystem::path& dir);
    void mount_archive(const std::filesystem::path& path);

    // All entries with this base name (case-insensitive), in mount order.
    const std::vector<uint32_t>& find(const std::string& name) const;
    bool exists(const std::string& name) const { return !find(name).empty(); }

    // Read the first entry with this name; throws FormatError if missing.
    Bytes read(const std::string& name) const;
    Bytes read(uint32_t entry, bool verify = true) const;

    const Entry& entry(uint32_t i) const { return entries_[i]; }
    size_t entry_count() const { return entries_.size(); }
    const std::filesystem::path& archive_path(uint32_t a) const { return archives_[a].path; }
    size_t archive_count() const { return archives_.size(); }

private:
    struct Archive {
        std::filesystem::path path;
        std::string key;            // scrambled ZIP XOR key (the file name)
    };
    void mount_zip(uint32_t index, uint64_t file_size);
    void mount_pak(uint32_t index, const Bytes& head, uint64_t file_size);
    void add(Entry e);

    std::vector<Archive> archives_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::vector<uint32_t>> by_name_;
};

}  // namespace nova
