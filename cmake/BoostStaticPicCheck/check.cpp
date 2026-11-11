#include <boost/filesystem.hpp>

bool check(const char * name) {
    try {
        return boost::filesystem::exists(boost::filesystem::path(name));
    } catch (const boost::filesystem::filesystem_error &) {
        return false;
    }
}
