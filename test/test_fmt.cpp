#include <fmt/compile.h>
#include <fmt/core.h>
#include <fmt/ostream.h>
#include <fmt/printf.h>
#include <fmt/ranges.h>
#include <fmt/std.h>

#include <iostream>
#include <string>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Pre.h"
#include "util/gflags_def.h"
DEFINE_string(msg, "hello workd", "the message");

struct Person
{
    std::string name;
    int age;
};

std::ostream &operator<<(std::ostream &os, const Person &p)
{
    os << fmt::format("{{Person name: {}, age: {}}}", p.name, p.age);
    return os;
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    std::vector<int> vec{1, 2, 3};
    std::string s = fmt::format("vec is {}", vec);
    LOG(INFO) << s;

    Person p;
    p.name = "Bin Yan";
    p.age = 20;
    LOG(INFO) << fmt::format("{}", util::pre(p));
    LOG(INFO) << fmt::format("from a format string I got {}", PRE(p));

    // std::ios_base::fmtflags f;

    LOG(INFO) << "PASS.";
}