#include "DSM.h"
#include "util/Page.h"
#include "util/gflags_def.h"

void test2(util::Page &&page)
{
    LOG(INFO) << "Test2: " << page;
}
void test(util::Page &&page)
{
    LOG(INFO) << "Test: " << page;
    test2(std::move(page));
}
int main()
{
    DSMConfig config;
    auto dsm_ = DSM::getInstance(config);
    dsm_->registerThread();

    {
        LOG(INFO) << "===== test basic";
        util::Page page(dsm_.get(), 4096);
        test(std::move(page));
    }

    {
        LOG(INFO) << "===== test ptr";
        util::Page page(dsm_.get(), 4096);
        auto ptr = std::make_shared<util::Page>(std::move(page));
    }
    {
        LOG(INFO) << "===== copy";
        util::Page page(dsm_.get(), 4096);
        auto ptr = std::make_shared<util::Page>(std::move(page));
        auto new_page = *ptr;
    }
    return 0;
}