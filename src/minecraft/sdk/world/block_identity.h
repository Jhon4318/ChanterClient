#pragma once

#include <cstdint>
#include <string>

namespace minecraft::sdk {

namespace block_identity {

// Reads Block::mBlockType and BlockType::NameInfo::mFullName for Bedrock 1.26.60.
// Returns false on an invalid pointer, unexpected string layout, or access violation.
// This is intentionally read-only: an unrecognized layout simply disables click matching.
bool try_get_type_name(const void* block, std::string& out);

}
}
