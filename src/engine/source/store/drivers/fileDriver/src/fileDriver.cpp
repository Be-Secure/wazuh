#include "store/drivers/fileDriver.hpp"

#include <fmt/format.h>

namespace store
{
FileDriver::FileDriver(const std::filesystem::path& path, bool create)
{
    if (create)
    {
        if (!std::filesystem::create_directories(path))
        {
            throw std::runtime_error(
                fmt::format("[FileDriver] Cannot create [{}]", path.string()));
        }
    }
    else
    {
        // Check path validity
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error(
                fmt::format("[FileDriver] Path [{}] does not exist", path.string()));
        }
        if (!std::filesystem::is_directory(path))
        {
            throw std::runtime_error(
                fmt::format("[FileDriver] Path [{}] is not a directory", path.string()));
        }
    }

    m_path = path;
}

std::filesystem::path FileDriver::nameToPath(const base::Name& name) const
{
    std::filesystem::path path {m_path};
    for (const auto& part : name.parts())
    {
        path /= part;
    }

    return path;
}

std::optional<base::Error> FileDriver::del(const base::Name& name)
{
    std::optional<base::Error> error = std::nullopt;
    auto path = nameToPath(name);

    if (!std::filesystem::exists(path))
    {
        error = base::Error {
            fmt::format("[FileDriver::erase] File [{}] does not exist", path.string())};
    }
    else
    {

        std::error_code ec;
        if (!std::filesystem::remove_all(path, ec))
        {
            error = base::Error {fmt::format(
                "[FileDriver::erase] Could not remove file [{}] due to [{}:{}]",
                path.string(),
                ec.value(),
                ec.message())};
        }

        // Remove empty parent directories
        bool next = true;
        for (path = path.parent_path();
             next && path != m_path && std::filesystem::is_empty(path);
             path = path.parent_path())
        {
            if (!std::filesystem::remove(path, ec))
            {
                error = base::Error {
                    fmt::format("[FileDriver::erase] [{}] Was successfully removed "
                                "bu could not remove parent dir [{}] due to [{}:{}]",
                                name.fullName(),
                                path.string(),
                                ec.value(),
                                ec.message())};
                next = false;
            }
        }
    }
    return error;
}

std::optional<base::Error> FileDriver::add(const base::Name& name,
                                           const json::Json& content)
{
    std::optional<base::Error> error = std::nullopt;
    auto path = nameToPath(name);

    auto duplicateError = content.checkDuplicateKeys();
    if (duplicateError)
    {
        error = base::Error {fmt::format("[FileDriver::add] [{}] has duplicate keys: {}",
                                         name.fullName(),
                                         duplicateError.value().message)};
    }
    else if (std::filesystem::exists(path))
    {
        error = base::Error {
            fmt::format("[FileDriver::add] File [{}] already exists", path.string())};
    }
    else
    {
        std::error_code ec;
        if (!std::filesystem::create_directories(path.parent_path(), ec)
            && ec.value() != 0)
        {
            error = base::Error {fmt::format(
                "[FileDriver::add] Could not create directories [{}] due to [{}:{}]",
                path.parent_path().string(),
                ec.value(),
                ec.message())};
        }
        else
        {
            std::ofstream file(path);
            if (!file.is_open())
            {
                error = base::Error {
                    fmt::format("[FileDriver::add] Could not open file [{}] for writing",
                                path.string())};
            }
            else
            {
                file << content.str();
            }
        }
    }
    return error;
}

std::variant<json::Json, base::Error> FileDriver::get(const base::Name& name) const
{
    std::variant<json::Json, base::Error> result;
    auto path = nameToPath(name);

    if (std::filesystem::exists(path))
    {
        if (std::filesystem::is_directory(path))
        {
            json::Json list;
            list.setArray();
            for (const auto& entry : std::filesystem::directory_iterator(path))
            {
                list.appendString(fmt::format("{}{}{}",
                                              name.fullName(),
                                              base::Name::SEPARATOR_S,
                                              entry.path().filename().string()));
            }

            result = std::move(list);
        }
        else
        {
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content {buffer.str()};
            file.close();
            try
            {
                result = json::Json {content.c_str()};
            }
            catch (const std::exception& e)
            {
                result = base::Error {
                    fmt::format("[FileDriver] Could not parse file [{}] due to [{}]",
                                path.string(),
                                e.what())};
            }
        }
    }
    else
    {
        result = base::Error {
            fmt::format("[FileDriver] File [{}] does not exist", path.string())};
    }

    return result;
}

std::optional<base::Error> FileDriver::update(const base::Name& name,
                                              const json::Json& content)
{
    std::optional<base::Error> error = std::nullopt;
    auto path = nameToPath(name);

    auto duplicateError = content.checkDuplicateKeys();
    if (duplicateError)
    {
        error =
            base::Error {fmt::format("[FileDriver::update] [{}] has duplicate keys: {}",
                                     name.fullName(),
                                     duplicateError.value().message)};
    }
    else if (!std::filesystem::exists(path))
    {
        error = base::Error {
            fmt::format("[FileDriver::update] File [{}] does not exist", path.string())};
    }
    else
    {
        std::ofstream file(path);
        if (!file.is_open())
        {
            error = base::Error {
                fmt::format("[FileDriver::update] Could not open file [{}] for writing",
                            path.string())};
        }
        else
        {
            file << content.str();
        }
    }
    return error;
}

} // namespace store