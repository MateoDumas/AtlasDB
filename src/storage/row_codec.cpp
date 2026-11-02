#include "storage/row_codec.h"

#include "common/bytes.h"

namespace atlas {

std::string encode_row(const Row& row) {
    ByteWriter w;
    w.row(row);
    return w.take();
}

Row decode_row(std::string_view bytes) {
    ByteReader r(bytes);
    return r.row();
}

}  // namespace atlas
