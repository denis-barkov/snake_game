#include "encode_json.h"

#include <iomanip>
#include <sstream>

namespace protocol {
namespace {

std::string json_escape(const std::string& in) {
  std::ostringstream out;
  for (unsigned char c : in) {
    switch (c) {
      case '\"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  return out.str();
}

void append_vec2_array(std::ostringstream& out, const std::vector<Vec2>& points) {
  out << "[";
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& p = points[i];
    out << "{\"x\":" << p.x << ",\"y\":" << p.y << "}";
    if (i + 1 < points.size()) out << ",";
  }
  out << "]";
}

}  // namespace

std::string encode_snapshot_json(const Snapshot& s) {
  std::ostringstream out;
  out << "{";
  out << "\"tick\":" << s.tick << ",";
  out << "\"w\":" << s.w << ",";
  out << "\"h\":" << s.h << ",";
  out << "\"foods\":";
  append_vec2_array(out, s.foods);
  out << ",";
  out << "\"snakes\":[";
  for (size_t i = 0; i < s.snakes.size(); ++i) {
    const auto& snake = s.snakes[i];
    out << "{";
    out << "\"id\":" << snake.id << ",";
    out << "\"user_id\":" << snake.user_id << ",";
    out << "\"color\":\"" << json_escape(snake.color) << "\",";
    out << "\"dir\":" << snake.dir << ",";
    out << "\"paused\":" << (snake.paused ? "true" : "false") << ",";
    out << "\"body\":";
    append_vec2_array(out, snake.body);
    out << "}";
    if (i + 1 < s.snakes.size()) out << ",";
  }
  out << "]";
  out << "}";
  return out.str();
}

}  // namespace protocol
