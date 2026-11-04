#pragma once
#include "FS.h"
// The watch's flash partition: none on the PC (the emulator's SD folder is used).
class FFatFS : public fs::FS {
public:
    bool begin(bool = false) { return false; }
    void end() {}
};
extern FFatFS FFat;
