#include <memory>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "config.hpp"

int main(int argc, char** argv)
{
    cxxopts::Options cmd_options("NS Weekly", "Naively simple weekly snippet");
    cmd_options.add_options()
        ("c,config", "Config file",
         cxxopts::value<std::string>()->default_value("/etc/nsweekly.yaml"))
        ("h,help", "Print this message.");
    auto opts = cmd_options.parse(argc, argv);

    if(opts.count("help"))
    {
        std::cout << cmd_options.help() << std::endl;
        return 0;
    }

    const std::string config_file = opts["config"].as<std::string>();

    auto conf = Configuration::fromYaml(std::move(config_file));
    if(!conf.has_value())
    {
        spdlog::error("Failed to load configuration: {}", errorMsg(conf.error()));
        return 3;
    }

    int exit_code = 0;
    auto app = App::create(*conf, exit_code);
    if(!app.has_value())
    {
        spdlog::error("Failed to initialize service: {}", errorMsg(app.error()));
        return exit_code;
    }
    auto started = (*app)->start();
    if(!started.has_value())
    {
        spdlog::error("Failed to start service: {}",
                      mw::errorMsg(started.error()));
        return 5;
    }
    (*app)->wait();

    return 0;
}
