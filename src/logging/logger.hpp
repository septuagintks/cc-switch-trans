#pragma once

#include "config/config.hpp"

#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace ccs {

struct LogField {
    std::string name;
    std::string value;
    bool quoted = true;
};

LogField field_string(std::string name, std::string value);
LogField field_number(std::string name, long long value);
LogField field_bool(std::string name, bool value);
LogField field_raw(std::string name, std::string raw_json);

class Logger {
public:
    explicit Logger(AppConfig config);

    bool open(std::string& error);
    void log(std::string level, std::string event, std::initializer_list<LogField> fields) const;
    void log(std::string level, std::string event, const std::vector<LogField>& fields) const;

private:
    AppConfig config_;
    mutable std::mutex mutex_;
    mutable std::ofstream file_;
};

} // namespace ccs
