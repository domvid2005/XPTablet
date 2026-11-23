// The computer's clipboard, as text (UTF-8): for pasting into the tablet's text boxes and copying out of them.
#pragma once
#include <string>

namespace xpt {
namespace clipboard {

bool get(std::string& text);        // false: nothing there as text (or no way to it here)
bool set(const std::string& text);
bool hasText();                     // cheap: there is text to paste (Windows asks without reading it)

}   // namespace clipboard
}   // namespace xpt
