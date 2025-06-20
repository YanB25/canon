#include <numa.h>

#include <thread>

#include "DSM.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Numa.h"

[[maybe_unused]] constexpr uint16_t kClientNodeId = 0;
[[maybe_unused]] constexpr uint16_t kServerNodeId = 1;

#include "util/gflags_def.h"
DEFINE_string(msg, "hello workd", "the message");
struct Person
{
    std::string name;
    int age;
};

std::ostream &operator<<(std::ostream &os, const Person &p)
{
    os << "{Person " << p.name << " age: " << p.age << "}";
    return os;
};

struct U
{
    template <typename T>
    operator T()
    {
        return {};
    }
};

struct Obj
{
    int a;
    Obj(int a_) : a(a_)
    {
    }
    Obj() : a(0)
    {
    }
};
std::ostream &operator<<(std::ostream &os, const Obj &o)
{
    os << o.a;
    return os;
}
// control the output

struct A
{
    int m_{10};
};
std::ostream &operator<<(std::ostream &os, const util::pre_impl<A> &a)
{
    std::ignore = a;
    os << "{A}";
    return os;
}
struct B
{
    A a_;
};

std::ostream &operator<<(std::ostream &os, const util::pre_impl<B> &b)
{
    os << "{B " << util::pre(b.inner().a_, b.ctx()) << "}";
    return os;
}
struct C
{
    B b_;
};
std::ostream &operator<<(std::ostream &os, const util::pre_impl<C> &c)
{
    os << "{C " << util::pre(c.inner().b_, c.ctx()) << "}";
    return os;
}
struct A2
{
};
struct B2
{
    A2 a_;
};
struct C2
{
    B2 b_;
};

int main(int argc, char *argv[])
{
    // google::InitGoogleLogging(argv[0]);
    // gflags::ParseCommandLineFlags(&argc, &argv, true);
    init_gflags(argc, argv);

    std::vector<int> vec{1, 2, 3, 4, 5};
    std::cout << util::pre(vec) << std::endl;

    std::cout << util::pre(vec, 2, 100, true) << std::endl;
    std::cout << util::pre(vec, 0, 100, false) << std::endl;

    std::cout << util::pre(5) << std::endl;
    Person p{.name = "Alice", .age = 20};
    std::cout << util::pre(p) << std::endl;
    // {Person Alice age: 20}

    std::vector<Person> persons{
        Person{.name = "A", .age = 25},
        Person{.name = "B", .age = 30},
        Person{.name = "C", .age = 45},
    };
    std::cout << util::pre(persons) << std::endl;
    // [{Person A age: 25}, {Person B age: 30}, {Person C age: 45}]
    std::cout << util::pre(persons, 1, 100, false) << std::endl;
    // [{Person A age: 25}, ...]

    std::map<std::string, Person> m;
    m["boss"] = Person{.name = "A", .age = 70};
    m["employee"] = Person{.name = "B", .age = 30};
    m["employee2"] = Person{.name = "C", .age = 30};
    m["employee4"] = Person{.name = "D", .age = 30};
    std::cout << "map: " << util::pre(m) << std::endl;
    std::cout << "map: " << util::pre(m, 0, 100, false) << std::endl;
    std::cout << "map: " << util::pre(m, 1, 100, false) << std::endl;
    std::cout << "map: " << util::pre(m, 2, 100, false) << std::endl;
    // {("boss", {Person A age: 70}), ("employee", {Person B age: 30})}

    std::vector<std::vector<Person>> matrix;
    matrix.push_back(
        {Person{.name = "A", .age = 10}, Person{.name = "B", .age = 20}});
    matrix.push_back(
        {Person{.name = "C", .age = 30}, Person{.name = "D", .age = 40}});
    std::cout << "matrix: " << util::pre(matrix) << std::endl;
    // [[{Person A age: 10}, {Person B age: 20}], [{Person C age: 30}, {Person D
    // age: 40}]]
    std::cout << "matrix(3): " << util::pre(matrix, 100, 3) << std::endl;
    // matrix(3): [[{Person A age: 10}, {Person B age: 20}], [{Person C age:
    // 30}, {Person D age: 40}]]
    std::cout << "matrix(2): " << util::pre(matrix, 100, 2) << std::endl;
    // matrix(2): [[..., ...], [..., ...]]
    std::cout << "matrix(1): " << util::pre(matrix, 100, 1) << std::endl;
    // matrix(1): [..., ...]
    std::cout << "matrix(0): " << util::pre(matrix, 100, 0) << std::endl;

    std::pair<int, int> pair{100, 200};
    std::cout << "pair: " << util::pre(pair) << std::endl;
    // pair: (100, 200)

    std::tuple<Person, int, int, int> t{
        Person{.name = "tuple", .age = 4}, 2, 3, 4};
    std::cout << "tuple: " << util::pre(t) << std::endl;
    // tuple: <{Person tuple age: 4}, 2, 3, 4>

    std::forward_list<Person> fl;
    fl.push_front(Person{.name = "A", .age = 1});
    fl.push_front(Person{.name = "B", .age = 1});
    fl.push_front(Person{.name = "C", .age = 1});
    std::cout << "forward_list: " << util::pre(fl) << std::endl;
    // forward_list: [{Person B age: 1}, {Person A age: 1}]
    std::cout << "forward_list: " << util::pre(fl, 1, 100, false) << std::endl;
    // forward_list(omit): [{Person B age: 1}, ...]
    std::cout << "forward_list: " << util::pre(fl, 2, 100, false) << std::endl;
    // forward_list: [{Person C age: 1}, {Person B age: 1}, ...]
    std::cout << "forward_list: " << util::pre(fl, 0, 100, false) << std::endl;
    // forward_list: [...]

    int c_array[] = {2, 4, 6, 8, 10};
    std::cout << "c style array: " << util::pre(c_array) << std::endl;
    // c style array: [2, 4, 6, 8, 10]
    std::cout << "c style array: " << util::pre(c_array, 1, 100, false)
              << std::endl;
    // c style array: [2, ...]
    std::cout << "c style array: " << util::pre(c_array, 2, 100, false)
              << std::endl;
    // c style array: [2, ..., 8]
    std::cout << "c style array: " << util::pre(c_array, 0, 100, false)
              << std::endl;
    // c style array: [...]

    std::cout << "c style string: " << util::pre("hello world") << std::endl;
    // c style string: "hello world"
    std::cout << "c style ptr: " << util::pre((void *) 1024) << std::endl;
    // c style ptr: 0x400
    std::cout << "char: " << util::pre('a') << std::endl;
    // char: 'a'
    std::cout << "boolean: " << util::pre(true) << std::endl;
    // boolean: true

    {
        struct S
        {
            int x;
            std::string s;
            std::vector<int> v;
            std::map<int, int> m;
            std::atomic<int> a;
            bool b;
        };
        S s = {42, "42", {1, 2, 3}, {{5, 2}, {7, 11}}, 10, false};
        std::cout << util::pre(s) << std::endl;
        // {Unknown <42, "42", [1, 2, 3], {(5, 2), (7, 11)}, atomic(10), false>}
    }

    {
        std::queue<int> q;
        for (size_t i = 0; i < 4; ++i)
        {
            q.push(i);
        }
        std::cout << util::pre(q) << std::endl;
    }

    std::list<int> ls;
    LOG(INFO) << util::pre(ls);
    for (size_t i = 0; i < 20; ++i)
    {
        ls.push_back(2 * i);
        LOG(INFO) << util::pre(ls, 10);
        LOG(INFO) << util::pre(ls);
    }
    for (size_t i = 0; i < 10; ++i)
    {
        ls.pop_front();
        LOG(INFO) << util::pre(ls, 10);
        LOG(INFO) << util::pre(ls);
    }

    {
        std::pair<int, int> p{5, 2};
        LOG(INFO) << util::pre(p);
        LOG(ERROR) << "1: " << util::pre(p, 100, 1);
        LOG(ERROR) << "0: " << util::pre(p, 100, 0);
    }

    std::map<int, int> umap_of_p;
    LOG(INFO) << util::pre(umap_of_p);
    for (size_t i = 0; i < 20; ++i)
    {
        umap_of_p.emplace(i, i * 2);
        LOG(INFO) << util::pre(umap_of_p);
    }

    for (size_t i = 0; i < 10; ++i)
    {
        umap_of_p.erase(i);
        LOG(INFO) << util::pre(umap_of_p);
    }
    LOG(WARNING) << "2: " << util::pre(umap_of_p, 100, 2);
    LOG(WARNING) << "1: " << util::pre(umap_of_p, 100, 1);
    LOG(WARNING) << "0: " << util::pre(umap_of_p, 100, 0);

    std::array<Obj, 10> arr{};
    LOG(INFO) << util::pre(arr);
    LOG(INFO) << util::pre(arr, 0, true);

    {
        std::forward_list<Obj> ls;
        LOG(INFO) << util::pre(ls);
        for (size_t i = 0; i < 3; ++i)
        {
            ls.push_front(Obj(i));
            LOG(INFO) << util::pre(ls);
        }
        LOG(INFO) << util::pre(ls, 0, true);
    }

    {
        std::tuple<int, int, int, int> t{1, 2, 3, 4};
        LOG(INFO) << util::pre(t);
    }

    {
        int c_array[]{1, 2, 3, 4};
        // auto c = util::c_style_array_view(c_array);
        LOG(INFO) << util::pre(c_array);
        LOG(INFO) << util::pre("hello world");
        LOG(INFO) << util::pre((void *) 100);
        LOG(INFO) << util::pre((int *) 100);
        LOG(INFO) << util::pre('a');
        LOG(INFO) << util::pre(true);
        LOG(INFO) << util::pre(false);
        LOG(INFO) << util::pre(std::string("hello world2"));
    }

    auto pa = std::make_shared<int>(5);
    LOG(INFO) << util::pre(pa);
    LOG(INFO) << util::pre(std::nullopt);
    LOG(INFO) << util::pre(std::optional<int>(5));
    LOG(INFO) << util::pre(std::atomic<int>(5));

    {
        int array[2][2][3] = {{{1, 2, 3}, {4, 5, 6}}, {{1, 2, 3}, {4, 5, 6}}};
        LOG(INFO) << util::pre(array);
        LOG(INFO) << "4: " << util::pre(array, 10, 4, false);
        LOG(INFO) << "3: " << util::pre(array, 10, 3, false);
        LOG(INFO) << "2: " << util::pre(array, 10, 2, false);
        LOG(INFO) << "1: " << util::pre(array, 10, 1, false);
        LOG(INFO) << "0: " << util::pre(array, 10, 0, false);

        LOG(INFO) << util::pre(array, 2, 10, false);
        LOG(INFO) << util::pre(array, 1, 10, false);
        LOG(INFO) << util::pre(array, 0, 10, false);
    }

    LOG(INFO) << util::pre(std::atomic<bool>(true));

    {
        std::queue<std::queue<int>> qs;
        for (size_t i = 0; i < 4; ++i)
        {
            std::queue<int> q;
            for (size_t j = 10; j < 14; ++j)
            {
                q.push(j);
            }
            qs.push(q);
        }
        LOG(INFO) << util::pre(qs);
        LOG(ERROR) << "3: " << util::pre(qs, 100, 3);
        LOG(ERROR) << "2: " << util::pre(qs, 100, 2);
        LOG(ERROR) << "1: " << util::pre(qs, 100, 1);
        LOG(ERROR) << "0: " << util::pre(qs, 100, 0);
    }
    {
        std::stack<int> s;
        for (size_t i = 0; i < 4; ++i)
        {
            s.push(i);
        }
        LOG(INFO) << util::pre(s);
    }
    {
        std::priority_queue<int> qp;
        for (size_t i = 0; i < 4; ++i)
        {
            qp.push(i);
        }
        LOG(INFO) << util::pre(qp);

        auto string = util::pre_str(qp);
        LOG(INFO) << util::pre(string);
    }
    {
        struct S
        {
            int x;
            std::string s;
            std::vector<int> v;
            std::map<int, int> m;
            std::atomic<int> a;
            bool b;
        };
        S s = {42, "42", {1, 2, 3}, {{5, 2}, {7, 11}}, 10, false};
        LOG(INFO) << util::pre(s);

        // const auto &[a, b, c, d, e, f] = s;
        // auto tuple = std::forward_as_tuple(a, b, c, d, e, f);

        // need count<S> to return 6
        // How?
        // count<S> generate
        // count_r<S, 1>
        // count_r<S, 1, 2>
        // count_r<S, 1, 2, 3>
        // ...
        // count_r<S, 1, 2, 3, ..., sizeof(S)>

        // Only one of them, i.e., count_r<S, 1, 2, 3, 4, 5, 6> is valid, other
        // fails.
        // In count_r<S, 1, 2, 3, 4, 5, 6>, return sizeof...(Ubiqs);

        // Is count_r<S, 1, 2, 3, 4> succeeded?
        // => Is S{Ubiqs{}, Ubiqs{}, Ubiqs{}, Ubiqus{}} valid?

        // Why Ubiqs{} is any type? operator T

        // count_r<T, 1, 2, 3, 4, 5, 6, 7, ..., 32>
        // count_r<T, 1, 2, 3, 4, 5, 6, 7, ..., 32> -> std::void_t<>
        // a) count_r<T, 2, 3, 4, 5, 6, 7, ..., 32>
        //    a) count_r<T, 3, ..., 32>
        //         a) count_r<T, ..., 27, 28, 29, 30, 31, 32>
        //             b) count_r<T, ..., 28, 29, 30, 31, 32>
        // b) count_r<T, ..., 28, 29, 30, 31, 32>
    }

    {
        U u;
        int a = u;
        LOG(INFO) << a;
    }
    {
        std::multiset<int> ms;
        for (size_t i = 0; i < 3; ++i)
        {
            ms.insert(i);
            ms.insert(i);
            ms.insert(i);
        }
        LOG(INFO) << util::pre(ms);
    }
    {
        std::multimap<int, int> mm;
        for (size_t i = 0; i < 3; ++i)
        {
            mm.emplace(i, 2 * i);
            mm.emplace(i, 2 * i);
            mm.emplace(i, 2 * i);
        }
        LOG(INFO) << util::pre(mm);
    }
    {
        std::unordered_multimap<int, int> o_mm;
        for (size_t i = 0; i < 3; ++i)
        {
            o_mm.emplace(i, 2 * i);
            o_mm.emplace(i, 2 * i);
            o_mm.emplace(i, 2 * i);
        }
        LOG(INFO) << util::pre(o_mm);
    }
    {
        std::unordered_multiset<int> o_mm;
        for (size_t i = 0; i < 3; ++i)
        {
            o_mm.emplace(i);
            o_mm.emplace(i);
            o_mm.emplace(i);
        }
        LOG(INFO) << util::pre(o_mm);
    }
    {
        C c;
        LOG(INFO) << util::pre(c);
        LOG(INFO) << "3: " << util::pre(c, 10, 3);
        LOG(INFO) << "2: " << util::pre(c, 10, 2);
        LOG(INFO) << "1: " << util::pre(c, 10, 1);
        LOG(INFO) << "0: " << util::pre(c, 10, 0);
    }
    {
        C2 c;
        LOG(INFO) << util::pre(c);
        LOG(INFO) << "1: " << util::pre(c, 10, 2);
        LOG(INFO) << "1: " << util::pre(c, 10, 1);
        LOG(INFO) << "0: " << util::pre(c, 10, 0);
    }

    LOG(INFO) << "PASS.";
}