#pragma once
#include "FS.h"
class SDFS : public fs::FS {
public:
    uint64_t cardSize() { return 0; }
    uint64_t totalBytes() { return 0; }
    uint64_t usedBytes() { return 0; }
};
extern SDFS SD;
