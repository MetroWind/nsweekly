#include <memory>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "config.hpp"
#include "game_import.hpp"

int main(int argc, char** argv)
{
    cxxopts::Options cmd_options("NS Weekly", "Naively simple weekly snippet");
    cmd_options.add_options()
        ("c,config", "Config file",
         cxxopts::value<std::string>()->default_value("/etc/nsweekly.yaml"))
        ("import-games-csv", "Import Tracker CSV and exit",
         cxxopts::value<std::string>())
        ("import-games-user", "Existing account to import into",
         cxxopts::value<std::string>())
        ("import-reviews-csv", "Import Reviews CSV and exit",
         cxxopts::value<std::string>())
        ("import-reviews-user", "Existing account to import reviews into",
         cxxopts::value<std::string>())
        ("h,help", "Print this message.");
    cxxopts::ParseResult opts;
    try
    {
        opts = cmd_options.parse(argc, argv);
    }
    catch(const cxxopts::exceptions::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }

    if(opts.count("help"))
    {
        std::cout << cmd_options.help() << std::endl;
        return 0;
    }

    if(bool(opts.count("import-games-csv")) !=
           bool(opts.count("import-games-user")) ||
       bool(opts.count("import-reviews-csv")) !=
           bool(opts.count("import-reviews-user")))
    {
        std::cerr << "Both CSV and user arguments are required together\n";
        return 2;
    }
    if(opts.count("import-games-csv") && opts.count("import-reviews-csv"))
    {
        std::cerr << "Import Tracker and Reviews separately\n";
        return 2;
    }

    const std::string config_file = opts["config"].as<std::string>();

    auto conf = Configuration::fromYaml(std::move(config_file));
    if(!conf.has_value())
    {
        spdlog::error("Failed to load configuration: {}", errorMsg(conf.error()));
        return 3;
    }

    if(opts.count("import-games-csv"))
    {
        return runGameImport(*conf, opts["import-games-csv"].as<std::string>(),
            opts["import-games-user"].as<std::string>(), std::cout);
    }

    if(opts.count("import-reviews-csv"))
    {
        return runGameImport(*conf, opts["import-reviews-csv"].as<std::string>(),
            opts["import-reviews-user"].as<std::string>(), std::cout,
            GameImportKind::REVIEWS);
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
