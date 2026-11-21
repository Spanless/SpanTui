#pragma once

#include <QString>

namespace span::core {

// Folder that holds the default data root (ProductPaths):
//   Windows: %LOCALAPPDATA%, from the environment variable;
//   macOS: ~/Library/Application Support, under $HOME.
QString userDataParentDirectory();

// Data root of older builds that ProductPaths moves into the default root
// once; empty where there is none.
QString legacyDefaultDataRoot();

} // namespace span::core
