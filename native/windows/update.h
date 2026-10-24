#pragma once

#include <functional>
#include <string>

namespace ss {

extern const char* kFeed; // the release the installers are published on

struct UpdateInfo {
  std::string version;
  std::string url;
};

/** a > b for dotted version numbers */
bool newerVersion(const std::string& a, const std::string& b);
/** Blocking (a worker thread): true and the newer version when there is one. */
bool checkForUpdate(const std::string& current, UpdateInfo& out);
/** Blocking: downloads the installer and starts it silently; the app should then quit. */
bool downloadAndInstall(const UpdateInfo& info, const std::function<void(int percent)>& progress);

}  // namespace ss
