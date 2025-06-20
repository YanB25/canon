#pragma once
#include <string>
#include <vector>

#include "DataFrame/DataFrame.h"
#include "bench/base_config.h"
#include "bench/config_factory.h"
#include "bench/result.h"
#include "glog/logging.h"
#include "util/DataFrameF.h"
#include "util/PerformanceReporter.h"
#include "util/Pre.h"
#include "util/Util.h"
#include "util/gflags_dec.h"

namespace bench
{
class ResultDataFrame
{
public:
    using StrDataFrame = hmdf::StdDataFrame<std::string>;
    ResultDataFrame()
    {
    }
    void reg_conf(const std::string &col_name, const std::string &value)
    {
        for (auto &[name, column] : user_str_data_)
        {
            if (name == col_name)
            {
                // hit
                column.push_back(value);
                return;
            }
        }
        // miss
        std::vector<std::string> new_str_vec;
        new_str_vec.push_back(value);
        // config is placed at very first
        user_str_data_.emplace_front(col_name, std::move(new_str_vec));
    }
    void reg_result(const std::string &col_name, uint64_t number)
    {
        for (auto &[name, column] : user_data_)
        {
            if (name == col_name)
            {
                // hit
                column.push_back(number);
                return;
            }
        }
        // miss
        std::vector<uint64_t> new_vec;
        new_vec.push_back(number);
        user_data_.emplace_back(col_name, std::move(new_vec));
    }
    void reg_result(const std::string &col_name, const std::string &value)
    {
        for (auto &[name, column] : user_str_data_)
        {
            if (name == col_name)
            {
                // hit
                column.push_back(value);
                return;
            }
        }
        // miss
        std::vector<std::string> new_str_vec;
        new_str_vec.push_back(value);
        user_str_data_.emplace_back(col_name, std::move(new_str_vec));
    }

    void reg_result(const ResultRecord &results, const IBenchConfig &config)
    {
        col_idx.push_back(config.name());
        col_machine_nr.push_back(FLAGS_machine_nr);
        col_x_thread_nr.push_back(config.thread_nr());
        col_x_coro_nr.push_back(config.coro_nr());

        // divide by 1000 to convert from ns to us
        auto &l = results.lat_ns;
        col_p50_us.push_back(l.p50 ? *l.p50 / 1000 : 0);
        col_p90_us.push_back(l.p90 ? *l.p90 / 1000 : 0);
        col_p99_us.push_back(l.p99 ? *l.p99 / 1000 : 0);
        col_p999_us.push_back(l.p999 ? *l.p999 / 1000 : 0);
        col_cluster_ops.push_back(results.cluster_ops);
        col_ops_variaty.push_back(results.variaty * 100);

        const auto &option_list = config.df_options();
        for (const auto &[key, value] : option_list)
        {
            reg_conf(key, value);
        }
    }

    template <typename V>
    void reg_result(const std::map<std::string, V> &m,
                    const IBenchConfig &config)
    {
        col_idx.push_back(config.name());
        col_machine_nr.push_back(FLAGS_machine_nr);
        col_x_thread_nr.push_back(config.thread_nr());
        col_x_coro_nr.push_back(config.coro_nr());

        const auto &option_list = config.df_options();
        for (const auto &[key, value] : option_list)
        {
            reg_conf(key, value);
        }
        for (const auto &[k, v] : m)
        {
            reg_result(k, v);
        }
    }

    void reg_latency(const ResultRecord &results, const IBenchConfig &config)
    {
        // for latency
        auto name =
            config.name() + "-" + std::to_string(config.effective_client_nr());

        if (unlikely(col_lat_idx.empty()))
        {
            col_lat_idx.push_back("lat_min");
            col_lat_idx.push_back("lat_p50");
            col_lat_idx.push_back("lat_p90");
            col_lat_idx.push_back("lat_p99");
            col_lat_idx.push_back("lat_p999");
            col_lat_idx.push_back("lat_max");
        }

        if (unlikely(lat_data_.count(name) != 0))
        {
            LOG(WARNING) << "[result] name " << util::pre(name)
                         << " already registered";
            return;
        }
        auto &l = results.lat_ns;
        lat_data_[name].push_back(l.min ? *l.min : 0);
        lat_data_[name].push_back(l.p50 ? *l.p50 : 0);
        lat_data_[name].push_back(l.p90 ? *l.p90 : 0);
        lat_data_[name].push_back(l.p99 ? *l.p99 : 0);
        lat_data_[name].push_back(l.p999 ? *l.p999 : 0);
        lat_data_[name].push_back(l.max ? *l.max : 0);
    }
    template <typename T>
    void reg_latency(const std::string &name, const OnePassBucketMonitor<T> &m)
    {
        if (unlikely(col_lat_idx.empty()))
        {
            col_lat_idx.push_back("lat_min");
            col_lat_idx.push_back("lat_p50");
            col_lat_idx.push_back("lat_p90");
            col_lat_idx.push_back("lat_p99");
            col_lat_idx.push_back("lat_p999");
            col_lat_idx.push_back("lat_max");
        }
        lat_data_[name].push_back(m.min());
        auto p50 = m.percentile(0.50);
        lat_data_[name].push_back(p50 ? *p50 : 0);
        auto p90 = m.percentile(0.90);
        lat_data_[name].push_back(p90 ? *p90 : 0);
        auto p99 = m.percentile(0.99);
        lat_data_[name].push_back(p99 ? *p99 : 0);
        auto p999 = m.percentile(0.999);
        lat_data_[name].push_back(p999 ? *p999 : 0);
        lat_data_[name].push_back(m.max());
    }

    void print()
    {
        if (!calculated_)
        {
            calculate();
        }
        if (has_tp_data_)
        {
            tp_df.write<std::ostream, std::string, uint64_t>(
                std::cout, hmdf::io_format::csv2);
        }
        if (has_lat_data_)
        {
            lat_df.write<std::ostream, std::string, uint64_t>(
                std::cout, hmdf::io_format::csv2);
        }
    }

    void dump(const std::string &binary, const std::string &exec_meta)
    {
        if (!calculated_)
        {
            calculate();
        }

        LOG_IF(INFO, FLAGS_no_csv)
            << "Skip dumping result to file: " << PRE(FLAGS_no_csv);

        if (has_tp_data_)
        {
            if (!FLAGS_no_csv)
            {
                auto filename = binary_to_csv_filename(binary, exec_meta);
                tp_df.write<std::string, uint64_t>(filename.c_str(),
                                                   hmdf::io_format::csv2);
                LOG(INFO) << "[DF] Write to " << PRE(filename);
            }

            tp_df.write<std::ostream, std::string, uint64_t>(
                std::cout, hmdf::io_format::csv2);
        }
        else
        {
            LOG(WARNING) << "[result] not throughput data recorded.";
        }
        if (has_lat_data_)
        {
            if (!FLAGS_no_csv)
            {
                std::map<std::string, std::string> info;
                info.emplace("kind", "lat");
                auto filename = binary_to_csv_filename(binary, exec_meta, info);
                lat_df.write<std::string, uint64_t>(filename.c_str(),
                                                    hmdf::io_format::csv2);
            }
            lat_df.write<std::ostream, std::string, uint64_t>(
                std::cout, hmdf::io_format::csv2);
        }
        else
        {
            LOG(WARNING) << "[result] not latency data recorded.";
        }
    }
    constexpr const char *machine_nr_column() const
    {
        return "machine";
    }
    constexpr const char *cluster_ops_column() const
    {
        return "ops(cluster)";
    }
    constexpr const char *ops_variaty_column() const
    {
        return "ops_variaty(percent)";
    }
    constexpr const char *thread_nr_column() const
    {
        return "x_thread_nr";
    }
    constexpr const char *coro_nr_column() const
    {
        return "x_coro_nr";
    }
    constexpr const char *client_nr_column() const
    {
        return "x_client_nr(local)";
    }
    constexpr const char *p50_us_column() const
    {
        return "p50(us)";
    }
    constexpr const char *p99_us_column() const
    {
        return "p99(us)";
    }

    void load_column(const std::string &name, std::vector<uint64_t> &&vec)
    {
        calculate();
        CHECK_EQ(vec.size(), tp_row_nr())
            << "Failed to load column " << util::pre(name) << ": size mismatch";
        return do_load_column(name, std::forward<std::vector<uint64_t>>(vec));
    }

    void product_columns(const std::string &target_name,
                         const std::string &lhs,
                         const std::string &rhs,
                         bool delete_old_cols = false)
    {
        calculate();
        return do_product_columns(target_name, lhs, rhs, delete_old_cols);
    }

    void divide_columns(const std::string &target_name,
                        const std::string &lhs,
                        const std::string &rhs,
                        bool delete_old_cols = false)
    {
        calculate();
        return do_divide_columns(target_name, lhs, rhs, delete_old_cols);
    }
    void scale_product_column(const std::string &target_name, uint64_t scale)
    {
        calculate();
        return do_scale_product_column(target_name, scale);
    }
    void scale_divide_column(const std::string &target_name, uint64_t scale)
    {
        calculate();
        return do_scale_divide_column(target_name, scale);
    }
    size_t tp_row_nr() const
    {
        return col_idx.size();
    }

private:
    // for throughput
    std::vector<std::string> col_idx;
    std::vector<uint64_t> col_machine_nr;
    std::vector<uint64_t> col_x_thread_nr;
    std::vector<uint64_t> col_x_coro_nr;
    std::vector<uint64_t> col_p50_us;
    std::vector<uint64_t> col_p90_us;
    std::vector<uint64_t> col_p99_us;
    std::vector<uint64_t> col_p999_us;
    std::vector<uint64_t> col_cluster_ops;
    std::vector<uint64_t> col_ops_variaty;

    // for latency
    std::vector<std::string> col_lat_idx;
    std::unordered_map<std::string, std::vector<uint64_t>> lat_data_;

    // for any user customized data
    // (name, column)
    using Data = std::pair<std::string, std::vector<uint64_t>>;
    std::list<Data> user_data_;
    using StrData = std::pair<std::string, std::vector<std::string>>;
    std::list<StrData> user_str_data_;

    bool calculated_{false};
    StrDataFrame tp_df;
    bool has_tp_data_{false};
    StrDataFrame lat_df;
    bool has_lat_data_{false};

    void calculate()
    {
        if (calculated_)
        {
            return;
        }

        if (!col_idx.empty())
        {
            has_tp_data_ = true;

            tp_df.load_index(std::move(col_idx));
            do_load_column(machine_nr_column(), std::move(col_machine_nr));
            do_load_column(thread_nr_column(), std::move(col_x_thread_nr));
            do_load_column(coro_nr_column(), std::move(col_x_coro_nr));

            // any user provided data
            for (auto &[name, column] : user_str_data_)
            {
                do_load_column(name, std::move(column));
            }
            user_str_data_.clear();
            for (auto &[name, column] : user_data_)
            {
                do_load_column(name, std::move(column));
            }
            user_data_.clear();

            // client_nr = thread_nr * coro_nr * machine_nr
            do_product_columns(client_nr_column(),
                               thread_nr_column(),
                               coro_nr_column(),
                               false);
            do_product_columns(client_nr_column(),
                               client_nr_column(),
                               machine_nr_column(),
                               false);

            do_load_column(p50_us_column(), std::move(col_p50_us));
            do_load_column(p99_us_column(), std::move(col_p99_us));

            do_load_column(cluster_ops_column(), std::move(col_cluster_ops));
            do_load_column(ops_variaty_column(), std::move(col_ops_variaty));
        }

        if (!col_lat_idx.empty())
        {
            has_lat_data_ = true;

            lat_df.load_index(std::move(col_lat_idx));
            for (auto &[name, vec] : lat_data_)
            {
                lat_df.load_column<uint64_t>(name.c_str(), std::move(vec));
            }

            std::map<std::string, std::string> info;
            info.emplace("kind", "lat");
        }
        calculated_ = true;
    }

    template <typename T>
    void do_load_column(const std::string &name, T &&vec)
    {
        using value_type = typename T::value_type;
        tp_df.load_column<value_type>(name.c_str(), std::forward<T>(vec));
    }

    void do_product_columns(const std::string &target_name,
                            const std::string &lhs,
                            const std::string &rhs,
                            bool delete_old_cols = false)
    {
        auto mul = hmdf::gen_F_mul<uint64_t, uint64_t, uint64_t>();
        tp_df.consolidate<uint64_t, uint64_t, uint64_t>(lhs.c_str(),
                                                        rhs.c_str(),
                                                        target_name.c_str(),
                                                        mul,
                                                        delete_old_cols);
    }
    void do_divide_columns(const std::string &target_name,
                           const std::string &lhs,
                           const std::string &rhs,
                           bool delete_old_cols = false)
    {
        auto div = hmdf::gen_F_div<uint64_t, uint64_t, uint64_t>();
        tp_df.consolidate<uint64_t, uint64_t, uint64_t>(lhs.c_str(),
                                                        rhs.c_str(),
                                                        target_name.c_str(),
                                                        div,
                                                        delete_old_cols);
    }
    void do_scale_product_column(const std::string &target_name, uint64_t scale)
    {
        auto prod = hmdf::gen_replace_F_mul<uint64_t>(scale);
        tp_df.replace<uint64_t>(target_name.c_str(), prod);
    }
    void do_scale_divide_column(const std::string &target_name, uint64_t scale)
    {
        auto div = hmdf::gen_replace_F_div<uint64_t>(scale);
        tp_df.replace<uint64_t>(target_name.c_str(), div);
    }
};

}  // namespace bench