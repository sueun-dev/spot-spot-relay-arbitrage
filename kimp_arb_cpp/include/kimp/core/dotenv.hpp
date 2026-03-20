#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>

namespace kimp {

bool load_dotenv_file(const std::filesystem::path& path, std::size_t& loaded_count);

std::optional<std::filesystem::path> find_dotenv_path(
    const std::optional<std::filesystem::path>& start_dir = std::nullopt);

std::optional<std::filesystem::path> load_dotenv_if_present(
    std::size_t* loaded_count = nullptr,
    std::ostream* log_stream = nullptr,
    const std::optional<std::filesystem::path>& start_dir = std::nullopt);

} // namespace kimp
