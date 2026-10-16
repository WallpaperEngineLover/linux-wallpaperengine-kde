#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>

#include "WallpaperEngine/Desktop/PowerMonitor.h"

using WallpaperEngine::Desktop::sysfsOnBattery;

namespace {
struct PowerSupplies {
    std::filesystem::path path
	= std::filesystem::temp_directory_path () / ("lwe-power-test-" + std::to_string (getpid ()));

    PowerSupplies () { std::filesystem::create_directories (path); }
    ~PowerSupplies () { std::filesystem::remove_all (path); }

    void supply (const std::string& name, const std::map<std::string, std::string>& files) const {
	std::filesystem::create_directories (path / name);

	for (const auto& [file, value] : files) {
	    std::ofstream (path / name / file) << value << "\n";
	}
    }
};
} // namespace

TEST_CASE ("A desktop without a battery has no answer") {
    PowerSupplies supplies;

    CHECK_FALSE (sysfsOnBattery (supplies.path).has_value ());

    supplies.supply ("hidpp_battery_0", { { "type", "Battery" }, { "scope", "Device" }, { "status", "Discharging" } });

    CHECK_FALSE (sysfsOnBattery (supplies.path).has_value ());
}

TEST_CASE ("A discharging laptop battery is battery power") {
    PowerSupplies supplies;
    supplies.supply ("AC", { { "type", "Mains" }, { "online", "0" } });
    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Discharging" } });

    CHECK (sysfsOnBattery (supplies.path) == true);
}

TEST_CASE ("Charging, full or plugged in is not battery power") {
    PowerSupplies supplies;
    supplies.supply ("AC", { { "type", "Mains" }, { "online", "0" } });
    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Charging" } });

    CHECK (sysfsOnBattery (supplies.path) == false);

    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Full" } });

    CHECK (sysfsOnBattery (supplies.path) == false);

    // a weak USB-C charger can still leave the battery discharging
    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Discharging" } });
    supplies.supply ("ucsi-source-psy-USBC000:001", { { "type", "USB" }, { "online", "1" } });

    CHECK (sysfsOnBattery (supplies.path) == false);
}

TEST_CASE ("Peripheral batteries are ignored") {
    PowerSupplies supplies;
    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Charging" } });
    supplies.supply ("hidpp_battery_0", { { "type", "Battery" }, { "scope", "Device" }, { "status", "Discharging" } });
    supplies.supply ("hid-headset", { { "type", "USB" }, { "scope", "Device" }, { "online", "1" } });

    CHECK (sysfsOnBattery (supplies.path) == false);

    supplies.supply ("BAT0", { { "type", "Battery" }, { "status", "Discharging" } });

    CHECK (sysfsOnBattery (supplies.path) == true);
}
