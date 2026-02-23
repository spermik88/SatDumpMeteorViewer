#pragma once

#include <filesystem>

namespace satdump
{
    std::filesystem::path get_archive_base_path();
    bool ensure_archive_base_path();
}
