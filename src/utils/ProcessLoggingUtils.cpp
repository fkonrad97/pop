#include "utils/ProcessLoggingUtils.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string_view>

namespace md::logging
{
    namespace
    {
        std::string normalize_log_path(std::string path)
        {
            constexpr std::string_view suffix = ".log";
            const std::filesystem::path input(path);
            std::filesystem::path parent = input.parent_path();
            std::string stem = input.filename().string();

            if (stem.empty())
            {
                stem = "pop";
            }

            if (stem.size() >= suffix.size() && std::string_view(stem).substr(stem.size() - suffix.size()) == suffix)
            {
                stem.resize(stem.size() - suffix.size());
            }

            const auto now = std::chrono::system_clock::now();
            const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
#if defined(_WIN32)
            localtime_s(&tm_buf, &now_time);
#else
            localtime_r(&now_time, &tm_buf);
#endif

            std::ostringstream ts;
            ts << std::put_time(&tm_buf, "%Y%m%d-%H%M%S");

            const std::filesystem::path output = parent / (stem + "-" + ts.str() + ".log");
            return output.string();
        }

        std::string make_prefix()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
#if defined(_WIN32)
            localtime_s(&tm_buf, &now_time);
#else
            localtime_r(&now_time, &tm_buf);
#endif
            std::ostringstream oss;
            oss << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "] ";
            return oss.str();
        }
    }

    class TimestampingStreambuf final : public std::streambuf
    {
    public:
        explicit TimestampingStreambuf(std::streambuf *dest) : dest_(dest) {}

    protected:
        int overflow(int ch) override
        {
            if (ch == traits_type::eof())
                return sync() == 0 ? 0 : traits_type::eof();

            if (at_line_start_)
            {
                const std::string prefix = make_prefix();
                if (dest_->sputn(prefix.data(), static_cast<std::streamsize>(prefix.size())) !=
                    static_cast<std::streamsize>(prefix.size()))
                {
                    return traits_type::eof();
                }
                at_line_start_ = false;
            }

            if (dest_->sputc(static_cast<char>(ch)) == traits_type::eof())
            {
                return traits_type::eof();
            }

            if (ch == '\n')
            {
                at_line_start_ = true;
            }

            return ch;
        }

        int sync() override
        {
            return dest_->pubsync();
        }

    private:
        std::streambuf *dest_;
        bool at_line_start_{true};
    };

    std::optional<ProcessLogSession> enable_process_file_logging(const std::string &base_path)
    {
        const std::string normalized_log_path = normalize_log_path(base_path);

        try
        {
            const std::filesystem::path p(normalized_log_path);
            if (p.has_parent_path())
            {
                std::filesystem::create_directories(p.parent_path());
            }

            auto log_stream = std::make_unique<std::ofstream>(normalized_log_path, std::ios::out | std::ios::app);
            if (!log_stream->is_open())
            {
                return std::nullopt;
            }

            auto log_buf = std::make_unique<TimestampingStreambuf>(log_stream->rdbuf());
            std::cerr.rdbuf(log_buf.get());
            std::cout.rdbuf(log_buf.get());

            ProcessLogSession session;
            session.path = normalized_log_path;
            session.stream_owner = std::move(log_stream);
            session.log_buf = std::move(log_buf);
            return session;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
