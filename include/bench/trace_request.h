#include <iostream>
#include <string>

#include "./request.h"

namespace bench
{
class PercentileLoader
{
public:
    // input the filename of a CSV that
    // - does not have header
    // - format is <percentile>, <byte>
    PercentileLoader(const std::string &fn) : file_(fn)
    {
        std::ifstream ifs(file_);
        if (!ifs)
        {
            LOG(FATAL) << "** Failed to open " << fn;
        }
        std::string line;
        while (std::getline(ifs, line))
        {
            std::stringstream ss(line);
            std::string cell;
            getline(ss, cell, ',');
            double p = std::stod(cell);

            getline(ss, cell, ',');
            size_t val = std::stoull(cell);
            sizes_[p] = val;
        }

        LOG_IF(FATAL, sizes_.empty())
            << "** Failed to load " << fn << ": empty";
    }
    uint64_t next() const
    {
        double select = fast_pseudo_rand_dbl(0, 1);

        auto it = sizes_.lower_bound(select);
        if (it == sizes_.begin())
        {
            return min();
        }
        else
        {
            auto upper = it->second;
            it--;
            auto lower = it->second;
            return fast_pseudo_rand_int(lower, upper);
        }
    }
    bool has_next() const
    {
        return true;
    }
    size_t min() const
    {
        return sizes_.begin()->second;
    }
    size_t max() const
    {
        return (--sizes_.end())->second;
    }
    size_t median() const
    {
        auto it = sizes_.lower_bound(0.5);
        if (it == sizes_.end())
        {
            return 0;
        }
        return it->second;
    }
    auto &debug()
    {
        return sizes_;
    }

private:
    std::string file_;
    std::map<double, size_t> sizes_;
};

class CSVTraceGenerator : public IRequestGenerator
{
public:
    using Dist = std::pair<size_t, double>;
    struct Config
    {
        IRequestGenerator::Config config;
        std::string filename;
    };

    CSVTraceGenerator(const Config &config)
        : IRequestGenerator(config.config), c_(config), loader_(config.filename)
    {
    }
    size_t next_value_size() const override
    {
        return loader_.next();
    }

private:
    Config c_;
    PercentileLoader loader_;
};
inline std::ostream &operator<<(std::ostream &os,
                                const CSVTraceGenerator::Config &c)
{
    os << "{CSV " << c.filename << "}";
    return os;
}
}  // namespace bench