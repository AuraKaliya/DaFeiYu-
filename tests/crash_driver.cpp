#include <dafeiyu/core.hpp>
#include <iostream>
using namespace dfy;
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    try {
        const std::string mode = argv[1], step = argv[2];
        auto profile = Profile::open(sdk::path_from_utf8(argv[3]));
        const auto state = sdk::path_from_utf8(argv[4]);
        if (mode == "action") {
            profile.data["plugins"][3]["manifest"] = "dfy_test_crash_action.json";
            profile.data["allowed_effects"] = Json::array({"local-write"});
        } else profile.data["plugins"][1]["manifest"] = "dfy_test_crash_channel.json";
        Runtime runtime(profile, state);
        if (step == "crash") { runtime.submit("crash", "crash recovery scenario"); throw std::runtime_error("fixture failed to terminate process"); }
        const auto run = runtime.resume("crash");
        if (mode == "action") {
            if (run.at("execution") != "waiting" || run.at("error") != "ACTION_EFFECT_UNKNOWN") throw std::runtime_error("write was not retained as unknown");
            Database effects(state / "plugins/test.crash-action/effects.sqlite3");
            auto count = effects.prepare("SELECT count(*) FROM effects"); count.step();
            if (count.column(0) != "1") throw std::runtime_error("write replayed");
        } else {
            if (run.at("delivery") != "sent" || run.at("execution") != "completed") throw std::runtime_error("receipt recovery failed");
            Database channel(state / "plugins/test.crash-channel/channel.sqlite3");
            auto count = channel.prepare("SELECT count(*) FROM deliveries"); count.step();
            if (count.column(0) != "1") throw std::runtime_error("delivery duplicated");
        }
        std::cout << "Recovered " << mode << " after hard process exit\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
