#include "core/vfs.h"

#include <algorithm>
#include <fstream>

#include <zlib.h>

#include "core/xor32.h"

namespace nova {

namespace {

constexpr size_t kEocdSize = 22;
constexpr size_t kCdEntrySize = 46;
constexpr size_t kLocalHeaderSize = 30;

Bytes read_range(const std::filesystem::path& path, uint64_t offset, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot open " + path.string());
    f.seekg(static_cast<std::streamoff>(offset));
    Bytes data(n);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n));
    if (static_cast<size_t>(f.gcount()) != n) throw FormatError("short read in " + path.string());
    return data;
}

// XOR with the repeating key, key index 0 at `data[0]`.
void unxor(uint8_t* data, size_t n, const std::string& key, size_t phase = 0) {
    for (size_t i = 0; i < n; ++i) data[i] ^= static_cast<uint8_t>(key[(i + phase) % key.size()]);
}

std::string base_name(const std::string& name) {
    auto slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

}  // namespace

size_t Vfs::mount_directory(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> paths;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        if (de.is_regular_file() && to_lower(de.path().extension().string()) == ".gla") paths.push_back(de.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) mount_archive(p);
    return paths.size();
}

void Vfs::mount_archive(const std::filesystem::path& path) {
    const auto index = static_cast<uint32_t>(archives_.size());
    archives_.push_back({path, path.filename().string()});
    const uint64_t size = std::filesystem::file_size(path);
    const Bytes head = read_range(path, 0, size_t(std::min<uint64_t>(size, 16)));
    if (head.size() >= 4 && (std::memcmp(head.data(), "SB", 2) == 0 || std::memcmp(head.data(), "SEND", 4) == 0)) {
        mount_zip(index, size);
    } else {
        mount_pak(index, head, size);
    }
}

void Vfs::mount_zip(uint32_t index, uint64_t file_size) {
    const std::string& key = archives_[index].key;
    if (file_size < kEocdSize) throw FormatError("archive too small");
    const Bytes data = read_range(archives_[index].path, file_size - kEocdSize, kEocdSize);
    // The end record is either a bare "SEND" region or the tail of the central
    // directory's XOR stream, whose phase is unknown: try them all.
    const size_t eocd_off = 0;   // `data` holds just the end record
    uint8_t eocd[kEocdSize];
    bool found = false;
    if (std::memcmp(&data[eocd_off], "SEND", 4) == 0) {
        std::memcpy(eocd, &data[eocd_off], kEocdSize);
        unxor(eocd + 4, kEocdSize - 4, key);
        found = true;
    } else {
        for (size_t phase = 0; phase < key.size() && !found; ++phase) {
            std::memcpy(eocd, &data[eocd_off], kEocdSize);
            unxor(eocd, kEocdSize, key, phase);
            found = std::memcmp(eocd, "PK\x05\x06", 4) == 0;
        }
    }
    if (!found) throw FormatError(archives_[index].path.string() + ": end record not found");
    const uint16_t count = load_le<uint16_t>(eocd + 10);
    const uint32_t cd_size = load_le<uint32_t>(eocd + 12);
    const uint32_t cd_off = load_le<uint32_t>(eocd + 16);
    if (count == 0) return;
    if (uint64_t(cd_off) + cd_size > file_size) throw FormatError("central directory past end of archive");
    Bytes cd = read_range(archives_[index].path, cd_off, cd_size);
    if (std::memcmp(cd.data(), "SDIR", 4) != 0) throw FormatError("bad central directory signature");
    unxor(cd.data() + 4, cd.size() - 4, key);
    size_t pos = 0;
    for (uint16_t i = 0; i < count; ++i) {
        check_range(cd.size(), pos, kCdEntrySize);
        if (i > 0 && std::memcmp(&cd[pos], "PK\x01\x02", 4) != 0) throw FormatError("bad central directory entry");
        Entry e;
        e.archive = index;
        const uint16_t method = load_le<uint16_t>(&cd[pos + 10]);
        e.crc = load_le<uint32_t>(&cd[pos + 16]);
        e.stored_size = load_le<uint32_t>(&cd[pos + 20]);
        e.size = load_le<uint32_t>(&cd[pos + 24]);
        const uint16_t name_len = load_le<uint16_t>(&cd[pos + 28]);
        const uint16_t extra_len = load_le<uint16_t>(&cd[pos + 30]);
        const uint16_t comment_len = load_le<uint16_t>(&cd[pos + 32]);
        e.offset = load_le<uint32_t>(&cd[pos + 42]);
        check_range(cd.size(), pos + kCdEntrySize, name_len);
        e.name.assign(reinterpret_cast<const char*>(&cd[pos + kCdEntrySize]), name_len);
        if (method == 0) e.method = Entry::Stored;
        else if (method == 8) e.method = Entry::Deflate;
        else throw FormatError(e.name + ": unsupported compression method");
        add(std::move(e));
        pos += kCdEntrySize + name_len + extra_len + comment_len;
    }
}

void Vfs::mount_pak(uint32_t index, const Bytes& head, uint64_t file_size) {
    const uint32_t data_start = read_be<uint32_t>(head, 4);
    const uint32_t name_table = read_be<uint32_t>(head, 8);
    const uint32_t count = read_be<uint32_t>(head, 12);
    if (count == 0 || name_table != 16 + 16 * count || data_start < name_table || data_start > file_size) {
        throw FormatError(archives_[index].path.string() + ": unrecognized archive format");
    }
    const Bytes data = read_range(archives_[index].path, 0, data_start);   // index + name table
    for (uint32_t i = 0; i < count; ++i) {
        const size_t rec = 16 + 16 * size_t(i);
        Entry e;
        e.archive = index;
        e.offset = read_be<uint32_t>(data, rec);
        e.size = e.stored_size = read_be<uint32_t>(data, rec + 4);
        const uint32_t name_off = read_be<uint32_t>(data, rec + 8);
        const uint32_t flags = read_be<uint32_t>(data, rec + 12);
        // Each name runs up to the NUL before the next name (or the table end).
        const uint32_t end = i + 1 < count ? read_be<uint32_t>(data, rec + 16 + 8) - 1 : data_start - name_table - 1;
        if (end < name_off) throw FormatError("bad pak name table");
        Bytes name(data.begin() + name_table + name_off, data.begin() + name_table + end);
        decode_xor32(name.data(), name.size());
        e.name.assign(name.begin(), name.end());
        e.method = (flags & 1) ? Entry::PakEncrypted : Entry::Pak;
        if (e.offset + e.size > file_size) throw FormatError(e.name + ": data runs past end of archive");
        add(std::move(e));
    }
}

void Vfs::add(Entry e) {
    const auto i = static_cast<uint32_t>(entries_.size());
    by_name_[to_lower(base_name(e.name))].push_back(i);
    entries_.push_back(std::move(e));
}

const std::vector<uint32_t>& Vfs::find(const std::string& name) const {
    static const std::vector<uint32_t> none;
    auto it = by_name_.find(to_lower(base_name(name)));
    return it == by_name_.end() ? none : it->second;
}

Bytes Vfs::read(const std::string& name) const {
    const auto& hits = find(name);
    if (hits.empty()) throw FormatError("file not found: " + name);
    return read(hits.front());
}

Bytes Vfs::read(uint32_t index, bool verify) const {
    const Entry& e = entries_.at(index);
    const Archive& a = archives_[e.archive];
    if (e.method == Entry::Pak || e.method == Entry::PakEncrypted) {
        Bytes out = read_range(a.path, e.offset, e.size);
        if (e.method == Entry::PakEncrypted) decode_xor32(out.data(), out.size());
        return out;
    }
    // Scrambled ZIP: the local header after its "SB" + u16 signature is XORed from key index 0.
    Bytes header = read_range(a.path, e.offset, kLocalHeaderSize);
    if (header[0] != 'S' || header[1] != 'B') throw FormatError(e.name + ": bad local header signature");
    unxor(header.data() + 4, kLocalHeaderSize - 4, a.key);
    const uint16_t name_len = load_le<uint16_t>(&header[26]);
    const uint16_t extra_len = load_le<uint16_t>(&header[28]);
    Bytes stored = read_range(a.path, e.offset + kLocalHeaderSize + name_len + extra_len, e.stored_size);
    Bytes out;
    if (e.method == Entry::Stored) {
        out = std::move(stored);
    } else {
        out.resize(e.size);
        z_stream zs{};
        if (inflateInit2(&zs, -15) != Z_OK) throw FormatError("inflateInit failed");
        zs.next_in = stored.data();
        zs.avail_in = static_cast<uInt>(stored.size());
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END || zs.total_out != e.size) throw FormatError(e.name + ": inflate failed");
    }
    if (verify && crc32(0, out.data(), static_cast<uInt>(out.size())) != e.crc) {
        throw FormatError(e.name + ": CRC mismatch");
    }
    return out;
}

}  // namespace nova
