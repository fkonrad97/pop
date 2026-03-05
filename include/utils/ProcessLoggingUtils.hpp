#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <streambuf>

namespace md::logging {
    struct ProcessLogSession {
        std::string path;
        std::unique_ptr<std::ostream> stream_owner;
        std::unique_ptr<std::streambuf> log_buf;
    };

    std::optional<ProcessLogSession> enable_process_file_logging(const std::string &base_path);
}
