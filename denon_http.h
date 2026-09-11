#pragma once
// Tiny helper for pulling <Tag><value>...</value></Tag> fields out of the
// Denon AVR-3312CI's goform status XML (formZone2_Zone2XmlStatus.xml etc).
// Not a real XML parser - this receiver's output is fixed-format enough
// that a plain substring search is reliable and cheap on the ESP32.
#include <string>

inline std::string denon_xml_extract(const std::string &body, const std::string &tag) {
  std::string open_tag = "<" + tag + "><value>";
  std::string close_tag = "</value></" + tag + ">";
  auto p1 = body.find(open_tag);
  if (p1 == std::string::npos)
    return "";
  p1 += open_tag.size();
  auto p2 = body.find(close_tag, p1);
  if (p2 == std::string::npos)
    return "";
  std::string val = body.substr(p1, p2 - p1);
  // Denon pads some fields (notably Zone name) with trailing spaces
  while (!val.empty() && val.back() == ' ')
    val.pop_back();
  while (!val.empty() && val.front() == ' ')
    val.erase(val.begin());
  return val;
}
