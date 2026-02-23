#include "archive_path.h"

#include <system_error>

namespace satdump
{
    std::filesystem::path get_archive_base_path()
    {
        std::filesystem::path preferred = std::filesystem::path("files") / "images";
#ifdef __ANDROID__
        return preferred;
#else
        if (std::filesystem::exists(preferred))
            return preferred;
        return std::filesystem::path("images");
#endif
    }

    bool ensure_archive_base_path()
    {
        std::error_code ec;
        std::filesystem::create_directories(get_archive_base_path(), ec);
        return !ec;
    }
}
