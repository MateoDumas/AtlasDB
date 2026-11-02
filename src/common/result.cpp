#include "common/result.h"

#include <algorithm>

namespace atlas {

namespace {

// Display width of a UTF-8 string (counts code points, not bytes).
size_t display_width(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

void pad(std::string& out, const std::string& s, size_t width, bool right) {
    size_t w = display_width(s);
    if (right) out.append(width - w, ' ');
    out += s;
    if (!right) out.append(width - w, ' ');
}

}  // namespace

std::string format_result(const QueryResult& r) {
    if (!r.has_rows) return r.tag + "\n";

    size_t ncols = r.columns.size();
    std::vector<size_t> widths(ncols);
    std::vector<bool> numeric(ncols, false);
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = display_width(r.columns[c]);
        if (c < r.types.size())
            numeric[c] = r.types[c] == DataType::Integer || r.types[c] == DataType::Real;
    }
    std::vector<std::vector<std::string>> cells;
    cells.reserve(r.rows.size());
    for (const auto& row : r.rows) {
        auto& line = cells.emplace_back();
        for (size_t c = 0; c < ncols; ++c) {
            line.push_back(c < row.size() ? row[c].to_string() : "");
            widths[c] = std::max(widths[c], display_width(line.back()));
        }
    }

    std::string sep = "+";
    for (auto w : widths) sep += std::string(w + 2, '-') + "+";
    sep += "\n";

    std::string out = sep + "|";
    for (size_t c = 0; c < ncols; ++c) {
        out += ' ';
        pad(out, r.columns[c], widths[c], false);
        out += " |";
    }
    out += "\n" + sep;
    for (const auto& line : cells) {
        out += "|";
        for (size_t c = 0; c < ncols; ++c) {
            out += ' ';
            pad(out, line[c], widths[c], numeric[c]);
            out += " |";
        }
        out += "\n";
    }
    if (!cells.empty()) out += sep;
    out += "(" + std::to_string(r.rows.size()) + (r.rows.size() == 1 ? " row)\n" : " rows)\n");
    return out;
}

}  // namespace atlas
