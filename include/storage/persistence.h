/*
 * FlexQL: Snapshot persistence helpers.
 */

#ifndef FLEXQL_PERSISTENCE_H
#define FLEXQL_PERSISTENCE_H

#include "executor.h"
#include "storage.h"

#include <string>

namespace persistence {

bool loadSnapshot(const std::string &path,
                  StorageEngine &storage,
                  Executor &executor,
                  std::string &err);

bool saveSnapshot(const std::string &path,
                  const StorageEngine &storage,
                  std::string &err);

} // namespace persistence

#endif /* FLEXQL_PERSISTENCE_H */
