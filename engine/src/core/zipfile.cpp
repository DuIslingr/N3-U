#include "core/zipfile.h"

#include <zlib.h>

namespace nova {

Bytes unzip_member(const Bytes& zip, const std::string& wanted) {
    size_t pos = 0;
    while (pos + 30 <= zip.size() && std::memcmp(&zip[pos], "PK\x03\x04", 4) == 0) {
        const uint16_t method = read_le<uint16_t>(zip, pos + 8);
        const uint32_t csize = read_le<uint32_t>(zip, pos + 18), usize = read_le<uint32_t>(zip, pos + 22);
        const uint16_t nlen = read_le<uint16_t>(zip, pos + 26), xlen = read_le<uint16_t>(zip, pos + 28);
        check_range(zip.size(), pos + 30, size_t(nlen) + xlen + csize);
        const std::string name(reinterpret_cast<const char*>(&zip[pos + 30]), nlen);
        const uint8_t* src = &zip[pos + 30 + nlen + xlen];
        if (name == wanted) {
            if (method == 0) return Bytes(src, src + csize);
            if (method != 8) throw FormatError(name + ": unsupported ZIP method");
            Bytes out(usize);
            z_stream zs{};
            inflateInit2(&zs, -15);
            zs.next_in = const_cast<uint8_t*>(src);
            zs.avail_in = csize;
            zs.next_out = out.data();
            zs.avail_out = usize;
            const int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END) throw FormatError("inflate failed in " + name);
            return out;
        }
        pos += 30 + nlen + xlen + csize;
    }
    return {};
}

}  // namespace nova
