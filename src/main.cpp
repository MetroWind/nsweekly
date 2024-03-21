#include <memory>

#include "app.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "data.hpp"
#include "http_client.hpp"
#include "spdlog/spdlog.h"
#include "utils.hpp"

int main()
{
    spdlog::set_level(spdlog::level::debug);
    Configuration conf;
    conf.client_id = "test";
    conf.client_secret = "7QHMYaQYj4UxpqlmYKUYYDiLoNRCSRSD";
    conf.listen_address = "localhost";
    conf.listen_port = 8123;
    conf.openid_url_prefix = "https://auth.xeno.darksair.org/realms/xeno";

    auto auth = AuthOpenIDConnect::create(
        conf, "http://localhost:8123/openid-redirect",
        std::make_unique<HTTPSession>());
    if(!auth.has_value())
    {
        spdlog::error("Failed to create authentication module: {}",
                      std::visit([](const auto& e) { return e.msg; },
                                 auth.error()));
        return 1;
    }
    auto data_source = DataSourceSqlite::newFromMemory();
    if(!data_source.has_value())
    {
        spdlog::error("Failed to create data source: {}",
                      errorMsg(data_source.error()));
        return 2;
    }

    App app(conf, *std::move(auth), *std::move(data_source));
    app.start();

    return 0;
}
