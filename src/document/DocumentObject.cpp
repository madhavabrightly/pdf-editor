#include "document/DocumentObject.h"

#include <atomic>

namespace rpfg {
namespace {

std::atomic<uint64_t> g_nextObjectId{1};

}  // namespace

DocumentObject::DocumentObject(ObjectType type)
    : type_(type), id_("obj_" + std::to_string(g_nextObjectId.fetch_add(1))) {}

}  // namespace rpfg
