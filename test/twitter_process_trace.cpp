// handling from https://github.com/twitter/cache-trace
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util/gflags_dec.h"
#include "util/gflags_def.h"

using namespace std;

constexpr int kClient = 128;

struct Item
{
    uint32_t key_size;
    uint32_t value_size;
    std::string key;
    uint8_t is_set;
};

std::vector<Item> client_vector[kClient];

uint32_t maxKeySize = 0;
uint32_t maxValueSize = 0;
uint64_t requestCnt = 0;
uint64_t setCnt = 0;

void handle_trace(const string &filename)
{
    ifstream file(filename);
    string line;
    while (getline(file, line))
    {
        stringstream ss(line);
        string cell;

        uint64_t ts;
        uint64_t client_id;
        string op;

        Item item;

        requestCnt++;

        // timestamp
        getline(ss, cell, ',');
        ts = stoull(cell);

        // anonymized key
        getline(ss, item.key, ',');

        // key size
        getline(ss, cell, ',');
        item.key_size = stoull(cell);
        assert(item.key.size() == item.key_size);

        maxKeySize = std::max(item.key_size, maxKeySize);

        // value size
        getline(ss, cell, ',');
        item.value_size = stoull(cell);
        maxValueSize = std::max(item.value_size, maxValueSize);

        if (item.value_size + item.key_size > 4000)
        {  // max value size
            item.value_size = 4000 - item.key_size;
        }

        // client id
        getline(ss, cell, ',');
        client_id = stoull(cell);

        // operation
        getline(ss, cell, ',');
        op.swap(cell);
        item.is_set = op[0] == 's';
        if (item.is_set)
        {
            setCnt++;
        }

        // TTL
        getline(ss, cell, ',');

        client_vector[requestCnt % kClient].push_back(std::move(item));
    }

    std::cout << "count: " << requestCnt << std::endl;
    std::cout << "maxKey: " << maxKeySize << std::endl;
    std::cout << "maxValue: " << maxValueSize << std::endl;
    std::cout << "set ratio: " << setCnt * 1.0 / requestCnt << std::endl;

    file.close();
}

void generate_files(const string &filename)
{
    for (size_t k = 0; k < kClient; ++k)
    {
        auto &v = client_vector[k];
        std::string file_name = filename + "-" + std::to_string(k);
        ofstream outFile(file_name, ios::out | ios::binary);

        uint64_t op_cnt = v.size();
        uint64_t set_cnt = 0;
        outFile.write((char *) (&op_cnt), 8);
        for (size_t i = 0; i < op_cnt; ++i)
        {
            outFile.write((char *) (&v[i].key_size), sizeof(v[i].key_size));
            outFile.write((char *) (&v[i].value_size), sizeof(v[i].value_size));
            outFile.write((char *) (&v[i].is_set), sizeof(v[i].is_set));
            outFile.write(v[i].key.c_str(), v[i].key_size);
            if (v[i].is_set)
            {
                set_cnt++;
            }
        }

        outFile.close();
        std::cout << "client" << k << ": " << op_cnt << " "
                  << set_cnt * 1.0 / op_cnt << std::endl;

        v.clear();
    }
}

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    if (argc < 2)
    {
        LOG(FATAL) << argv[0] << " <filename> " << std::endl;
        exit(22);
    }
    for (size_t i = 1; i < argc; ++i)
    {
        cout << "----------------" << argv[i] << "------------------" << endl;
        handle_trace(argv[i]);
        generate_files(argv[i]);
    }

    return 0;
}
