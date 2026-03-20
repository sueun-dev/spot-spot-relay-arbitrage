#include "kimp/core/dotenv.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace kimp {

namespace {

std::string trim_ascii(std::string_view value) {
    std::size_t start = 0;
    std::size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string unquote_env_value(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

} // namespace

bool load_dotenv_file(const std::filesystem::path& path, std::size_t& loaded_count) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.rfind("export ", 0) == 0) {
            trimmed = trim_ascii(trimmed.substr(7));
        }

        const std::size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trim_ascii(std::string_view(trimmed).substr(0, eq_pos));
        std::string value = trim_ascii(std::string_view(trimmed).substr(eq_pos + 1));
        if (key.empty()) {
            continue;
        }

        value = unquote_env_value(value);
        if (std::getenv(key.c_str()) == nullptr) {
            ::setenv(key.c_str(), value.c_str(), 0);
            ++loaded_count;
        }
    }

    return true;
}

std::optional<std::filesystem::path> find_dotenv_path(
    const std::optional<std::filesystem::path>& start_dir) {
    std::error_code ec;
    auto current = start_dir.value_or(std::filesystem::current_path(ec));
    if (ec) {
        return std::nullopt;
    }

    if (!current.empty() && !std::filesystem::is_directory(current, ec) && !ec) {
        current = current.parent_path();
    }

    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = current / ".env";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (!current.has_parent_path()) {
            break;
        }
        auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = std::move(parent);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> load_dotenv_if_present(
    std::size_t* loaded_count,
    std::ostream* log_stream,
    const std::optional<std::filesystem::path>& start_dir) {
    auto dotenv_path = find_dotenv_path(start_dir);
    if (!dotenv_path) {
        if (loaded_count) {
            *loaded_count = 0;
        }
        return std::nullopt;
    }

    std::size_t local_count = 0;
    if (!load_dotenv_file(*dotenv_path, local_count)) {
        if (loaded_count) {
            *loaded_count = 0;
        }
        return std::nullopt;
    }

    if (loaded_count) {
        *loaded_count = local_count;
    }
    if (log_stream) {
        (*log_stream) << "Loaded " << local_count << " env vars from "
                      << dotenv_path->string() << std::endl;
    }
    return dotenv_path;
}

} // namespace kimp
