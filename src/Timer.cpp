#include "Timer.h"

#include <glog/logging.h>

#include "fmt/core.h"

ContTimer<true>::ContTimer(const std::string &name, const std::string &step)
{
    init(name, step);
}
ContTimer<true>::ContTimer()
{
}

void ContTimer<true>::init(const std::string &name,
                           const std::string &first_step)
{
    clear();

    name_ = name;
    step_ = first_step;
    start_ = pin_ = std::chrono::steady_clock::now();
    inited_ = true;
}

void ContTimer<true>::pin(const std::string this_step)
{
    DCHECK(inited_);
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - pin_)
                  .count();
    std::string event = step_ + " => " + this_step;
    event_ns_[event] = ns;

    pin_ = now;
    step_ = this_step;
}
void ContTimer<true>::report(std::ostream &os) const
{
    DCHECK(inited_);
    auto now = std::chrono::steady_clock::now();
    auto total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_)
            .count();
    auto str = fmt::format("[{}]: *summary* takes {} ns ({} ms)\n",
                           name_,
                           total_ns,
                           total_ns / 1e6);
    for (const auto &[event, ns] : event_ns_)
    {
        auto fmt_str = fmt::format("{}% [{}] takes {} ns ({} ms)\n",
                                   100.0f * ns / total_ns,
                                   event,
                                   ns,
                                   ns / 1e6);
        str += fmt_str;
    }
    os << str << std::endl;
}

ContTimer<true>::~ContTimer()
{
    // pin("~Dtor()");
    // report();
}