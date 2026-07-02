#ifndef LSM_COMPACTION_H
#define LSM_COMPACTION_H

#include "common.h"
#include "manifest.h"

namespace lsm {

class Compaction {
public:
    // Run compaction to merge all Level 0 files and overlapping Level 1 files.
    // Creates new consolidated non-overlapping Level 1 files.
    static Status Run(const std::string& db_path, Manifest* manifest);
};

} // namespace lsm

#endif // LSM_COMPACTION_H
