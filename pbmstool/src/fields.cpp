#include "fields.h"
#include <cstdio>
#include <initializer_list>

namespace pace {

namespace {

// Same formatting as `std::cout << std::fixed << std::setprecision(prec)`.
std::string fixed(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

std::string idx_key(const char* prefix, int idx, const char* suffix) {
    return prefix + std::to_string(idx) + suffix;
}

struct BitDef { uint8_t mask; const char* name; };

void add_bits(KeyValues& out, uint8_t val, std::initializer_list<BitDef> defs) {
    for (const auto& d : defs)
        out.emplace_back(d.name, (val & d.mask) ? "1" : "0");
}

} // namespace

KeyValues analog_fields(uint8_t pack, const AnalogData& a) {
    KeyValues out;
    out.emplace_back("pack", std::to_string(static_cast<int>(pack)));
    out.emplace_back("cell_count", std::to_string(a.cell_count));
    for (int k = 0; k < a.cell_count; ++k)
        out.emplace_back(idx_key("cell_", k + 1, "_mv"),
                         std::to_string(static_cast<int>(a.cell_mv[k])));
    out.emplace_back("temp_count", std::to_string(a.temp_count));
    for (int k = 0; k < a.temp_count; ++k) {
        double tc = a.temp_raw[k] / 10.0 - 273.15;
        out.emplace_back(idx_key("temp_", k + 1, "_c"), fixed(tc, 1));
    }
    out.emplace_back("current_a",        fixed(a.current_10ma / 100.0, 2));
    out.emplace_back("total_voltage_mv", std::to_string(a.total_volt_mv));
    out.emplace_back("remain_cap_mah",   std::to_string(a.remain_cap_mah));
    out.emplace_back("full_cap_mah",     std::to_string(a.full_cap_mah));
    out.emplace_back("design_cap_mah",   std::to_string(a.design_cap_mah));
    out.emplace_back("cycle",            std::to_string(a.cycle));
    return out;
}

KeyValues alarm_fields(uint8_t pack, const AlarmData& al) {
    KeyValues out;
    out.emplace_back("pack", std::to_string(static_cast<int>(pack)));
    out.emplace_back("cell_count", std::to_string(al.cell_count));
    for (int k = 0; k < al.cell_count; ++k)
        out.emplace_back(idx_key("cell_", k + 1, "_volt_alarm"),
                         std::to_string(static_cast<int>(al.cell_volt_alarm[k])));
    out.emplace_back("temp_count", std::to_string(al.temp_count));
    for (int k = 0; k < al.temp_count; ++k)
        out.emplace_back(idx_key("temp_", k + 1, "_alarm"),
                         std::to_string(static_cast<int>(al.temp_alarm[k])));
    out.emplace_back("charge_curr_state",    std::to_string(static_cast<int>(al.charge_curr_state)));
    out.emplace_back("total_volt_state",     std::to_string(static_cast<int>(al.total_volt_state)));
    out.emplace_back("discharge_curr_state", std::to_string(static_cast<int>(al.discharge_curr_state)));

    // Status1: protection triggers
    add_bits(out, al.status[0], {
        {0x01, "protect_cell_overvolt"},
        {0x02, "protect_cell_undervolt"},
        {0x04, "protect_pack_overvolt"},
        {0x08, "protect_pack_undervolt"},
        {0x10, "protect_chg_overcurr"},
        {0x20, "protect_dchg_overcurr"},
        {0x40, "protect_short_circuit"},
        {0x80, "protect_chg_overvolt"},
    });
    // Status2: temperature protection triggers
    add_bits(out, al.status[1], {
        {0x01, "protect_chg_temp_high"},
        {0x02, "protect_chg_temp_low"},
        {0x04, "protect_dchg_temp_high"},
        {0x08, "protect_dchg_temp_low"},
        {0x10, "protect_env_temp_high"},
        {0x20, "protect_env_temp_low"},
        {0x40, "protect_mosfet_temp_high"},
        {0x80, "fully_charged"},
    });
    // Status3: system switch state
    add_bits(out, al.status[2], {
        {0x01, "current_limit_active"},
        {0x02, "charge_mos_on"},
        {0x04, "discharge_mos_on"},
        {0x10, "reverse_connect"},
        {0x20, "ac_input_present"},
        {0x80, "heater_active"},
    });
    // Status4: additional switch states
    add_bits(out, al.status[3], {
        {0x01, "buzzer_off"},
        {0x10, "current_limit_enabled"},
        {0x20, "light_alarm_active"},
    });
    // Status5: hardware faults
    add_bits(out, al.status[4], {
        {0x01, "fault_charge_mos"},
        {0x02, "fault_discharge_mos"},
        {0x04, "fault_temp_sensor"},
        {0x10, "fault_cell"},
        {0x20, "fault_sampling"},
        {0x40, "fault_eeprom"},
        {0x80, "fault_rtc"},
    });
    // Status6+7: per-cell active balancing (bit0=cell1..bit7=cell8, then cell9-16)
    for (int c = 0; c < al.cell_count; ++c) {
        int byte_idx = (c < 8) ? 5 : 6;
        int bit = c % 8;
        out.emplace_back(idx_key("cell_", c + 1, "_balancing"),
                         std::to_string((al.status[byte_idx] >> bit) & 1));
    }
    // Status8: soft alarm flags (warnings, non-tripping)
    add_bits(out, al.status[7], {
        {0x01, "alarm_cell_overvolt"},
        {0x02, "alarm_cell_undervolt"},
        {0x04, "alarm_pack_overvolt"},
        {0x08, "alarm_pack_undervolt"},
        {0x10, "alarm_chg_overcurr"},
        {0x20, "alarm_dchg_overcurr"},
    });
    // Status9: temperature alarm flags
    add_bits(out, al.status[8], {
        {0x01, "alarm_chg_temp_high"},
        {0x02, "alarm_chg_temp_low"},
        {0x04, "alarm_dchg_temp_high"},
        {0x08, "alarm_dchg_temp_low"},
        {0x10, "alarm_env_temp_high"},
        {0x20, "alarm_env_temp_low"},
        {0x40, "alarm_mosfet_temp_high"},
        {0x80, "alarm_mosfet_temp_prot"},
    });
    return out;
}

} // namespace pace
