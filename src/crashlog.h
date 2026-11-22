// A record of a crash in X-Plane's process, for finding what took it down: on Windows the faulting code's place and the call stack
// (each frame as module+offset, read later against the build's PDB), written to a file as it happens; and an uncaught C++
// exception, an abort, a pure virtual call or a C runtime call given a bad argument in XPTablet's own code. The next start puts the
// record in X-Plane's Log.txt. Elsewhere it does nothing.
#pragma once
#include <string>

namespace xpt {
namespace crashlog {

void install(const std::string& dir);   // dir: Output/XPTablet/ (ends in a slash); the record is crash.txt in it
void uninstall();
// what the last session recorded, if anything (and moves the file aside, so it is told once)
std::string takeLast(const std::string& dir);

}   // namespace crashlog
}   // namespace xpt
